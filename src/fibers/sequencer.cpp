#include <silk/fibers/sequencer.h>

#include <silk/util/assert.h>

#include <cerrno>

namespace silk
{

void FiberSequencer::reset(uint64_t value) noexcept
{
    SILK_ASSERT(waiters.empty() && requestQueue.empty() && cancelQueue.empty());
    counter.store(value, std::memory_order_release);
}

void FiberSequencer::registerWaiter(uint64_t token, Future * future) noexcept
{
    // Slow path: register future in the request queue for the next combiner to process.
    future->sequencer = this;
    future->token = token;
    future->state.store(0, std::memory_order_relaxed);

    requestQueue.push(future);

    // Waiter half of a StoreLoad (Dekker) handshake on requestQueue vs counter (acquire / release cannot do
    // StoreLoad), symmetric to the producer half in drain: order the push above before the counter re-read
    // below, so a registration racing an advance is never missed by both the re-read and the combiner's scan.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // Re-check after push: handles the race where increment (or stop) fired
    // between the check above and the push. If the counter is now satisfied or
    // the sequencer is stopped, become the combiner and drain the queue (which
    // will set our future immediately).
    if (counter.load(std::memory_order_acquire) >= token || stopFlag.load(std::memory_order_acquire))
    {
        drain();
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
    // Producer half of a StoreLoad (Dekker) handshake on counter vs combinerState (acquire / release cannot
    // do StoreLoad): order a caller's preceding counter advance before the combinerState observe below, so a
    // combiner found already running is guaranteed to see the advance - else it skips a reached waiter and exits.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // Only one combiner runs at a time; others signal PENDING and return.
    if (!acquireCombiner())
    {
        return;
    }

    WaitList wakeList;
    WaitList cancelList;

    for (;;)
    {
        uint64_t current = counter.load(std::memory_order_acquire);
        bool stopping = stopFlag.load(std::memory_order_acquire);

        // Drain cancelled futures. The wake loop may have already removed (and cleared IN_TABLE on) one whose
        // token was reached first; Tree::remove is UB on a node already out of the tree, so only remove if we
        // still hold IN_TABLE. The cancelQueue is the sole completer of a cancelled tree future either way.
        Future * cancelled = cancelQueue.popAll();
        while (cancelled)
        {
            Future * next = RequestQueue::next(cancelled);

            uint32_t prev = cancelled->state.fetch_and(~Future::IN_TABLE, std::memory_order_relaxed);
            SILK_ASSERT(prev & Future::CANCELLED);
            if (prev & Future::IN_TABLE)
            {
                waiters.remove(cancelled);
            }

            cancelList.push(cancelled);
            cancelled = next;
        }

        // Classify incoming futures. A future whose token is already reached skips the tree entirely - it
        // would only be inserted here and immediately popped by the wake loop below. It must NOT set
        // IN_TABLE: leaving it clear keeps it out of the tree, so a racing cancelWait sees "not in table"
        // and routes its cancellation through the CANCELLED flag here, rather than enqueuing a tree-removal
        // for a future that was never inserted (which would also double-set the future).
        Future * future = requestQueue.popAll();
        while (future)
        {
            Future * next = RequestQueue::next(future);

            if (future->token <= current)
            {
                uint32_t prev = future->state.load(std::memory_order_acquire);
                SILK_ASSERT((prev & Future::IN_TABLE) == 0);
                if (prev & Future::CANCELLED)
                {
                    cancelList.push(future);
                }
                else
                {
                    wakeList.push(future);
                }
            }
            else if (stopping)
            {
                // Stopped: an unreached waiter never enters the tree - complete it with ECANCELED. IN_TABLE
                // stays clear, so a racing cancelWait routes through the CANCELLED flag and enqueues nothing;
                // this list is the future's sole completer either way.
                cancelList.push(future);
            }
            else
            {
                // Must wait: claim a tree slot by setting IN_TABLE, unless a cancel raced ahead - then route
                // to cancel without entering the tree. One CAS, so IN_TABLE is set only on a real insert (no
                // fetch_or-then-undo); a cancel landing between the load and the CAS just fails it and re-checks.
                uint32_t prev = future->state.load(std::memory_order_relaxed);
                for (;;)
                {
                    SILK_ASSERT((prev & Future::IN_TABLE) == 0);
                    if (prev & Future::CANCELLED)
                    {
                        cancelList.push(future);
                        break;
                    }
                    if (future->state.compare_exchange_weak(
                            prev, prev | Future::IN_TABLE, std::memory_order_acq_rel, std::memory_order_relaxed))
                    {
                        waiters.insert(future);
                        break;
                    }
                }
            }

            future = next;
        }

        // Wake reached tree entries. A reached future that was cancelled while tree-resident is already in
        // the cancelQueue (cancelWait saw IN_TABLE set), so the cancelQueue drain completes it as ECANCELED;
        // drop it here and route only non-cancelled futures to the wake list, so each lands in exactly one list.
        while (Future * future = waiters.min())
        {
            if (future->token > current)
            {
                break;
            }

            uint32_t prev = future->state.fetch_and(~Future::IN_TABLE, std::memory_order_relaxed);
            SILK_ASSERT(prev & Future::IN_TABLE);

            waiters.remove(future);

            if ((prev & Future::CANCELLED) == 0)
            {
                wakeList.push(future);
            }
        }

        // Stopped: flush the remaining (unreached) tree entries with ECANCELED. A tree future cancelled while
        // tree-resident sits in the cancelQueue (cancelWait saw IN_TABLE), which stays its sole completer -
        // clearing IN_TABLE here routes the next cancelQueue drain past the tree removal, as in the wake loop.
        if (stopping)
        {
            while (Future * future = waiters.min())
            {
                uint32_t prev = future->state.fetch_and(~Future::IN_TABLE, std::memory_order_relaxed);
                SILK_ASSERT(prev & Future::IN_TABLE);

                waiters.remove(future);

                if ((prev & Future::CANCELLED) == 0)
                {
                    cancelList.push(future);
                }
            }
        }

        // Repeat if another thread signalled PENDING while we were draining,
        // meaning new work (increments or cancellations) arrived during this pass.
        if (releaseCombiner())
        {
            break;
        }

        // Combiner half: order the PENDING -> BUSY restore before the next counter load, so the re-read sees
        // the advance that signalled PENDING. (First pass needs none - PENDING forces this re-loop.)
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    // Wake outside the combiner so a woken fiber re-entering drain cannot deadlock on the combiner.
    setAll(&wakeList, 0);
    setAll(&cancelList, ECANCELED);
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

void FiberSequencer::setAll(WaitList * wakeList, int err) noexcept
{
    FiberFuture * wakeBatch[WAKE_BATCH];
    uint64_t batchSize = 0;

    while (Future * future = wakeList->pop())
    {
        wakeBatch[batchSize++] = future;
        if (batchSize == WAKE_BATCH)
        {
            FiberFuture::setAll(err, wakeBatch, batchSize);
            batchSize = 0;
        }
    }

    if (batchSize)
    {
        FiberFuture::setAll(err, wakeBatch, batchSize);
    }
}

} // namespace silk
