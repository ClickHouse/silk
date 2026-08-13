#pragma once

#include <silk/fibers/future.h>
#include <silk/util/stack.h>
#include <silk/util/tree.h>

#include <atomic>
#include <cstdint>

namespace silk
{

/**
 * Monotone counter with fiber-aware ordered waiters.
 *
 * Waiters register a Future at a specific token; increment advances the
 * counter and wakes all futures whose token has been reached, in token order.
 *
 * stop transitions the sequencer into a stopped state: every unreached waiter
 * is woken with ECANCELED and every subsequent unreached wait completes with
 * ECANCELED without suspending, while the counter (and increment / advance)
 * keeps working - a wait at a reached token still returns 0.
 */
class FiberSequencer
{
public:
    /**
     * Waiter handle. Inherits FiberFuture so callers can call wait directly
     * or pass future to FiberFuture::waitForMultiple
     */
    class Future : public FiberFuture
    {
    public:
        /** Cancel a pending wait. Sets the future with ECANCELED if still pending. */
        void cancel() noexcept
        {
            if (sequencer)
            {
                sequencer->cancelWait(this);
            }
        }

    private:
        friend class FiberSequencer;

        static constexpr uint32_t IN_TABLE = 1 << 0;
        static constexpr uint32_t CANCELLED = 1 << 1;

        StackEntry stackEntry;
        TreeEntry treeEntry;
        FiberSequencer * sequencer = nullptr;
        uint64_t token = 0;
        std::atomic<uint32_t> state{};
    };

    /** Return the current counter value for use as a wait token. */
    uint64_t get() const noexcept { return counter.load(std::memory_order_acquire); }

    /**
     * Wait until the counter reaches @p token, blocking the calling fiber.
     * Returns 0 on normal wakeup, ECANCELED if the sequencer has been stopped.
     * Returns 0 immediately if the counter is already >= @p token, even after stop.
     */
    int wait(uint64_t token, uint64_t * waitCycles = nullptr) noexcept
    {
        // Fast path: counter already satisfied.
        if (counter.load(std::memory_order_acquire) >= token)
        {
            return 0;
        }

        Future future;
        registerWaiter(token, &future);
        return future.wait(waitCycles);
    }

    /**
     * Register @p future to be set when the counter reaches @p token: with 0 on
     * normal wakeup, with ECANCELED if the sequencer is stopped first.
     * Sets the future immediately if the counter is already >= @p token.
     */
    void wait(uint64_t token, Future * future) noexcept
    {
        // Fast path: counter already satisfied.
        if (counter.load(std::memory_order_acquire) >= token)
        {
            future->set(0);
            return;
        }

        registerWaiter(token, future);
    }

    /**
     * Increment the counter and wake all futures whose token has been reached.
     * Returns the new counter value.
     */
    uint64_t increment() noexcept
    {
        uint64_t current = counter.fetch_add(1, std::memory_order_release) + 1;
        drain();
        return current;
    }

    /**
     * Advance the counter to @p value if @p value exceeds the current counter.
     * Wakes all futures whose token is now reached.
     * Returns true if the counter was advanced, false if it was already >= @p value.
     */
    bool advance(uint64_t value) noexcept
    {
        uint64_t current = counter.load(std::memory_order_relaxed);
        for (;;)
        {
            if (current >= value)
            {
                return false;
            }
            if (counter.compare_exchange_weak(current, value, std::memory_order_release, std::memory_order_relaxed))
            {
                break;
            }
        }

        drain();
        return true;
    }

    /**
     * Rebase the counter to @p value, up or down. The stopped state is preserved. The caller guarantees
     * quiescence: no registered waiter - neither tree-resident nor still in the request queue - and no
     * concurrent increment / advance / wait / stop.
     */
    void reset(uint64_t value) noexcept;

    /**
     * Transition into the stopped state and wake every unreached waiter with
     * ECANCELED. After this, every unreached wait completes with ECANCELED
     * without suspending; a reached wait still returns 0, and increment /
     * advance keep working. Idempotent.
     */
    void stop() noexcept
    {
        // The release store orders the flag before drain's seq_cst fence and queue reads; a racing registerWaiter
        // either observes the flag in its post-push re-check or its push is observed by this drain - never neither.
        stopFlag.store(true, std::memory_order_release);
        drain();
    }

    /** Returns true if stop has been called. */
    bool stopped() const noexcept { return stopFlag.load(std::memory_order_acquire); }

private:
    //
    // Constants.
    //

    static constexpr uint32_t FREE = 0;
    static constexpr uint32_t BUSY = 1;
    static constexpr uint32_t PENDING = 2;

    static constexpr uint32_t WAKE_BATCH = 32;

    //
    // Data structures.
    //

    struct FutureCompare
    {
        bool operator()(const Future & l, const Future & r) const noexcept { return l.token < r.token; }
    };

    using RequestQueue = LockFreeStack<Future, &Future::stackEntry>;
    using WaiterTree = Tree<Future, &Future::treeEntry, FutureCompare, true /* AllowDuplicates */>;
    using WaitList = Stack<Future, &Future::stackEntry>;

    //
    // Helpers.
    //

    void registerWaiter(uint64_t token, Future * future) noexcept;
    void cancelWait(Future * future) noexcept;
    void drain() noexcept;
    bool acquireCombiner() noexcept;
    bool releaseCombiner() noexcept;
    static void setAll(WaitList * wakeList, int err) noexcept;

    //
    // State.
    //

    std::atomic<uint64_t> counter{};
    std::atomic<uint32_t> combinerState{FREE};
    std::atomic<bool> stopFlag{};
    RequestQueue requestQueue;
    RequestQueue cancelQueue;
    WaiterTree waiters;
};

} // namespace silk
