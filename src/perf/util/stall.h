#pragma once

#include <cstdint>
#include <random>

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
