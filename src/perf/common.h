#pragma once

#include <cerrno>
#include <cstdint>
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
 * Sort latNs, compute latency statistics, and print the "latency_us" JSON
 * section. Outputs a trailing comma; must be followed by printCounters().
 */
void printLatencyUs(std::vector<uint64_t> & latNs) noexcept;

/**
 * Print the "counters" JSON section (no trailing comma; closes the object).
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
