#include <silk/fibers/multi-lock.h>

#include <mutex>

namespace silk
{

bool FiberMultiLock::try_lock(uint64_t key, ScopedLock * scopedLock) noexcept
{
    WaitEntry * entry = &scopedLock->entry;
    entry->key = key;

    {
        std::lock_guard guard(spinLock);

        WaitEntry * prev = tree.find(entry);
        if (prev)
        {
            return false;
        }

        tree.insert(entry);
    }

    scopedLock->multiLock = this;
    return true;
}

void FiberMultiLock::lock(uint64_t key, ScopedLock * scopedLock, uint64_t * waitCycles) noexcept
{
    WaitEntry * entry = &scopedLock->entry;
    entry->key = key;
    scopedLock->multiLock = this;

    {
        std::lock_guard guard(spinLock);

        WaitEntry * prev = tree.find(entry);
        if (!prev)
        {
            tree.insert(entry);
            return;
        }

        // Held: queue behind the holder and park on our own future below.
        prev->waiters.push_back(entry);
    }

    entry->wait(waitCycles);
}

void FiberMultiLock::unlock(ScopedLock * scopedLock) noexcept
{
    WaitEntry * entry = &scopedLock->entry;

    WaitEntry * successor;
    {
        std::lock_guard guard(spinLock);

        successor = entry->waiters.pop_front();
        if (successor)
        {
            // The promoted waiter inherits the rest of the queue and takes the holder's place in the tree.
            successor->waiters.splice(&entry->waiters);
            tree.remove(entry);
            tree.insert(successor);
        }
        else
        {
            tree.remove(entry);
        }
    }

    if (successor)
    {
        successor->set(0);
    }
}

} // namespace silk
