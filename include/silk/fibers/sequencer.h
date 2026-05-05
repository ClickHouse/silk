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

    /** Wait until the counter reaches @p token, blocking the calling fiber. */
    void wait(uint64_t token) noexcept
    {
        Future future;
        wait(token, &future);
        future.wait();
    }

    /**
     * Register @p future to be set when the counter reaches @p token.
     * Sets the future immediately if the counter is already >= @p token.
     */
    void wait(uint64_t token, Future * future) noexcept;

    /**
     * Increment the counter and wake all futures whose token has been reached.
     * Returns the new counter value.
     */
    uint64_t increment() noexcept;

    /**
     * Advance the counter to @p value if @p value exceeds the current counter.
     * Wakes all futures whose token is now reached.
     * Returns true if the counter was advanced, false if it was already >= @p value.
     */
    [[nodiscard]] bool advance(uint64_t value) noexcept;

private:
    struct FutureCompare
    {
        bool operator()(const Future & l, const Future & r) const noexcept { return l.token < r.token; }
    };

    using RequestQueue = LockFreeStack<Future, &Future::stackEntry>;
    using WaiterTree = Tree<Future, &Future::treeEntry, FutureCompare, true /* AllowDuplicates */>;
    using WaitList = Stack<Future, &Future::stackEntry>;

    //
    // Constants.
    //

    static constexpr uint32_t FREE = 0;
    static constexpr uint32_t BUSY = 1;
    static constexpr uint32_t PENDING = 2;

    //
    // Helpers.
    //

    void cancelWait(Future * future) noexcept;
    void drain() noexcept;
    bool acquireCombiner() noexcept;
    bool releaseCombiner() noexcept;

    //
    // State.
    //

    std::atomic<uint64_t> counter{};
    std::atomic<uint32_t> combinerState{FREE};
    RequestQueue requestQueue;
    RequestQueue cancelQueue;
    WaiterTree waiters;
};

} // namespace silk
