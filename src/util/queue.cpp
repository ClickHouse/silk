#include <silk/util/queue.h>

#include <silk/util/assert.h>

namespace silk
{

void QueueBase::initialize() noexcept
{
    if (pool)
    {
        // Skip the second initialization.
        return;
    }

    pool = new MemoryPool();
    SILK_ASSERT(pool);
}

void QueueBase::destroy() noexcept
{
    delete pool;
    pool = nullptr;
}

QueueBase::QueueBase(QueueNode * dummy) noexcept
{
    SILK_ASSERT(dummy);

    dummy->value.store(nullptr, std::memory_order_relaxed);
    dummy->next.store(nullptr, std::memory_order_relaxed);

    head.store(TaggedPtr{dummy, 0}, std::memory_order_relaxed);
    tail.store(TaggedPtr{dummy, 0}, std::memory_order_relaxed);
}

void QueueBase::enqueue(void * value, QueueNode * node) noexcept
{
    node->value.store(value, std::memory_order_relaxed);
    node->next.store(nullptr, std::memory_order_relaxed);

    for (;;)
    {
        TaggedPtr tailSnapshot = tail.load(std::memory_order_acquire);
        QueueNode * next = tailSnapshot.ptr->next.load(std::memory_order_acquire);

        // Check if tail and tail->next are consistent.
        if (tailSnapshot == tail.load(std::memory_order_acquire))
        {
            // Try to link node at the end of the linked list.
            if (!next)
            {
                if (tailSnapshot.ptr->next.compare_exchange_weak(next, node, std::memory_order_acq_rel, std::memory_order_relaxed))
                {
                    // Try to swing tail to the inserted node.
                    TaggedPtr newTail{node, tailSnapshot.tag + 1};
                    tail.compare_exchange_weak(tailSnapshot, newTail, std::memory_order_acq_rel, std::memory_order_relaxed);
                    return;
                }
            }
            else
            {
                // Try to swing tail to the next node.
                TaggedPtr newTail{next, tailSnapshot.tag + 1};
                tail.compare_exchange_weak(tailSnapshot, newTail, std::memory_order_acq_rel, std::memory_order_relaxed);
            }
        }
    }
}

void * QueueBase::dequeue(QueueNode ** recycled) noexcept
{
    for (;;)
    {
        TaggedPtr headSnapshot = head.load(std::memory_order_acquire);
        TaggedPtr tailSnapshot = tail.load(std::memory_order_acquire);
        QueueNode * next = headSnapshot.ptr->next.load(std::memory_order_acquire);

        // Check if head, tail and head->next are consistent.
        if (headSnapshot == head.load(std::memory_order_acquire))
        {
            // Check if queue is empty or tail falling behind.
            if (headSnapshot.ptr == tailSnapshot.ptr)
            {
                // Check if queue is empty.
                if (!next)
                {
                    return nullptr;
                }

                // Tail is falling behind, try to advance it.
                TaggedPtr newTail{next, tailSnapshot.tag + 1};
                tail.compare_exchange_weak(tailSnapshot, newTail, std::memory_order_acq_rel, std::memory_order_relaxed);
            }
            else
            {
                // Read value before CAS, otherwise another dequeue might free the next node.
                void * value = next->value.load(std::memory_order_relaxed);

                // Try to swing head to the next node.
                TaggedPtr newHead{next, headSnapshot.tag + 1};
                if (head.compare_exchange_weak(headSnapshot, newHead, std::memory_order_acq_rel, std::memory_order_relaxed))
                {
                    *recycled = headSnapshot.ptr;
                    return value;
                }
            }
        }
    }
}

} // namespace silk
