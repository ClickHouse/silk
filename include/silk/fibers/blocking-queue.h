#pragma once

#include <silk/fibers/futex.h>
#include <silk/util/bounded-queue.h>

#include <cerrno>
#include <cstdint>

namespace silk
{

/**
 * Fiber-aware bounded MPMC blocking queue.
 *
 * enqueue and dequeue suspend the calling fiber when the queue is full or
 * empty, respectively, and resume when space or an item becomes available.
 * teardown stops new enqueues and unblocks waiters; a torn-down queue still
 * drains - dequeue / tryDequeue yield the remaining items and report closed
 * (ECANCELED / false) only once empty, so an owner can dequeue to clean up.
 */
template <typename T>
class FiberBlockingQueue
{
public:
    explicit FiberBlockingQueue(uint64_t capacity) noexcept
        : queue(capacity)
    {
    }

    /**
     * Append a value to the tail of the queue, suspending if full.
     * Returns 0 on success or ECANCELED if the queue has been torn down.
     */
    [[nodiscard]] int enqueue(T value) noexcept
    {
        while (!spaceAvailable.stopped())
        {
            uint64_t token = spaceAvailable.get();
            if (queue.enqueue(value))
            {
                itemAvailable.post();
                return 0;
            }

            int r = spaceAvailable.wait(token + 1);
            if (r)
            {
                return r;
            }
        }

        return ECANCELED;
    }

    /**
     * Write the head value into @p value, suspending if empty.
     * Returns 0 on success, or ECANCELED once the queue is both empty and torn down (remaining items drain first).
     */
    [[nodiscard]] int dequeue(T * value) noexcept
    {
        for (;;)
        {
            uint64_t token = itemAvailable.get();

            // Drain before honoring teardown: read the stop before the dequeue. teardown stops only after close saw
            // every reserved slot published, so a dequeue after the stop read sees every item a successful enqueue
            // published, and an empty result is final.
            bool stopped = itemAvailable.stopped();
            if (queue.dequeue(value))
            {
                spaceAvailable.post();
                return 0;
            }

            if (stopped)
            {
                return ECANCELED;
            }

            // A stop ends the wait with ECANCELED and drops every later post; the next pass reads the stop and drains.
            int r = itemAvailable.wait(token + 1);
            SILK_UNUSED(r);
        }
    }

    /** Append a value without suspending. Returns false if the queue is full or torn down. */
    [[nodiscard]] bool tryEnqueue(T value) noexcept
    {
        if (!spaceAvailable.stopped() && queue.enqueue(value))
        {
            itemAvailable.post();
            return true;
        }
        return false;
    }

    /** Write the head value into @p value without suspending. Drains after teardown; returns false only when empty. */
    [[nodiscard]] bool tryDequeue(T * value) noexcept
    {
        if (queue.dequeue(value))
        {
            spaceAvailable.post();
            return true;
        }
        return false;
    }

    /** Stop new enqueues and unblock waiters: enqueue returns ECANCELED; dequeue drains the rest, then ECANCELED. */
    void teardown() noexcept
    {
        // Every enqueue that reserved before the close has published when it returns, so dequeue drains it.
        queue.close();
        spaceAvailable.stop();
        itemAvailable.stop();
    }

private:
    BoundedQueue<T> queue;

    /** Posted by dequeue, read by every enqueue; cache-line isolated from dequeuePos and itemAvailable. */
    alignas(kCacheLineSize) FiberFutex spaceAvailable;

    /** Posted by every enqueue, waited by dequeue; isolated so producer posts never invalidate the consumer's lines. */
    alignas(kCacheLineSize) FiberFutex itemAvailable;
};

} // namespace silk
