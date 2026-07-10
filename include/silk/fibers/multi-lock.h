#pragma once

#include <silk/fibers/future.h>
#include <silk/util/assert.h>
#include <silk/util/list.h>
#include <silk/util/spinlock.h>
#include <silk/util/tree.h>

#include <cstdint>

namespace silk
{

/**
 * A keyed, fiber-aware lock over a uint64_t key space. The same key gives mutual exclusion,
 * distinct keys proceed independently. Contenders for a held key are granted it in FIFO order.
 */
class FiberMultiLock
{
    /** One lock acquisition. The base future is set to grant the key to a waiting contender. */
    struct WaitEntry : FiberFuture
    {
        uint64_t key;
        TreeEntry treeNode;
        ListEntry listNode;
        List<WaitEntry, &WaitEntry::listNode> waiters;
    };

public:
    FiberMultiLock() noexcept = default;
    ~FiberMultiLock() noexcept { SILK_ASSERT(tree.empty()); }

    /**
     * Scoped per-key lock handle. Default-construct one and pass it to FiberMultiLock::lock to acquire the key;
     * the destructor releases the key and hands it to the next waiter. The handle must outlive the hold,
     * and a default-constructed handle never passed to lock destructs as a no-op.
     */
    class ScopedLock
    {
    public:
        ScopedLock() noexcept = default;
        ~ScopedLock() noexcept
        {
            if (multiLock)
            {
                multiLock->unlock(this);
            }
        }

        // non-copyable
        ScopedLock(const ScopedLock &) = delete;
        ScopedLock & operator=(const ScopedLock &) = delete;

    private:
        friend class FiberMultiLock;
        FiberMultiLock * multiLock = nullptr;
        WaitEntry entry;
    };

    /**
     * Try to acquire key without suspending. On success populates scopedLock and returns true; if the key
     * is already held returns false and leaves scopedLock empty, so its destructor is a no-op.
     */
    [[nodiscard]] bool try_lock(uint64_t key, ScopedLock * scopedLock) noexcept;

    /** Acquire key, suspending the calling fiber until it is free, and populate scopedLock. */
    void lock(uint64_t key, ScopedLock * scopedLock, uint64_t * waitCycles = nullptr) noexcept;

private:
    struct Compare
    {
        bool operator()(const WaitEntry & left, const WaitEntry & right) const noexcept { return left.key < right.key; }
    };

    /** Release the lock held via scopedLock, handing it to the next queued waiter if any. */
    void unlock(ScopedLock * scopedLock) noexcept;

    //
    // State.
    //

    SpinLock spinLock;
    Tree<WaitEntry, &WaitEntry::treeNode, Compare> tree;
};

} // namespace silk
