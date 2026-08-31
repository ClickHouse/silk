#include "pmc.h"

#include <silk/util/logger.h>

#include <cerrno>
#include <cstring>

#include <sched.h>
#include <unistd.h>

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

/** Layout of a group read with PERF_FORMAT_GROUP | TOTAL_TIME_ENABLED | TOTAL_TIME_RUNNING. */
struct GroupReadFormat
{
    /** Number of events in the group. */
    uint64_t count;

    /** Time the group was enabled, in nanoseconds. */
    uint64_t timeEnabled;

    /** Time the group was scheduled on the PMU, in nanoseconds. */
    uint64_t timeRunning;

    /** Event values, in the order the events were opened. */
    uint64_t cycles;
    uint64_t instructions;
    uint64_t contextSwitches;
};

static int perfEventOpen(perf_event_attr * attr, pid_t pid, int cpu, int groupFd, unsigned long flags) noexcept
{
    return static_cast<int>(::syscall(__NR_perf_event_open, attr, pid, cpu, groupFd, flags));
}

/**
 * Open one counting event on the CPU for all processes. A negative groupFd
 * creates a disabled group leader; members inherit the leader's enable state.
 */
static int openEvent(uint32_t type, uint64_t config, int cpu, int groupFd) noexcept
{
    perf_event_attr attr = {};
    attr.size = sizeof(attr);
    attr.type = type;
    attr.config = config;
    attr.disabled = groupFd < 0 ? 1 : 0;
    attr.exclude_hv = 1;
    attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;

    return perfEventOpen(&attr, -1, cpu, groupFd, 0);
}

int Pmc::open() noexcept
{
    close();

    cpu_set_t cpuSet;
    CPU_ZERO(&cpuSet);

    int r = ::sched_getaffinity(0, sizeof(cpuSet), &cpuSet);
    if (r)
    {
        r = errno;
        SILK_WARN("could not read the process affinity mask: %s", std::strerror(r));
        return r;
    }

    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    {
        if (!CPU_ISSET(cpu, &cpuSet))
        {
            continue;
        }

        CpuGroup group;
        r = openGroup(cpu, &group);
        if (r)
        {
            close();
            SILK_WARN(
                "could not open the counters on CPU %d: %s - needs kernel.perf_event_paranoid <= 0 or CAP_PERFMON", cpu, std::strerror(r));
            return r;
        }

        groups.push_back(group);
    }

    return 0;
}

void Pmc::enable() noexcept
{
    for (const CpuGroup & group : groups)
    {
        ::ioctl(group.cyclesFd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
    }
}

void Pmc::disable() noexcept
{
    for (const CpuGroup & group : groups)
    {
        ::ioctl(group.cyclesFd, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
    }
}

int Pmc::read(Counts * counts) noexcept
{
    if (groups.empty())
    {
        return ENODEV;
    }

    Counts totals;

    for (const CpuGroup & group : groups)
    {
        GroupReadFormat values;
        ssize_t n = ::read(group.cyclesFd, &values, sizeof(values));
        if (n < 0)
        {
            return errno;
        }

        // A short read or a group that never counted invalidates the totals.
        if (n != static_cast<ssize_t>(sizeof(values)) || values.count != 3 || values.timeRunning == 0)
        {
            return EIO;
        }

        uint64_t cycles = values.cycles;
        uint64_t instructions = values.instructions;
        uint64_t contextSwitches = values.contextSwitches;

        // The whole group rides the PMU schedule, so a multiplexed window
        // undercounts all three events, including the software one. Scale
        // in floating point - counts times nanoseconds overflows uint64_t.
        if (values.timeRunning < values.timeEnabled)
        {
            totals.scaled = true;
            double scale = static_cast<double>(values.timeEnabled) / static_cast<double>(values.timeRunning);
            cycles = static_cast<uint64_t>(static_cast<double>(cycles) * scale);
            instructions = static_cast<uint64_t>(static_cast<double>(instructions) * scale);
            contextSwitches = static_cast<uint64_t>(static_cast<double>(contextSwitches) * scale);
        }

        totals.cycles += cycles;
        totals.instructions += instructions;
        totals.contextSwitches += contextSwitches;
    }

    *counts = totals;
    return 0;
}

void Pmc::close() noexcept
{
    for (const CpuGroup & group : groups)
    {
        ::close(group.contextSwitchesFd);
        ::close(group.instructionsFd);
        ::close(group.cyclesFd);
    }

    groups.clear();
}

int Pmc::openGroup(int cpu, CpuGroup * group) noexcept
{
    int cyclesFd = openEvent(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, cpu, -1);
    if (cyclesFd < 0)
    {
        return errno;
    }

    int instructionsFd = openEvent(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, cpu, cyclesFd);
    if (instructionsFd < 0)
    {
        int r = errno;
        ::close(cyclesFd);
        return r;
    }

    int contextSwitchesFd = openEvent(PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES, cpu, cyclesFd);
    if (contextSwitchesFd < 0)
    {
        int r = errno;
        ::close(instructionsFd);
        ::close(cyclesFd);
        return r;
    }

    *group = {cpu, cyclesFd, instructionsFd, contextSwitchesFd};
    return 0;
}
