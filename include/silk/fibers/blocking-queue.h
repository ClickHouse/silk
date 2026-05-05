#pragma once

#include <silk/fibers/futex.h>
#include <silk/util/bounded-queue.h>

#include <atomic>
#include <cerrno>
#include <cstdint>

namespace silk
{

/**
 * Fiber-aware bounded MPMC blocking queue.
 *
 * enqueue and dequeue suspend the calling fiber when the queue is full or
 * empty, respectively, and resume when space or an item becomes available.
 * teardown unblocks all current and future waiters with ECANCELED.
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
        while (!stopping.load(std::memory_order_relaxed))
        {
            uint64_t token = spaceAvailable.get();
            if (queue.enqueue(value))
            {
                itemAvailable.post();
                return 0;
            }
            if (stopping.load(std::memory_order_relaxed))
            {
                break;
            }
            spaceAvailable.wait(token + 1);
        }
        return ECANCELED;
    }

    /**
     * Write the head value into @p value, suspending if empty.
     * Returns 0 on success or ECANCELED if the queue has been torn down.
     */
    [[nodiscard]] int dequeue(T * value) noexcept
    {
        while (!stopping.load(std::memory_order_relaxed))
        {
            uint64_t token = itemAvailable.get();
            if (queue.dequeue(value))
            {
                spaceAvailable.post();
                return 0;
            }
            if (stopping.load(std::memory_order_relaxed))
            {
                break;
            }
            itemAvailable.wait(token + 1);
        }
        return ECANCELED;
    }

    /** Append a value without suspending. Returns false if the queue is full. */
    [[nodiscard]] bool tryEnqueue(T value) noexcept
    {
        if (!stopping.load(std::memory_order_relaxed) && queue.enqueue(value))
        {
            itemAvailable.post();
            return true;
        }
        return false;
    }

    /** Write the head value into @p value without suspending. Returns false if empty. */
    [[nodiscard]] bool tryDequeue(T * value) noexcept
    {
        if (!stopping.load(std::memory_order_relaxed) && queue.dequeue(value))
        {
            spaceAvailable.post();
            return true;
        }
        return false;
    }

    /** Unblock all current and future enqueue/dequeue callers with ECANCELED. */
    void teardown() noexcept
    {
        stopping.store(true, std::memory_order_relaxed);
        spaceAvailable.post();
        itemAvailable.post();
    }

private:
    BoundedQueue<T> queue;
    FiberFutex spaceAvailable;
    FiberFutex itemAvailable;
    std::atomic<bool> stopping{};
};

} // namespace silk
