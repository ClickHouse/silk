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
    head.store(TaggedPtr<QueueNode>{dummy, 0}, std::memory_order_relaxed);
    tail.store(TaggedPtr<QueueNode>{dummy, 0}, std::memory_order_relaxed);
}

QueueBase::QueueNode * QueueBase::allocateDummy() noexcept
{
    QueueNode * dummy = pool->allocate();
    SILK_ASSERT(dummy, "could not allocate a queue node");

    dummy->value.store(nullptr, std::memory_order_relaxed);

    // Keep the tag, so that a CAS from a read of next before this reuse of dummy fails.
    TaggedPtr<QueueNode> dummyNext = dummy->next.load(std::memory_order_relaxed);
    TaggedPtr<QueueNode> lastNext{nullptr, dummyNext.tag};
    dummy->next.store(lastNext, std::memory_order_relaxed);
    return dummy;
}

void QueueBase::enqueue(void * value, QueueNode * node) noexcept
{
    node->value.store(value, std::memory_order_relaxed);

    // Keep the tag, so that a CAS from a read of next before this reuse of node fails.
    TaggedPtr<QueueNode> nodeNext = node->next.load(std::memory_order_relaxed);
    TaggedPtr<QueueNode> lastNext{nullptr, nodeNext.tag};
    node->next.store(lastNext, std::memory_order_relaxed);

    for (;;)
    {
        TaggedPtr<QueueNode> tailSnapshot = tail.load(std::memory_order_acquire);
        TaggedPtr<QueueNode> next = tailSnapshot.ptr->next.load(std::memory_order_acquire);

        // Check if tail and tail->next are consistent.
        if (tailSnapshot == tail.load(std::memory_order_acquire))
        {
            // Try to link node at the end of the linked list.
            if (!next.ptr)
            {
                TaggedPtr<QueueNode> newNext{node, next.tag + 1};
                if (tailSnapshot.ptr->next.compare_exchange_weak(next, newNext, std::memory_order_acq_rel, std::memory_order_relaxed))
                {
                    // Try to swing tail to the inserted node.
                    TaggedPtr<QueueNode> newTail{node, tailSnapshot.tag + 1};
                    tail.compare_exchange_weak(tailSnapshot, newTail, std::memory_order_acq_rel, std::memory_order_relaxed);
                    return;
                }
            }
            else
            {
                // Try to swing tail to the next node.
                TaggedPtr<QueueNode> newTail{next.ptr, tailSnapshot.tag + 1};
                tail.compare_exchange_weak(tailSnapshot, newTail, std::memory_order_acq_rel, std::memory_order_relaxed);
            }
        }
    }
}

void * QueueBase::dequeue(QueueNode ** recycled) noexcept
{
    for (;;)
    {
        TaggedPtr<QueueNode> headSnapshot = head.load(std::memory_order_acquire);
        TaggedPtr<QueueNode> tailSnapshot = tail.load(std::memory_order_acquire);
        QueueNode * next = headSnapshot.ptr->next.load(std::memory_order_acquire).ptr;

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
                TaggedPtr<QueueNode> newTail{next, tailSnapshot.tag + 1};
                tail.compare_exchange_weak(tailSnapshot, newTail, std::memory_order_acq_rel, std::memory_order_relaxed);
            }
            else
            {
                // Read value before CAS, otherwise another dequeue might free the next node.
                void * value = next->value.load(std::memory_order_relaxed);

                // Try to swing head to the next node.
                TaggedPtr<QueueNode> newHead{next, headSnapshot.tag + 1};
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
