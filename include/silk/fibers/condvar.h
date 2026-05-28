#pragma once

#include <silk/fibers/future.h>
#include <silk/util/list.h>
#include <silk/util/spinlock.h>

#include <cerrno>
#include <cstdint>

namespace silk
{

/**
 * Fiber-aware condition variable.
 *
 * Mirrors std::condition_variable: a fiber atomically releases @p lock and
 * suspends inside wait(); the lock is re-acquired before wait() returns.
 * notify_one() wakes one currently waiting fiber and is a no-op if none is
 * waiting; notify_all() wakes everyone currently waiting and has no effect
 * on fibers that arrive later. Spurious wakeups are permitted by the standard
 * and callers must re-check the protected predicate after wait() returns.
 */
class FiberCondVar
{
public:
    /**
     * Per-waiter state. Inherits FiberFuture so wait_for can route through
     * FiberFuture::waitWithTimeout. Constructed on the stack inside wait() /
     * wait_for(); not intended for direct use by callers.
     */
    class Future : public FiberFuture
    {
    private:
        friend class FiberCondVar;

        /** Cancel a pending wait. Sets the future with ECANCELED if still queued. */
        void cancel() noexcept { condVar->cancelWait(this); }

        ListEntry listEntry;
        FiberCondVar * condVar = nullptr;
        // True while this Future is linked in FiberCondVar::waiters. Read and
        // written only under spinLock; serializes cancel() vs notify_one() /
        // notify_all() so exactly one path removes the future from the list
        // and calls set().
        bool inWaiters = false;
    };

    /**
     * Suspend the calling fiber until a notify wakes it. @p lock must be held
     * on entry; it is released while the fiber is suspended and re-acquired
     * before this function returns.
     */
    template <typename Lockable>
    void wait(Lockable & lock) noexcept
    {
        Future future;
        attach(&future);

        lock.unlock();
        future.wait();
        lock.lock();
    }

    /**
     * Suspend the calling fiber until a notify wakes it, or until @p nanoseconds
     * elapses. @p lock must be held on entry; it is released while suspended and
     * re-acquired before this function returns, whether the wakeup came from a
     * notify or from the timeout.
     *
     * Returns 0 on a notify-driven wakeup or ETIMEDOUT on timeout. If a notify
     * races with the timeout at the boundary the notification may be lost; the
     * caller's predicate re-check (per the usual cv contract) handles that.
     */
    template <typename Lockable>
    [[nodiscard]] int wait_for(Lockable & lock, uint64_t nanoseconds) noexcept
    {
        Future future;
        attach(&future);

        lock.unlock();

        int r = FiberFuture::waitWithTimeout(&future, nanoseconds);
        if (r == ETIMEDOUT)
        {
            future.cancel();
            future.wait();
        }

        lock.lock();
        return r;
    }

    /** Wake one currently waiting fiber. No-op if no fibers are waiting. */
    void notify_one() noexcept;

    /** Wake all currently waiting fibers. No effect on fibers that arrive later. */
    void notify_all() noexcept;

private:
    using WaiterList = List<Future, &Future::listEntry>;

    //
    // Helpers.
    //

    void attach(Future * future) noexcept;
    void cancelWait(Future * future) noexcept;

    //
    // State.
    //

    SpinLock spinLock;
    WaiterList waiters;
};

} // namespace silk
