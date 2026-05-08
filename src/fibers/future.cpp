#include <silk/fibers/future.h>

#include <silk/fibers/fiber.h>
#include <silk/util/assert.h>
#include <silk/util/sanitizers.h>
#include <silk/util/spinlock.h>

#include <atomic>
#include <cerrno>

namespace silk
{

bool FiberFuture::subscribe(SubscribeCallback * callback) noexcept
{
    State currentState;
    currentState.raw = state.load(std::memory_order_relaxed);
    for (;;)
    {
        if (currentState.isSet)
        {
            return false;
        }

        State newState{};
        newState.waiter = reinterpret_cast<uint64_t>(callback);
        newState.hasCallback = true;

        if (state.compare_exchange_weak(currentState.raw, newState.raw, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            return true;
        }
    }
}

void FiberFuture::signal() noexcept
{
    State currentState;
    currentState.raw = state.load(std::memory_order_relaxed);
    for (;;)
    {
        if (currentState.isSet)
        {
            return;
        }

        State newState(currentState);
        newState.isSet = true;

        if (state.compare_exchange_weak(currentState.raw, newState.raw, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            if (currentState.multipleWait)
            {
                // Wake the waiter first, then increment the counter. waitForMultiple
                // spins on the counter only after completionFuture.wait() returns, so
                // this order is safe: the drain loop will not start until the wake has
                // already been delivered.
                MultipleWaitState * waitState = reinterpret_cast<MultipleWaitState *>(currentState.waiter);
                waitState->completionFuture->signal();
                waitState->completionCounter.fetch_add(1, std::memory_order_release);

                // TSan can't see happens-before through relaxed-spin + fence; annotate explicitly.
                TSAN_RELEASE(waitState);
            }
            else if (currentState.hasCallback)
            {
                SubscribeCallback * callback = reinterpret_cast<SubscribeCallback *>(currentState.waiter);
                callback(this);
            }
            else
            {
                Fiber * waitingFiber = reinterpret_cast<Fiber *>(currentState.waiter);
                if (waitingFiber)
                {
                    FiberScheduler::schedule(waitingFiber);
                }
            }
            return;
        }
    }
}

int FiberFuture::suspend() noexcept
{
    FiberScheduler::suspend(reinterpret_cast<FiberScheduler::SuspendCallback *>(suspendCallback), this);

    State currentState;
    currentState.raw = state.load(std::memory_order_acquire);
    SILK_ASSERT(currentState.isSet);

    return error;
}

// This callback runs while the fiber is in the SUSPENDED state, which is the
// only safe window to register the waiter. Registering it before suspending
// would allow signal() to schedule the fiber before it has actually left the
// CPU, causing a double-resume. The double-check below handles the race where
// the future was set between wait() reading isSet=false and this callback
// running: if already set, reschedule the fiber immediately instead of parking.
void FiberFuture::suspendCallback(Fiber * fiber, FiberFuture * future) noexcept
{
    State currentState;
    currentState.raw = future->state.load(std::memory_order_relaxed);
    for (;;)
    {
        if (currentState.isSet)
        {
            FiberScheduler::schedule(fiber);
            return;
        }

        State newState;
        newState.waiter = reinterpret_cast<uint64_t>(fiber);
        SILK_ASSERT(!newState.isSet);

        if (future->state.compare_exchange_weak(currentState.raw, newState.raw, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            return;
        }
    }
}

uint64_t FiberFuture::waitForMultiple(FiberFuture ** futureArray, uint64_t futureArraySize) noexcept
{
    if (futureArraySize == 0)
    {
        return 0;
    }
    if (futureArraySize == 1)
    {
        futureArray[0]->wait();
        return 0;
    }

    //
    // Attach waitState to futures until one is found already set.
    // multipleWaitCount == futureArraySize means all were attached (none set yet);
    // multipleWaitCount < futureArraySize means we stopped early.
    //

    FiberFuture completionFuture;

    MultipleWaitState waitState;
    waitState.completionFuture = &completionFuture;

    uint64_t multipleWaitCount = 0;
    for (uint64_t i = 0; i < futureArraySize; ++i)
    {
        if (!futureArray[i]->attachWaiter(&waitState))
        {
            break;
        }
        ++multipleWaitCount;
    }

    if (multipleWaitCount == 0)
    {
        // first future was already set
        return 0;
    }

    if (multipleWaitCount == futureArraySize)
    {
        // all attached; block until first signals
        completionFuture.wait();
    }

    //
    // Detach waitState from all attached futures.
    // A future that could not be detached (already set) has called or will call
    // completionCounter.fetch_add(); we must wait for all such increments before
    // returning, because waitState lives on our stack.
    //

    uint64_t completionIndex = multipleWaitCount;

    uint64_t outstandingCount = 0;
    for (uint64_t i = 0; i < multipleWaitCount; ++i)
    {
        if (!futureArray[i]->detachWaiter(&waitState))
        {
            if (completionIndex == multipleWaitCount)
            {
                completionIndex = i;
            }
            ++outstandingCount;
        }
    }

    if (waitState.completionCounter.load(std::memory_order_acquire) < outstandingCount)
    {
        while (!spinWait([&] { return waitState.completionCounter.load(std::memory_order_relaxed) >= outstandingCount; }))
        {
            schedYield();
        }
        std::atomic_thread_fence(std::memory_order_acquire);
    }

    // acquire side; pairs with TSAN_RELEASE in signal
    TSAN_ACQUIRE(&waitState);

    return completionIndex;
}

// Returns false without modifying state if the future is already set.
bool FiberFuture::attachWaiter(MultipleWaitState * waitState) noexcept
{
    State currentState;
    currentState.raw = state.load(std::memory_order_relaxed);
    for (;;)
    {
        if (currentState.isSet)
        {
            return false;
        }

        State newState;
        newState.waiter = reinterpret_cast<uint64_t>(waitState);
        newState.multipleWait = 1;
        SILK_ASSERT(!newState.isSet);

        if (state.compare_exchange_weak(currentState.raw, newState.raw, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            return true;
        }
    }
}

// Returns false if the future is already set, meaning signal() has been called
// (or is in progress) and waitState->completionCounter will be incremented.
bool FiberFuture::detachWaiter(MultipleWaitState * waitState) noexcept
{
    State currentState;
    currentState.raw = state.load(std::memory_order_relaxed);
    for (;;)
    {
        if (currentState.isSet)
        {
            return false;
        }

        SILK_ASSERT(currentState.multipleWait);
        SILK_ASSERT(reinterpret_cast<MultipleWaitState *>(currentState.waiter) == waitState);

        if (state.compare_exchange_weak(currentState.raw, 0, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            return true;
        }
    }
}

int FiberFuture::waitWithTimeout(FiberFuture * future, uint64_t nanoseconds) noexcept
{
    FiberScheduler::SleepFuture sleepFuture;
    FiberScheduler::sleep(nanoseconds, &sleepFuture);

    FiberFuture * futureArray[] = {future, &sleepFuture};
    waitForMultiple(futureArray, std::size(futureArray));

    int r;
    if (future->isSet(&r))
    {
        sleepFuture.cancel();
    }
    else
    {
        r = ETIMEDOUT;
    }

    // Must wait before returning: the sleep tree holds a raw pointer to sleepFuture,
    // and cancelQueue may also reference it. Returning before the sleep is fully
    // resolved would free the stack-allocated future while it is still reachable.
    sleepFuture.wait();
    return r;
}

} // namespace silk
