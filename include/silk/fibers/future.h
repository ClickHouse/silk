#pragma once

#include <atomic>
#include <cstdint>

namespace silk
{

class Fiber;

/**
 * Single-producer, single-consumer future.
 *
 * The producing fiber calls set(); the consuming fiber (or thread) calls wait() or isSet().
 * Call reset() to reuse the future for the next fiber.
 */
class FiberFuture
{
public:
    /**
     * Set the result and wake the waiting fiber if one is registered.
     * Must be called at most once.
     */
    void set(int err) noexcept
    {
        error = err;
        signal();
    }

    /**
     * Non-blocking check. Returns true and writes the result to *err if set,
     * false otherwise.
     */
    [[nodiscard]] bool isSet(int * err) noexcept
    {
        State currentState;
        currentState.raw = state.load(std::memory_order_acquire);
        if (currentState.isSet)
        {
            *err = error;
            return true;
        }
        return false;
    }

    /**
     * Block until the result is available, then return it.
     * Returns immediately if set() has already been called.
     */
    int wait() noexcept
    {
        State currentState;
        currentState.raw = state.load(std::memory_order_acquire);
        if (currentState.isSet)
        {
            return error;
        }
        return suspend();
    }

    /**
     * Callback signature for subscribe(). The future is already set when
     * the callback is invoked; call wait() to retrieve the result.
     */
    using SubscribeCallback = void(FiberFuture * future) noexcept;

    /**
     * Atomically attach a callback to be invoked on completion.
     *
     * Returns true if the callback was registered and will be called by signal()
     * when the future is set. Returns false if the future is already set; the
     * caller is responsible for invoking the callback.
     *
     * Constraints (same as wait()):
     *   - At most one waiter at a time; do not combine with wait() or waitForMultiple().
     *   - The callback runs on an arbitrary thread and must not block.
     *
     * @return true if registered, false if the future was already set.
     */
    [[nodiscard]] bool subscribe(SubscribeCallback * callback) noexcept;

    /**
     * Reset the future so it can be reused. Must only be called when no
     * fiber is currently waiting and the previous result has been consumed.
     */
    void reset() noexcept
    {
        error = 0;
        state.store(0, std::memory_order_release);
    }

    /**
     * Block until at least one future in @p futureArray is set, then return
     * the index of a completed future. Returns immediately if any future is
     * already set when called.
     * Each future must have no existing waiter on entry (single waiter per future).
     *
     * Note: the caller is still responsible for eventually calling wait() on
     * every future in the array, including those that were not yet set when
     * this function returned.
     *
     * @param futureArray     Array of futures to watch.
     * @param futureArraySize Number of futures in the array.
     * @return                Index of a future that is set on return.
     */
    static uint64_t waitForMultiple(FiberFuture ** futureArray, uint64_t futureArraySize) noexcept;

    /**
     * Wait for @p future with a timeout. Blocks until @p future is set or
     * @p nanoseconds elapses, whichever comes first.
     *
     * Returns the future's error value on success, or ETIMEDOUT if the deadline
     * expired before the future was set. The future is left unmodified on timeout;
     * the caller is responsible for any subsequent cancellation or cleanup.
     *
     * @param future      Future to wait on.
     * @param nanoseconds Maximum time to wait in nanoseconds.
     */
    [[nodiscard]] static int waitWithTimeout(FiberFuture * future, uint64_t nanoseconds) noexcept;

private:
    /**
     * Packed future state.
     */
    union State
    {
        struct
        {
            uint64_t waiter : 61;
            uint64_t multipleWait : 1;
            uint64_t hasCallback : 1;
            uint64_t isSet : 1;
        };
        uint64_t raw = 0;
    };

    static_assert(sizeof(State) == 8);

    /**
     * Shared state to coordinate a multiple wait operation.
     */
    struct MultipleWaitState
    {
        FiberFuture * completionFuture;
        std::atomic<uint64_t> completionCounter;
    };

    static_assert(alignof(MultipleWaitState) >= 8);

    //
    // Helpers.
    //

    void signal() noexcept;
    int suspend() noexcept;
    static void suspendCallback(Fiber * fiber, FiberFuture * future) noexcept;
    bool attachWaiter(MultipleWaitState * waitState) noexcept;
    bool detachWaiter(MultipleWaitState * waitState) noexcept;

    //
    // State.
    //

    std::atomic<uint64_t> state{};
    int error = 0;
};

} // namespace silk
