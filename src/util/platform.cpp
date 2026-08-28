#include <silk/util/assert.h>
#include <silk/util/platform.h>

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

    char buffer[64];
    ssize_t size = ::read(fd, buffer, sizeof(buffer) - 1);
    int r = errno;
    ::close(fd);

    if (size <= 0)
    {
        SILK_FAIL("could not read the possible-cpu list: r=%d", r);
    }

    buffer[size] = '\0';

    // The file holds an ordered kernel cpu-list such as "0-15" or "0", so the
    // last number is the highest possible CPU id.
    const char * lastNumber = buffer;
    for (const char * position = buffer; *position; ++position)
    {
        if (*position == '-' || *position == ',')
        {
            lastNumber = position + 1;
        }
    }

    char * end = nullptr;
    unsigned long maxCpuId = std::strtoul(lastNumber, &end, 10);
    SILK_ASSERT(end != lastNumber && maxCpuId < UINT16_MAX, "malformed possible-cpu list '%s'", buffer);

    return static_cast<uint16_t>(maxCpuId + 1);
}

uint16_t getProcessorCount() noexcept
{
    static const uint16_t count = readPossibleCpuCount();
    return count;
}

} // namespace silk
