#include <silk/fibers/mutex.h>

#include <silk/fibers/fiber.h>
#include <silk/util/assert.h>
#include <silk/util/spinlock.h>

#include <atomic>

namespace silk
{

bool FiberMutex::try_lock() noexcept
{
    State currentState;
    currentState.raw = state.load(std::memory_order_relaxed);
    for (;;)
    {
        if (currentState.raw)
        {
            return false;
        }

        State newState;
        newState.owner = reinterpret_cast<uint64_t>(FiberScheduler::getCurrentFiber());
        SILK_ASSERT(!newState.hasWaiters);

        if (state.compare_exchange_weak(currentState.raw, newState.raw, std::memory_order_acquire, std::memory_order_relaxed))
        {
            return true;
        }
    }
}

void FiberMutex::lock() noexcept
{
    // Spin for ~500 ns (16 x ~35 ns PAUSE on Skylake) before suspending.
    static constexpr uint32_t SPIN_COUNT = 16;

    State currentState;
    currentState.raw = state.load(std::memory_order_relaxed);

    SILK_ASSERT_DEBUG(currentState.owner != reinterpret_cast<uint64_t>(FiberScheduler::getCurrentFiber()), "FiberMutex is not reentrant");

    // Spin briefly before suspending: if the owner is on another CPU and releases
    // within ~500 ns, we avoid the full scheduler wakeup path.
    // Skip if there are already waiters in the queue.
    //
    // The owner pointer is loaded from a stale snapshot of state, so by the time
    // we call isFiberRunning the original owner may have unlocked, returned, and
    // been recycled by the fiber pool into a different fiber. The pool never
    // unmaps Fiber memory, so the load on owner->state is always safe; the worst
    // case is a spurious 500 ns spin against the wrong fiber's state, after which
    // lockHelper takes the slow path. No correctness consequence.
    if (!currentState.hasWaiters)
    {
        Fiber * owner = reinterpret_cast<Fiber *>(currentState.owner);
        if (owner && FiberScheduler::isFiberRunning(owner))
        {
            spinWait([this] { return !state.load(std::memory_order_relaxed); }, SPIN_COUNT);
        }
    }

    // Slow path: try to acquire; suspend if still locked.
    while (!lockHelper())
    {
        FiberScheduler::suspend(reinterpret_cast<FiberScheduler::SuspendCallback *>(suspendCallback), this);
    }
}

void FiberMutex::unlock() noexcept
{
    State currentState;
    currentState.raw = state.load(std::memory_order_relaxed);

    SILK_ASSERT_DEBUG(
        currentState.owner == reinterpret_cast<uint64_t>(FiberScheduler::getCurrentFiber()),
        "FiberMutex::unlock called by non-owner fiber");

    for (;;)
    {
        SILK_ASSERT(currentState.raw);

        if (state.compare_exchange_weak(currentState.raw, 0, std::memory_order_release, std::memory_order_relaxed))
        {
            if (currentState.hasWaiters)
            {
                FiberScheduler::releaseWaiters(reinterpret_cast<uint64_t>(this));
            }
            return;
        }
    }
}

bool FiberMutex::lockHelper() noexcept
{
    State currentState;
    currentState.raw = state.load(std::memory_order_relaxed);
    for (;;)
    {
        if (!currentState.raw)
        {
            State newState;
            newState.owner = reinterpret_cast<uint64_t>(FiberScheduler::getCurrentFiber());
            SILK_ASSERT(!newState.hasWaiters);

            if (state.compare_exchange_weak(currentState.raw, newState.raw, std::memory_order_acquire, std::memory_order_relaxed))
            {
                return true;
            }
            continue;
        }

        // hasWaiters signals to unlock() that there are suspended fibers waiting
        // in the waiter table, so it must call releaseWaiters. The first fiber to
        // find the mutex locked sets this flag; subsequent fibers see it already
        // set and skip straight to the last return false below.
        if (!currentState.hasWaiters)
        {
            State newState(currentState);
            newState.hasWaiters = true;

            if (state.compare_exchange_weak(currentState.raw, newState.raw, std::memory_order_release, std::memory_order_relaxed))
            {
                return false;
            }
            continue;
        }

        return false;
    }
}

void FiberMutex::suspendCallback(Fiber * fiber, FiberMutex * mutex) noexcept
{
    State currentState;
    currentState.raw = mutex->state.load(std::memory_order_acquire);
    if (!currentState.raw)
    {
        FiberScheduler::schedule(fiber);
        return;
    }

    FiberScheduler::enqueueWaiter(reinterpret_cast<uint64_t>(mutex), fiber);

    // Re-check after enqueue. If hasWaiters is false, we must release waiters
    // ourselves. This covers two cases:
    // 1. Mutex is unlocked: the holder already called releaseWaiters but missed us
    //    (we were not yet in the stack).
    // 2. Mutex is locked with hasWaiters=false: the holder unlocked (seeing
    //    hasWaiters=true, getting an empty stack) and re-acquired before we
    //    enqueued, resetting hasWaiters=false. The holder's next unlock will not
    //    call releaseWaiters. Releasing now causes a spurious wakeup, but the
    //    fibers will retry lockHelper, setting hasWaiters=true, and the holder
    //    will release them on the following unlock.
    currentState.raw = mutex->state.load(std::memory_order_acquire);
    if (!currentState.hasWaiters)
    {
        FiberScheduler::releaseWaiters(reinterpret_cast<uint64_t>(mutex));
    }
}

} // namespace silk
