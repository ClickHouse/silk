#pragma once

#include <cstdint>
#include <vector>

/**
 * Aggregated hardware/software performance counters over a measured window.
 *
 * open creates one counter group per CPU in the process affinity mask, counting
 * everything scheduled on that CPU: CPU cycles (the group leader), instructions,
 * and context switches. The per-CPU, all-process scope captures kernel and
 * io_uring offload (io-wq, sqpoll) work that a per-thread counter on the
 * submitting thread would miss; it also counts unrelated work on those CPUs, so
 * the numbers are only meaningful on an otherwise idle machine.
 *
 * The events of a group are co-scheduled, so the instructions / cycles ratio is
 * internally consistent. When a group was multiplexed off the PMU, read scales
 * its counts up by the enabled / running time ratio and sets scaled.
 *
 * open requires kernel.perf_event_paranoid <= 0 or CAP_PERFMON. After a failed
 * open the object is inert: enable and disable are no-ops and read fails, so
 * callers wire the bracketing in unconditionally.
 *
 * Counters start disabled; the lifecycle is open, enable, disable, read.
 */
class Pmc
{
public:
    /** Counter totals summed over every per-CPU group. */
    struct Counts
    {
        /** CPU cycles over the window, summed across CPUs. */
        uint64_t cycles = 0;

        /** Retired instructions over the window, summed across CPUs. */
        uint64_t instructions = 0;

        /** Context switches over the window, summed across CPUs. */
        uint64_t contextSwitches = 0;

        /** True when any group was multiplexed and its counts were scaled up. */
        bool scaled = false;
    };

    Pmc() noexcept = default;
    ~Pmc() noexcept { close(); }

    Pmc(const Pmc &) = delete;
    Pmc & operator=(const Pmc &) = delete;

    /**
     * Open one disabled counter group per CPU in the process affinity mask.
     * Returns 0 with a group open on every CPU, otherwise the errno of the
     * first failure (commonly EACCES when the counters are not permitted)
     * with every group closed.
     */
    int open() noexcept;

    /** Start counting on every group. No-op when not open. */
    void enable() noexcept;

    /** Stop counting on every group. No-op when not open. */
    void disable() noexcept;

    /**
     * Sum the scaled totals of every group into counts. Returns 0 on success,
     * ENODEV when not open, or an errno when any group could not be read.
     */
    int read(Counts * counts) noexcept;

    /** Close every group. Idempotent. */
    void close() noexcept;

private:
    /** Perf event file descriptors of one CPU's counter group. */
    struct CpuGroup
    {
        /** CPU the group counts on. */
        int cpu = -1;

        /** CPU cycles event; the group leader. */
        int cyclesFd = -1;

        /** Retired instructions event. */
        int instructionsFd = -1;

        /** Context switches event. */
        int contextSwitchesFd = -1;
    };

    /**
     * Open the three-event group on one CPU into group. Returns 0 on success,
     * otherwise the errno of the failed event with nothing left open.
     */
    static int openGroup(int cpu, CpuGroup * group) noexcept;

    /** One open group per CPU in the affinity mask at open time. */
    std::vector<CpuGroup> groups;
};
