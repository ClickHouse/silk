#pragma once

#include <atomic>
#include <cstdint>

namespace silk
{

class Fiber;

/**
 * Fiber-aware event. Multiple fibers may wait concurrently; post wakes all of them.
 *
 * Maintains a monotonically increasing counter. Waiters capture a token with get
 * before checking a condition; if the condition is not met, they call wait(token+1)
 * to sleep until the counter advances past that token. This is the same pattern
 * as Linux futex.
 *
 * post has release semantics; get and wait have acquire semantics.
 *
 * stop transitions the futex into a stopped state: every current waiter is woken
 * and every subsequent wait returns ECANCELED without suspending.
 */
class FiberFutex
{
public:
    /** Return the current counter value for use as a wait token. */
    uint64_t get() const noexcept
    {
        State currentState;
        currentState.raw = state.load(std::memory_order_acquire);
        return currentState.counter;
    }

    /** Wait until at least one post fires after this call, or stop() is called. */
    int wait() noexcept { return wait(get() + 1); }

    /**
     * Wait until the counter reaches @p token, or stop() is called.
     * Returns 0 on normal wakeup, ECANCELED if the futex has been stopped.
     * Returns 0 immediately if the counter is already >= @p token (even after stop,
     * the satisfied-token case takes precedence so callers do not lose state changes).
     * @p token is typically obtained as get() + 1 to wait for the next post.
     */
    [[nodiscard]] int wait(uint64_t token) noexcept;

    /** Increment the counter and wake all waiting fibers. No-op once stopped. */
    void post() noexcept;

    /**
     * Transition into the stopped state and wake all current waiters. After this,
     * every wait returns ECANCELED until the object is destroyed. Idempotent.
     */
    void stop() noexcept;

    /** Returns true if stop has been called. */
    bool stopped() const noexcept
    {
        State currentState;
        currentState.raw = state.load(std::memory_order_acquire);
        return currentState.stopped;
    }

private:
    /**
     * Packed event state. counter occupies the low 62 bits, the high two bits are
     * hasWaiters and stopped. Layout is chosen so the (counter, hasWaiters, stopped)
     * triple updates atomically with a single 64-bit CAS.
     */
    union State
    {
        struct
        {
            uint64_t counter : 62;
            uint64_t hasWaiters : 1;
            uint64_t stopped : 1;
        };
        uint64_t raw = 0;
    };

    static_assert(sizeof(State) == 8);

    struct SuspendCtx
    {
        FiberFutex * event;
        uint64_t token;
    };

    //
    // Helpers.
    //

    bool waitHelper(uint64_t token) noexcept;
    static void suspendCallback(Fiber * fiber, SuspendCtx * ctx) noexcept;

    //
    // State.
    //

    std::atomic<uint64_t> state{};
};

} // namespace silk
