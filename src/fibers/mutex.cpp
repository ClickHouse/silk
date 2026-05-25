#include <silk/fibers/mutex.h>

#include <silk/fibers/fiber.h>
#include <silk/util/assert.h>
#include <silk/util/spinlock.h>

#include <atomic>

namespace silk
{

void FiberMutex::lockSlow(State currentState) noexcept
{
    if (currentState.exclusive)
    {
        SILK_ASSERT_DEBUG(
            currentState.value != reinterpret_cast<uint64_t>(FiberScheduler::getCurrentFiber()), "FiberMutex is not reentrant");

        // Spin for ~500 ns (16 x ~35 ns PAUSE on Skylake) before suspending.
        static constexpr uint32_t SPIN_COUNT = 16;

        // Spin briefly only when the blocker is an identifiable exclusive owner
        // currently running on another CPU. Shared holders have no recorded
        // identity, so we cannot verify they are running and skip the spin in
        // that case - exclusive acquirers waiting on shared traffic go straight
        // to suspend.
        if (!currentState.hasWaiters)
        {
            Fiber * owner = reinterpret_cast<Fiber *>(currentState.value);
            bool ownerRunning = owner && FiberScheduler::isFiberRunning(owner);
            if (ownerRunning)
            {
                spinWait([this] { return !state.load(std::memory_order_relaxed); }, SPIN_COUNT);
                currentState.raw = state.load(std::memory_order_relaxed);
            }
        }
    }

    while (!lockHelper(&currentState))
    {
        SuspendCtx ctx{this, true};
        FiberScheduler::suspend(reinterpret_cast<FiberScheduler::SuspendCallback *>(suspendCallback), &ctx);
        currentState.raw = state.load(std::memory_order_relaxed);
    }
}

void FiberMutex::lockSharedSlow(State currentState) noexcept
{
    static constexpr uint32_t SPIN_COUNT = 16;

    // Same spin policy as lockSlow: only when the blocker is an identifiable
    // exclusive owner running on another CPU.
    if (!currentState.hasWaiters && currentState.exclusive)
    {
        Fiber * owner = reinterpret_cast<Fiber *>(currentState.value);
        bool ownerRunning = owner && FiberScheduler::isFiberRunning(owner);
        if (ownerRunning)
        {
            spinWait(
                [this]
                {
                    State s;
                    s.raw = state.load(std::memory_order_relaxed);
                    return !s.exclusive;
                },
                SPIN_COUNT);
            currentState.raw = state.load(std::memory_order_relaxed);
        }
    }

    while (!lockSharedHelper(&currentState))
    {
        SuspendCtx ctx{this, false};
        FiberScheduler::suspend(reinterpret_cast<FiberScheduler::SuspendCallback *>(suspendCallback), &ctx);
        currentState.raw = state.load(std::memory_order_relaxed);
    }
}

bool FiberMutex::lockHelper(State * currentState) noexcept
{
    for (;;)
    {
        if (!currentState->raw)
        {
            State newState;
            newState.exclusive = 1;
            newState.value = reinterpret_cast<uint64_t>(FiberScheduler::getCurrentFiber());
            SILK_ASSERT(!newState.hasWaiters);

            if (state.compare_exchange_weak(currentState->raw, newState.raw, std::memory_order_acq_rel, std::memory_order_relaxed))
            {
                return true;
            }
            continue;
        }

        // hasWaiters signals to the releasing fiber (unlock or unlock_shared)
        // that there are suspended fibers in the waiter table, so it must call
        // releaseWaiters. The first waiter to find the lock held sets the bit;
        // subsequent waiters see it already set and skip straight to the last
        // return false below.
        if (!currentState->hasWaiters)
        {
            State newState(*currentState);
            newState.hasWaiters = 1;

            if (state.compare_exchange_weak(currentState->raw, newState.raw, std::memory_order_release, std::memory_order_relaxed))
            {
                return false;
            }
            continue;
        }

        return false;
    }
}

bool FiberMutex::lockSharedHelper(State * currentState) noexcept
{
    for (;;)
    {
        if (!currentState->exclusive)
        {
            State newState(*currentState);
            newState.value = currentState->value + 1;

            if (state.compare_exchange_weak(currentState->raw, newState.raw, std::memory_order_acquire, std::memory_order_relaxed))
            {
                return true;
            }
            continue;
        }

        if (!currentState->hasWaiters)
        {
            State newState(*currentState);
            newState.hasWaiters = 1;

            if (state.compare_exchange_weak(currentState->raw, newState.raw, std::memory_order_release, std::memory_order_relaxed))
            {
                return false;
            }
            continue;
        }

        return false;
    }
}

void FiberMutex::suspendCallback(Fiber * fiber, SuspendCtx * ctx) noexcept
{
    FiberMutex * mutex = ctx->mutex;
    bool exclusive = ctx->exclusive;

    State currentState;
    currentState.raw = mutex->state.load(std::memory_order_acquire);
    bool satisfied = exclusive ? !currentState.raw : !currentState.exclusive;
    if (satisfied)
    {
        FiberScheduler::schedule(fiber);
        return;
    }

    FiberScheduler::enqueueWaiter(reinterpret_cast<uint64_t>(mutex), fiber);

    // Re-check after enqueue. If hasWaiters is false, the previous releasing
    // fiber already called releaseWaiters but missed us (we were not yet in
    // the waiter table). Self-release here is correct: the popped fiber will
    // retry its helper and either acquire or re-arm hasWaiters.
    currentState.raw = mutex->state.load(std::memory_order_acquire);
    if (!currentState.hasWaiters)
    {
        FiberScheduler::releaseWaiters(reinterpret_cast<uint64_t>(mutex));
    }
}

} // namespace silk
