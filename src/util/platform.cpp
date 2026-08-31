#include <silk/util/platform.h>

#include <silk/util/assert.h>
#include <silk/util/scope-guard.h>

#include <cerrno>
#include <cstdlib>

#include <fcntl.h>
#include <unistd.h>

namespace silk
{

/** Read the highest possible CPU id from the kernel's possible-cpu list and return that id plus one. */
static uint16_t readPossibleCpuCount() noexcept
{
    int fd = ::open("/sys/devices/system/cpu/possible", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        int r = errno;
        SILK_FAIL("could not open the possible-cpu list: r=%d", r);
    }

    SILK_SCOPE_EXIT
    {
        ::close(fd);
    };

    // A sysfs attribute is at most one page, so a page-sized buffer that fills
    // up without hitting EOF means the file is malformed, not undersized.
    char buffer[4096];
    size_t size = 0;

    for (;;)
    {
        ssize_t bytes = ::read(fd, buffer + size, sizeof(buffer) - 1 - size);

        if (bytes > 0)
        {
            size += static_cast<size_t>(bytes);
            SILK_ASSERT(size < sizeof(buffer) - 1, "the possible-cpu list is too long");
            continue;
        }

        if (bytes == 0)
        {
            break;
        }

        int r = errno;
        SILK_ASSERT(r == EINTR, "could not read the possible-cpu list: r=%d", r);
    }

    SILK_ASSERT(size, "the possible-cpu list is empty");
    buffer[size] = '\0';

    // The file holds a kernel cpu-list: comma-separated ids and ranges such as
    // "0-15" or "0,2-4,7". Parse every element so a malformed list is rejected
    // rather than silently truncated, and track the highest id seen.
    unsigned long maxCpuId = 0;
    char * position = buffer;

    for (;;)
    {
        char * end = nullptr;
        unsigned long firstCpuId = std::strtoul(position, &end, 10);

        if (end == position || firstCpuId >= UINT16_MAX)
        {
            SILK_FAIL("malformed possible-cpu list '%s'", buffer);
        }

        position = end;
        unsigned long lastCpuId = firstCpuId;

        if (*position == '-')
        {
            ++position;
            lastCpuId = std::strtoul(position, &end, 10);

            if (end == position || lastCpuId >= UINT16_MAX || lastCpuId < firstCpuId)
            {
                SILK_FAIL("malformed possible-cpu list '%s'", buffer);
            }

            position = end;
        }

        if (lastCpuId > maxCpuId)
        {
            maxCpuId = lastCpuId;
        }

        if (*position == ',')
        {
            ++position;
            continue;
        }

        if (*position && (*position != '\n' || position[1]))
        {
            SILK_FAIL("malformed possible-cpu list '%s'", buffer);
        }

        break;
    }

    return static_cast<uint16_t>(maxCpuId + 1);
}

uint16_t getProcessorCount() noexcept
{
    static const uint16_t count = readPossibleCpuCount();
    return count;
}

} // namespace silk
