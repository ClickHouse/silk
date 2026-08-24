#pragma once

#include <cstdint>

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
