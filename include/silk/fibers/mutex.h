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
 * Wake model is wake-all on every release; readers and a queued writer race
 * naturally in their slow paths. Sustained shared traffic can delay an
 * exclusive acquirer.
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
            return state.compare_exchange_weak(currentState.raw, newState.raw, std::memory_order_acquire, std::memory_order_relaxed);
        }
        return false;
    }

    /** Acquire the exclusive lock, suspending the calling fiber until it becomes available. */
    void lock() noexcept
    {
        State currentState;
        currentState.raw = state.load(std::memory_order_relaxed);
        if (!currentState.raw)
        {
            State newState;
            newState.exclusive = 1;
            newState.value = reinterpret_cast<uint64_t>(FiberScheduler::getCurrentFiber());
            if (state.compare_exchange_weak(currentState.raw, newState.raw, std::memory_order_acquire, std::memory_order_relaxed))
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
                if (currentState.hasWaiters) [[unlikely]]
                {
                    FiberScheduler::releaseWaiters(reinterpret_cast<uint64_t>(this));
                }
                return;
            }
        }
    }

    /** Attempt to acquire a shared lock without suspending; returns true on success. */
    [[nodiscard]] bool try_lock_shared() noexcept
    {
        State currentState;
        currentState.raw = state.load(std::memory_order_relaxed);
        if (!currentState.exclusive)
        {
            State newState(currentState);
            newState.value = currentState.value + 1;
            return state.compare_exchange_weak(currentState.raw, newState.raw, std::memory_order_acquire, std::memory_order_relaxed);
        }
        return false;
    }

    /** Acquire a shared lock, suspending the calling fiber until no exclusive holder remains. */
    void lock_shared() noexcept
    {
        State currentState;
        currentState.raw = state.load(std::memory_order_relaxed);
        if (!currentState.exclusive)
        {
            State newState(currentState);
            newState.value = currentState.value + 1;
            if (state.compare_exchange_weak(currentState.raw, newState.raw, std::memory_order_acquire, std::memory_order_relaxed))
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
                // Last shared holder out - clear hasWaiters atomically so the next
                // acquirer sees a clean unlocked state.
                newState.hasWaiters = 0;
            }

            if (state.compare_exchange_weak(currentState.raw, newState.raw, std::memory_order_release, std::memory_order_relaxed))
            {
                if (newState.value == 0 && currentState.hasWaiters) [[unlikely]]
                {
                    FiberScheduler::releaseWaiters(reinterpret_cast<uint64_t>(this));
                }
                return;
            }
        }
    }

private:
    /**
     * Packed mutex state. value reinterprets based on exclusive: shared holder
     * count when exclusive=0, Fiber * (owner) when exclusive=1. raw=0 is the
     * canonical unlocked state.
     */
    union State
    {
        struct
        {
            uint64_t value : 62;
            uint64_t exclusive : 1;
            uint64_t hasWaiters : 1;
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
