#pragma once

#include <cerrno>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <signal.h>

/**
 * Block SIGPIPE, SIGINT and SIGTERM on the calling thread.
 * Returns the signal mask for use with sigwaitFor.
 */
sigset_t blockSignals() noexcept;

/**
 * Wait for a signal in mask for up to ns nanoseconds.
 * Returns true if a signal was received, false if the timeout expired.
 */
bool sigwaitFor(const sigset_t & mask, uint64_t ns) noexcept;

/**
 * Sort latNs, compute latency statistics, and print the "latency_us" JSON section.
 */
void printLatencyUs(std::vector<uint64_t> & latNs) noexcept;

/**
 * Print the "scheduler_latency" JSON section, aggregating per-CPU profiler
 * histograms by event kind and fiber category.
 */
void printSchedulerLatency() noexcept;

/**
 * Print the "counters" JSON section: scheduler-wide simple counters.
 * Outputs no trailing comma; intended as the last field of the JSON object.
 */
void printCounters() noexcept;

/**
 * Parse a size string with optional k/m/g suffix (case-insensitive) into bytes.
 * Examples: "4k" -> 4096, "1g" -> 1073741824, "512" -> 512.
 */
uint64_t parseSize(const std::string & str);

/**
 * Parse a duration string with an optional unit suffix into nanoseconds.
 * Supported units: ns, us, ms, s, m. No suffix is treated as seconds.
 * Examples: "10" -> 10000000000, "100us" -> 100000, "1ms" -> 1000000.
 */
uint64_t parseDuration(const std::string & str);

/**
 * Format a nanosecond duration as a string using the largest exact unit.
 * Examples: 10000000000 -> "10s", 1500000 -> "1500us", 500 -> "500ns".
 */
std::string formatDuration(uint64_t ns);

static inline bool isExpectedShutdown(int r)
{
    return r == ECONNRESET || r == ECANCELED || r == EBADF || r == EPIPE || r == EINVAL;
}

/**
 * Per-connection Poisson scheduler for stall messages in net-perf workloads.
 * next() returns the stall budget (nanoseconds) for the upcoming message;
 * the client encodes the value into the first 4 bytes and the server reads
 * it back with busyLoopForStall. Inter-arrival times are exponentially
 * distributed at the configured rate. rateHz = 0 disables stalls entirely.
 */
class StallScheduler
{
public:
    /** Default-constructed scheduler is disabled; next() returns 0. */
    StallScheduler() noexcept = default;

    /**
     * Construct an armed scheduler. The first inter-arrival is sampled in the
     * constructor so the caller's first call to next() returns 0 (no stall on
     * the first message). rateHz = 0 leaves the scheduler disabled.
     */
    StallScheduler(double rateHz_, uint64_t stallNs_, uint64_t seed) noexcept
        : rateHz(rateHz_)
        , stallNs(stallNs_)
        , rng(seed)
    {
        // The discarded return value samples the first inter-arrival and
        // arms nextStallCycles; subsequent calls fire when due.
        next();
    }

    /** True if the scheduler will ever emit a stall. */
    bool enabled() const noexcept { return rateHz > 0.0; }

    /** Returns the stall duration (ns) for the next message; 0 means no stall. */
    uint32_t next() noexcept;

private:
    double rateHz = 0.0;
    uint64_t stallNs = 0;
    std::mt19937_64 rng;
    uint64_t nextStallCycles = 0;
};

/**
 * Read the leading uint32_t stall_ns prefix from a fully-received message
 * buffer and busy-loop (RDTSC + cpuPause) for that duration. The server
 * side of every net-perf variant calls this immediately after reading a
 * complete message, before echoing.
 */
void busyLoopForStall(const char * buf) noexcept;
