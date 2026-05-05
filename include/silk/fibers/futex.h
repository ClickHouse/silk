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

    /** Wait until at least one post fires after this call. */
    void wait() noexcept { wait(get() + 1); }

    /**
     * Wait until the counter reaches @p token.
     * Returns immediately if the counter is already >= @p token.
     * @p token is typically obtained as get() + 1 to wait for the next post.
     */
    void wait(uint64_t token) noexcept;

    /** Increment the counter and wake all waiting fibers. */
    void post() noexcept;

private:
    /**
     * Packed event state.
     */
    union State
    {
        struct
        {
            uint64_t counter : 63;
            uint64_t hasWaiters : 1;
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
