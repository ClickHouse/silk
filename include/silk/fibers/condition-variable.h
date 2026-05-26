#pragma once

#include <silk/fibers/future.h>
#include <silk/fibers/sequencer.h>

#include <cstdint>

namespace silk
{

/**
 * Fiber-aware condition variable.
 *
 * Both notify_one and notify_all wake every waiter.
 * Callers must loop on a predicate to tolerate spurious wakeups.
 */
class FiberConditionVariable
{
public:
    /**
     * Suspend the calling fiber until a notification fires. The lock is
     * released for the duration of the wait and reacquired before returning.
     */
    template <typename Lock>
    void wait(Lock & lock) noexcept
    {
        uint64_t token = sequencer.get() + 1;
        lock.unlock();
        sequencer.wait(token);
        lock.lock();
    }

    /**
     * Suspend the calling fiber until a notification fires or @p nanoseconds
     * elapses, whichever comes first. The lock is released for the duration of
     * the wait and reacquired before returning.
     * Returns 0 on notification, or ETIMEDOUT if the deadline expired first.
     */
    template <typename Lock>
    [[nodiscard]] int wait_for(Lock & lock, uint64_t nanoseconds) noexcept
    {
        uint64_t token = sequencer.get() + 1;
        lock.unlock();

        FiberSequencer::Future future;
        sequencer.wait(token, &future);

        const int r = FiberFuture::waitWithTimeout(&future, nanoseconds);

        if (r == ETIMEDOUT)
        {
            future.cancel();
            (void)future.wait();
        }

        lock.lock();
        return r;
    }

    /** Wake every waiter whose captured token has been reached. */
    void notify_one() noexcept { (void)sequencer.increment(); }

    /** Wake every waiter whose captured token has been reached. */
    void notify_all() noexcept { (void)sequencer.increment(); }

private:
    FiberSequencer sequencer;
};

} // namespace silk
