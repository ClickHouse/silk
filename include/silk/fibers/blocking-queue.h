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
            if (queue.dequeue(value))
            {
                spaceAvailable.post();
                return 0;
            }

            // Drain before honoring teardown: a torn-down queue still yields its remaining items and reports
            // ECANCELED only once empty, so an owner can dequeue to clean up after teardown.
            if (itemAvailable.stopped())
            {
                return ECANCELED;
            }

            int r = itemAvailable.wait(token + 1);
            if (r)
            {
                return r;
            }
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
        spaceAvailable.stop();
        itemAvailable.stop();
    }

private:
    BoundedQueue<T> queue;
    FiberFutex spaceAvailable;
    FiberFutex itemAvailable;
};

} // namespace silk
