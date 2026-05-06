#pragma once

#include <silk/util/memory-pool.h>

#include <atomic>
#include <cstdint>

namespace silk
{

/**
 * Lock-free FIFO queue implementing the Michael & Scott algorithm
 * (https://www.cs.rochester.edu/u/scott/papers/1996_PODC_queues.pdf).
 *
 * Use the typed wrappers (Queue or IntrusiveQueue) instead of this class directly.
 */
class QueueBase
{
public:
    /**
     * Queue node holding a pointer to the enqueued value.
     */
    struct alignas(CACHELINE_SIZE) QueueNode
    {
        StackEntry stackEntry;
        std::atomic<QueueNode *> next;
        std::atomic<void *> value;
        // TODO(vskipin): we can store small data in-place
    };

    static_assert(sizeof(QueueNode) == CACHELINE_SIZE);

    // Match offsets used by src/gdb/fiber.py::_walk_queue
    static_assert(offsetof(QueueNode, next) == 8);
    static_assert(offsetof(QueueNode, value) == 16);

    static void initialize() noexcept;
    static void destroy() noexcept;

    /** Initialize the queue with an externally supplied sentinel node. */
    explicit QueueBase(QueueNode * dummyNode) noexcept;

    /** Append a value to the tail of the queue using a pre-allocated node. Cannot fail. */
    void enqueue(void * value, QueueNode * node) noexcept;

    /**
     * Remove and return the value at the head of the queue, or nullptr if empty.
     * On success, stores the recycled dummy node into *recycled; the caller must
     * keep it alive for the next enqueue (e.g. store in the dequeued element).
     */
    void * dequeue(QueueNode ** recycled) noexcept;

    /**
     * Returns true if the queue appears empty.  This is a relaxed check; it may transiently
     * return true while a concurrent enqueue is in progress.
     */
    bool empty() const noexcept
    {
        QueueNode * sentinel = head.load(std::memory_order_relaxed).ptr;
        return sentinel->next.load(std::memory_order_relaxed) == nullptr;
    }

protected:
    /**
     * 128-bit tagged pointer; the tag prevents the ABA problem.
     */
    struct alignas(16) TaggedPtr
    {
        QueueNode * ptr;
        uint64_t tag;

        bool operator==(const TaggedPtr & other) const noexcept { return ptr == other.ptr && tag == other.tag; }
    };

    static_assert(sizeof(TaggedPtr) == 16);
    static_assert(std::atomic<TaggedPtr>::is_always_lock_free);

    using MemoryPool = MemoryPool<QueueNode, &QueueNode::stackEntry>;

    //
    // State.
    //

    std::atomic<TaggedPtr> head;
    std::atomic<TaggedPtr> tail;

    static inline MemoryPool * pool;
};

/**
 * Typed lock-free FIFO queue. Nodes are allocated from a shared MemoryPool.
 */
template <typename T>
class Queue : private QueueBase
{
public:
    Queue() noexcept
        : QueueBase(pool->allocate())
    {
    }
    ~Queue() noexcept { pool->deallocate(head.load(std::memory_order_relaxed).ptr); }

    /** Append an object, allocating a node from the pool. Returns false on OOM. */
    [[nodiscard]] bool enqueue(T * value) noexcept
    {
        QueueNode * node = pool->allocate();
        if (node)
        {
            QueueBase::enqueue(value, node);
            return true;
        }
        return false;
    }

    /** Remove and return the object at the head of the queue, or nullptr if empty. */
    T * dequeue() noexcept
    {
        QueueNode * recycled;
        T * value = static_cast<T *>(QueueBase::dequeue(&recycled));
        if (value)
        {
            pool->deallocate(recycled);
        }
        return value;
    }

    using QueueBase::empty;
};

/**
 * Typed lock-free FIFO queue using nodes embedded in the queued objects.
 *
 * T must contain:
 *   - A QueueBase::Node member (the intrusive node), and
 *   - A QueueBase::Node * member pointed to by reservedPtr, initialized
 *     to the address of that embedded node.
 *
 * enqueue cannot fail: it uses the pre-allocated node from reservedPtr.
 * After each dequeue the recycled sentinel is stored back into reservedPtr so
 * the invariant is maintained for the next enqueue.
 */
template <typename T, QueueBase::QueueNode * T::* reservedPtr>
class IntrusiveQueue : private QueueBase
{
public:
    IntrusiveQueue() noexcept
        : QueueBase(&dummy)
    {
    }

    /** Append object to the tail using its reserved node. Cannot fail. */
    void enqueue(T * object) noexcept { QueueBase::enqueue(object, object->*reservedPtr); }

    /** Remove and return the object at the head, or nullptr if empty.
     *  Stores the recycled sentinel into the dequeued object's reservedPtr. */
    T * dequeue() noexcept
    {
        QueueBase::QueueNode * recycled;
        T * object = static_cast<T *>(QueueBase::dequeue(&recycled));
        if (object)
        {
            object->*reservedPtr = recycled;
        }
        return object;
    }

    using QueueBase::empty;

private:
    QueueBase::QueueNode dummy;
};

} // namespace silk
