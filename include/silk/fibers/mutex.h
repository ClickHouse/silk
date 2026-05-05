#pragma once

#include <atomic>
#include <cstdint>

namespace silk
{

class Fiber;

/**
 * Fiber-aware mutex. Suspends the calling fiber (rather than blocking the thread)
 * while waiting for the lock.
 *
 * Conforms to the BasicLockable and Lockable named requirements; compatible with
 * std::lock_guard and std::unique_lock.
 */
class FiberMutex
{
public:
    /** Attempt to acquire the lock without suspending; returns true on success. */
    [[nodiscard]] bool try_lock() noexcept;

    /** Acquire the lock, suspending the calling fiber until it becomes available. */
    void lock() noexcept;

    /** Release the lock and wake any waiting fibers. */
    void unlock() noexcept;

private:
    /**
     * Packed mutex state.
     */
    union State
    {
        struct
        {
            uint64_t owner : 63;
            uint64_t hasWaiters : 1;
        };
        uint64_t raw = 0;
    };

    static_assert(sizeof(State) == 8);

    //
    // Helpers.
    //

    bool lockHelper() noexcept;
    static void suspendCallback(Fiber * fiber, FiberMutex * mutex) noexcept;

    //
    // State.
    //

    std::atomic<uint64_t> state{};
};

} // namespace silk
