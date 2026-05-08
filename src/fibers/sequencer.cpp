#include <silk/fibers/sequencer.h>

#include <silk/util/assert.h>

#include <cerrno>

namespace silk
{

void FiberSequencer::wait(uint64_t token, Future * future) noexcept
{
    future->sequencer = this;
    future->token = token;
    future->state.store(0, std::memory_order_relaxed);

    // Fast path: counter already satisfied.
    if (counter.load(std::memory_order_acquire) >= token)
    {
        future->set(0);
        return;
    }

    // Slow path: register future in the request queue for the next combiner to process.
    requestQueue.push(future);

    // Re-check after push: handles the race where increment fired between
    // the check above and the push. If the counter is now satisfied, become
    // the combiner and drain the queue (which will set our future immediately).
    if (counter.load(std::memory_order_acquire) >= token)
    {
        drain();
    }
}

uint64_t FiberSequencer::increment() noexcept
{
    uint64_t current = counter.fetch_add(1, std::memory_order_release) + 1;
    drain();
    return current;
}

bool FiberSequencer::advance(uint64_t value) noexcept
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
            drain();
            return true;
        }
    }
}

void FiberSequencer::cancelWait(Future * future) noexcept
{
    uint32_t expected = future->state.load(std::memory_order_relaxed);
    for (;;)
    {
        if (expected & Future::CANCELLED)
        {
            return;
        }
        if (future->state.compare_exchange_weak(
                expected, expected | Future::CANCELLED, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            break;
        }
    }

    if (expected & Future::IN_TABLE)
    {
        cancelQueue.push(future);
    }
    drain();
}

void FiberSequencer::drain() noexcept
{
    // Only one combiner runs at a time; others signal PENDING and return.
    if (!acquireCombiner())
    {
        return;
    }

    WaitList wakeList;
    WaitList cancelList;

    // Repeat if another thread signalled PENDING while we were draining,
    // meaning new work (increments or cancellations) arrived during this pass.
    do
    {
        uint64_t current = counter.load(std::memory_order_acquire);

        // Drain cancelled futures: they are in the tree (IN_TABLE was set when
        // cancelWait pushed them here), so remove them directly.
        Future * cancelled = cancelQueue.popAll();
        while (cancelled)
        {
            Future * next = RequestQueue::next(cancelled);
            waiters.remove(cancelled);
            cancelled->state.fetch_and(~Future::IN_TABLE, std::memory_order_relaxed);
            cancelList.push(cancelled);
            cancelled = next;
        }

        // Classify incoming futures: set IN_TABLE and check whether CANCELLED
        // raced ahead of us. If so, skip the tree insert and cancel instead.
        Future * future = requestQueue.popAll();
        while (future)
        {
            Future * next = RequestQueue::next(future);
            uint32_t prev = future->state.fetch_or(Future::IN_TABLE, std::memory_order_acq_rel);
            if (prev & Future::CANCELLED)
            {
                future->state.fetch_and(~Future::IN_TABLE, std::memory_order_relaxed);
                cancelList.push(future);
            }
            else
            {
                waiters.insert(future);
            }
            future = next;
        }

        // Wake all tree entries whose token has been reached.
        while (Future * future = waiters.min())
        {
            if (future->token > current)
            {
                break;
            }
            waiters.remove(future);
            future->state.fetch_and(~Future::IN_TABLE, std::memory_order_relaxed);
            wakeList.push(future);
        }

    } while (!releaseCombiner());

    // Wake outside the combiner so set can itself call drain without deadlocking.
    while (Future * future = wakeList.pop())
    {
        future->set(0);
    }
    while (Future * future = cancelList.pop())
    {
        future->set(ECANCELED);
    }
}

bool FiberSequencer::acquireCombiner() noexcept
{
    uint32_t state = combinerState.load(std::memory_order_relaxed);
    for (;;)
    {
        if (state == FREE)
        {
            if (combinerState.compare_exchange_weak(state, BUSY, std::memory_order_acquire, std::memory_order_relaxed))
            {
                return true;
            }
        }
        else if (state == BUSY)
        {
            if (combinerState.compare_exchange_weak(state, PENDING, std::memory_order_release, std::memory_order_relaxed))
            {
                return false;
            }
        }
        else
        {
            SILK_ASSERT(state == PENDING);
            return false;
        }
    }
}

bool FiberSequencer::releaseCombiner() noexcept
{
    uint32_t state = combinerState.load(std::memory_order_relaxed);
    for (;;)
    {
        if (state == BUSY)
        {
            if (combinerState.compare_exchange_weak(state, FREE, std::memory_order_release, std::memory_order_acquire))
            {
                return true;
            }
        }
        else
        {
            SILK_ASSERT(state == PENDING);
            combinerState.store(BUSY, std::memory_order_relaxed);
            return false;
        }
    }
}

} // namespace silk
