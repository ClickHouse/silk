#pragma once

#include <silk/fibers/sequencer.h>

#include <atomic>
#include <cstdint>

namespace silk
{

/**
 * Fair fiber-aware mutex. Waiters are granted the lock in FIFO order (ticket lock).
 *
 * Conforms to BasicLockable; compatible with std::lock_guard.
 */
class FairFiberMutex
{
public:
    using Future = FiberSequencer::Future;

    /** Acquire the lock, suspending the calling fiber until it becomes available. */
    void lock() noexcept
    {
        uint64_t ticket = nextTicket.fetch_add(1, std::memory_order_relaxed);
        sequencer.wait(ticket);
    }

    /**
     * Take a ticket and register @p future to be set when the lock is available.
     * Returns without blocking; the caller must wait on the future separately.
     * This allows the caller to signal readiness after ticket acquisition but
     * before actually blocking.
     */
    void lock(Future * future) noexcept
    {
        uint64_t ticket = nextTicket.fetch_add(1, std::memory_order_relaxed);
        sequencer.wait(ticket, future);
    }

    /** Release the lock and wake the next waiting fiber. */
    void unlock() noexcept { sequencer.increment(); }

private:
    std::atomic<uint64_t> nextTicket{};
    FiberSequencer sequencer;
};

} // namespace silk
