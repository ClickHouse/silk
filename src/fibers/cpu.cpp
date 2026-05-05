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

static bool cpuInCpulist(uint32_t cpu, const char * list) noexcept
{
    const char * p = list;
    while (*p && *p != '\n')
    {
        uint32_t start = 0;
        while (*p >= '0' && *p <= '9')
        {
            start = start * 10 + static_cast<uint32_t>(*p++ - '0');
        }
        uint32_t end = start;
        if (*p == '-')
        {
            ++p;
            end = 0;
            while (*p >= '0' && *p <= '9')
            {
                end = end * 10 + static_cast<uint32_t>(*p++ - '0');
            }
        }
        if (cpu >= start && cpu <= end)
        {
            return true;
        }
        if (*p == ',')
        {
            ++p;
        }
        else
        {
            break;
        }
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

        topologies[cpu].numaNodeId = 0;
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
    if (first.numaNodeId == second.numaNodeId)
    {
        // same NUMA ~50 us
        return Tsc::nanosecondsToCycles(50'000);
    }
    // cross-NUMA ~500 us
    return Tsc::nanosecondsToCycles(500'000);
}

} // namespace silk
