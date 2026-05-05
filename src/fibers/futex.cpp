#include <silk/fibers/futex.h>

#include <silk/fibers/fiber.h>
#include <silk/util/spinlock.h>

#include <atomic>

namespace silk
{

void FiberFutex::wait(uint64_t token) noexcept
{
    // Spin for ~500 ns (16 x ~35 ns PAUSE on Skylake) before suspending.
    static constexpr uint32_t SPIN_COUNT = 16;

    State currentState;
    currentState.raw = state.load(std::memory_order_relaxed);

    // Spin briefly before suspending: if the poster is on another CPU and fires
    // within ~500 ns, we avoid the full scheduler wakeup path.
    // Skip if there are already waiters in the queue.
    if (!currentState.hasWaiters)
    {
        spinWait(
            [this, token]
            {
                State s;
                s.raw = state.load(std::memory_order_relaxed);
                return s.counter >= token || s.hasWaiters;
            },
            SPIN_COUNT);
    }

    while (!waitHelper(token))
    {
        SuspendCtx ctx{this, token};
        FiberScheduler::suspend(reinterpret_cast<FiberScheduler::SuspendCallback *>(suspendCallback), &ctx);
    }
}

void FiberFutex::post() noexcept
{
    State currentState;
    currentState.raw = state.load(std::memory_order_relaxed);
    for (;;)
    {
        State newState;
        newState.counter = currentState.counter + 1;
        newState.hasWaiters = 0;
        if (state.compare_exchange_weak(currentState.raw, newState.raw, std::memory_order_release, std::memory_order_relaxed))
        {
            break;
        }
    }

    if (currentState.hasWaiters)
    {
        FiberScheduler::releaseWaiters(reinterpret_cast<uint64_t>(this));
    }
}

bool FiberFutex::waitHelper(uint64_t token) noexcept
{
    State currentState;
    currentState.raw = state.load(std::memory_order_acquire);
    for (;;)
    {
        if (currentState.counter >= token)
        {
            return true;
        }

        if (!currentState.hasWaiters)
        {
            State newState(currentState);
            newState.hasWaiters = 1;
            if (state.compare_exchange_weak(currentState.raw, newState.raw, std::memory_order_release, std::memory_order_acquire))
            {
                return false;
            }
            continue;
        }

        return false;
    }
}

void FiberFutex::suspendCallback(Fiber * fiber, SuspendCtx * ctx) noexcept
{
    FiberFutex * event = ctx->event;

    State currentState;
    currentState.raw = event->state.load(std::memory_order_acquire);
    if (currentState.counter >= ctx->token)
    {
        FiberScheduler::schedule(fiber);
        return;
    }

    FiberScheduler::enqueueWaiter(reinterpret_cast<uint64_t>(event), fiber);

    // Re-check after enqueue. If hasWaiters is false, post() already ran its
    // releaseWaiters but missed us (we were not yet in the table). We must
    // release now to avoid a missed wakeup. If the counter already satisfies
    // the token, the fibers we wake will return immediately from waitHelper.
    currentState.raw = event->state.load(std::memory_order_acquire);
    if (!currentState.hasWaiters)
    {
        FiberScheduler::releaseWaiters(reinterpret_cast<uint64_t>(event));
    }
}

} // namespace silk
