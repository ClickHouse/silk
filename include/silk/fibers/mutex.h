#pragma once

#include <silk/fibers/fiber.h>
#include <silk/util/assert.h>

#include <atomic>
#include <cstdint>

namespace silk
{

/**
 * Fiber-aware shared mutex. Suspends the calling fiber (rather than blocking
 * the thread) while waiting for the lock.
 *
 * Conforms to the BasicLockable, Lockable, and SharedMutex named requirements;
 * compatible with std::lock_guard, std::unique_lock, and std::shared_lock.
 *
 * Wake model is wake-all on every release. Writer-priority is best-effort:
 * while an exclusive waiter is queued, hasExclusiveWaiters forces new
 * lock_shared callers off the fast path. The bit is briefly cleared at each
 * release and re-armed when the writer re-suspends, so a wake-up race or a
 * reader arriving in that gap can still slip ahead. This strongly favors
 * writers in practice without providing strict starvation-freedom.
 */
class FiberMutex
{
public:
    /** Attempt to acquire the exclusive lock without suspending; returns true on success. */
    [[nodiscard]] bool try_lock() noexcept
    {
        State currentState;
        currentState.raw = state.load(std::memory_order_relaxed);
        if (!currentState.raw)
        {
            State newState;
            newState.exclusive = 1;
            newState.value = reinterpret_cast<uint64_t>(FiberScheduler::getCurrentFiber());
            return state.compare_exchange_weak(currentState.raw, newState.raw, std::memory_order_acq_rel, std::memory_order_relaxed);
        }
        return false;
    }

    /** Acquire the exclusive lock, suspending the calling fiber until it becomes available. */
    void lock() noexcept
    {
        State currentState;
        currentState.raw = state.load(std::memory_order_acquire);
        if (!currentState.raw)
        {
            State newState;
            newState.exclusive = 1;
            newState.value = reinterpret_cast<uint64_t>(FiberScheduler::getCurrentFiber());
            if (state.compare_exchange_weak(currentState.raw, newState.raw, std::memory_order_acq_rel, std::memory_order_acquire))
            {
                return;
            }
        }

        lockSlow(currentState);
    }

    /** Release the exclusive lock and wake any waiting fibers. */
    void unlock() noexcept
    {
        State currentState;
        currentState.raw = state.load(std::memory_order_relaxed);

        SILK_ASSERT_DEBUG(
            currentState.exclusive && currentState.value == reinterpret_cast<uint64_t>(FiberScheduler::getCurrentFiber()),
            "FiberMutex::unlock called by non-owner fiber");

        for (;;)
        {
            SILK_ASSERT(currentState.exclusive && currentState.value);

            if (state.compare_exchange_weak(currentState.raw, 0, std::memory_order_release, std::memory_order_relaxed))
            {
                if (currentState.hasExclusiveWaiters || currentState.hasSharedWaiters) [[unlikely]]
                {
                    FiberScheduler::releaseWaiters(reinterpret_cast<uint64_t>(this));
                }
                return;
            }
        }
    }

    /**
     * Attempt to acquire a shared lock without suspending; returns true on success. Respects
     * writer priority - returns false if any exclusive waiter is queued.
     */
    [[nodiscard]] bool try_lock_shared() noexcept
    {
        State currentState;
        currentState.raw = state.load(std::memory_order_relaxed);
        if (!currentState.exclusive && !currentState.hasExclusiveWaiters)
        {
            State newState(currentState);
            newState.value = currentState.value + 1;
            return state.compare_exchange_weak(currentState.raw, newState.raw, std::memory_order_acquire, std::memory_order_relaxed);
        }
        return false;
    }

    /**
     * Acquire a shared lock, suspending the calling fiber until no exclusive holder remains and
     * no exclusive waiter is queued ahead.
     */
    void lock_shared() noexcept
    {
        State currentState;
        currentState.raw = state.load(std::memory_order_acquire);
        if (!currentState.exclusive && !currentState.hasExclusiveWaiters)
        {
            State newState(currentState);
            newState.value = currentState.value + 1;
            if (state.compare_exchange_weak(currentState.raw, newState.raw, std::memory_order_acquire, std::memory_order_acquire))
            {
                return;
            }
        }

        lockSharedSlow(currentState);
    }

    /** Release a shared lock and, if last shared holder, wake any waiting fibers. */
    void unlock_shared() noexcept
    {
        State currentState;
        currentState.raw = state.load(std::memory_order_relaxed);

        SILK_ASSERT_DEBUG(!currentState.exclusive && currentState.value > 0, "unlock_shared called without shared lock");

        for (;;)
        {
            SILK_ASSERT(!currentState.exclusive && currentState.value > 0);

            State newState(currentState);
            newState.value = currentState.value - 1;
            if (newState.value == 0)
            {
                // Last shared holder out - clear waiter bits atomically so the next acquirer
                // sees a clean unlocked state.
                newState.hasExclusiveWaiters = 0;
                newState.hasSharedWaiters = 0;
            }

            if (state.compare_exchange_weak(currentState.raw, newState.raw, std::memory_order_release, std::memory_order_relaxed))
            {
                if (newState.value == 0 && (currentState.hasExclusiveWaiters || currentState.hasSharedWaiters)) [[unlikely]]
                {
                    FiberScheduler::releaseWaiters(reinterpret_cast<uint64_t>(this));
                }
                return;
            }
        }
    }

private:
    /**
     * Packed mutex state. value reinterprets based on exclusive: shared holder count when
     * exclusive=0, Fiber * (owner) when exclusive=1. raw=0 is the canonical unlocked state.
     * hasExclusiveWaiters / hasSharedWaiters are set by waiters when they suspend and indicate
     * to the releaser that releaseWaiters must be called.
     */
    union State
    {
        struct
        {
            uint64_t value : 61;
            uint64_t exclusive : 1;
            uint64_t hasExclusiveWaiters : 1;
            uint64_t hasSharedWaiters : 1;
        };
        uint64_t raw = 0;
    };

    static_assert(sizeof(State) == 8);

    /** Suspend context: mutex pointer plus the acquire mode the waiter is blocked on. */
    struct SuspendCtx
    {
        FiberMutex * mutex;
        bool exclusive;
    };

    //
    // Helpers.
    //

    void lockSlow(State currentState) noexcept;
    void lockSharedSlow(State currentState) noexcept;
    bool lockHelper(State * currentState) noexcept;
    bool lockSharedHelper(State * currentState) noexcept;
    static void suspendCallback(Fiber * fiber, SuspendCtx * ctx) noexcept;

    //
    // State.
    //

    std::atomic<uint64_t> state{};
};

} // namespace silk
