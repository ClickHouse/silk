#include "cpu.h"

#include <silk/util/tsc.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <fcntl.h>
#include <unistd.h>

namespace silk
{

static int readSysfsUint32(const char * path, uint32_t * out) noexcept
{
    int fd = ::open(path, O_RDONLY);
    if (fd < 0)
    {
        return errno;
    }

    char buf[32];
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);

    if (n < 0)
    {
        return errno;
    }
    if (n == 0)
    {
        return EIO;
    }
    buf[n] = '\0';

    char * end;
    uint32_t val = ::strtoul(buf, &end, 10);
    if (end == buf)
    {
        return EINVAL;
    }
    *out = val;
    return 0;
}

// Parse a non-negative decimal integer at *p, advancing p past the digits.
// Returns the parsed value, or 0 if no digits are present.
static uint32_t parseUint32(const char *& p) noexcept
{
    uint32_t value = 0;
    while (*p >= '0' && *p <= '9')
    {
        value = value * 10 + static_cast<uint32_t>(*p++ - '0');
    }
    return value;
}

// Parse a single Linux cpulist entry "[a[-b[:c[/d]]]]" and return whether @p cpu
// is included. The kernel's full grammar (see bitmap_parselist) selects bit
// positions in [a, b] for which (pos - a) % d < c -- the stride form is rare
// outside of cgroup-restricted layouts but is valid sysfs output.
static bool cpuInCpulistEntry(uint32_t cpu, const char *& p) noexcept
{
    uint32_t start = parseUint32(p);
    uint32_t end = start;
    if (*p == '-')
    {
        ++p;
        end = parseUint32(p);
    }

    // Optional stride: ":used[/group]". used defaults to 1 (every position),
    // group defaults to used (a:c is shorthand for a:c/c, which selects all
    // positions in the range).
    uint32_t used = 1;
    uint32_t group = 1;
    if (*p == ':')
    {
        ++p;
        used = parseUint32(p);
        group = used;
        if (*p == '/')
        {
            ++p;
            group = parseUint32(p);
        }
    }

    if (cpu < start || cpu > end || group == 0)
    {
        return false;
    }
    return (cpu - start) % group < used;
}

bool cpuInCpulist(uint32_t cpu, const char * list) noexcept
{
    const char * p = list;
    while (*p && *p != '\n')
    {
        if (cpuInCpulistEntry(cpu, p))
        {
            return true;
        }
        if (*p != ',')
        {
            break;
        }
        ++p;
    }
    return false;
}

void readCpuTopologies(CpuTopology * topologies, uint32_t processorCount) noexcept
{
    char path[128];

    for (uint32_t cpu = 0; cpu < processorCount; ++cpu)
    {
        ::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/topology/physical_package_id", cpu);
        readSysfsUint32(path, &topologies[cpu].packageId);

        ::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/topology/core_id", cpu);
        readSysfsUint32(path, &topologies[cpu].coreId);

        // numaNodeId stays at UINT32_MAX until the NUMA pass below sets it.
        // In environments without NUMA sysfs (containers, minimal kernels) it
        // remains UINT32_MAX so topologyCostCycles falls through to the safe
        // cross-NUMA cost rather than misclassifying every CPU as same-node.
    }

    // Open each NUMA node file once and fill all CPUs that belong to it.
    char buf[4096];
    for (uint32_t node = 0;; ++node)
    {
        ::snprintf(path, sizeof(path), "/sys/devices/system/node/node%u/cpulist", node);
        int fd = ::open(path, O_RDONLY);
        if (fd < 0)
        {
            break;
        }
        ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        ::close(fd);

        if (n <= 0)
        {
            continue;
        }
        buf[n] = '\0';

        for (uint32_t cpu = 0; cpu < processorCount; ++cpu)
        {
            if (cpuInCpulist(cpu, buf))
            {
                topologies[cpu].numaNodeId = node;
            }
        }
    }
}

uint64_t topologyCostCycles(const CpuTopology & first, const CpuTopology & second) noexcept
{
    if (first.packageId == UINT32_MAX || second.packageId == UINT32_MAX)
    {
        // topology is unknown
        return Tsc::nanosecondsToCycles(500'000);
    }
    if (first.packageId == second.packageId && first.coreId == second.coreId)
    {
        // HT sibling ~1 us
        return Tsc::nanosecondsToCycles(1'000);
    }
    // Treat unknown NUMA (UINT32_MAX, e.g. containers without /sys/devices/system/node)
    // as cross-NUMA: the equality check below would otherwise classify two unknown
    // CPUs as same-node.
    if (first.numaNodeId != UINT32_MAX && first.numaNodeId == second.numaNodeId)
    {
        // same NUMA ~50 us
        return Tsc::nanosecondsToCycles(50'000);
    }
    // cross-NUMA ~500 us
    return Tsc::nanosecondsToCycles(500'000);
}

} // namespace silk
