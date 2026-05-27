#include <silk/fibers/condvar.h>

#include <cerrno>

namespace silk
{

void FiberCondVar::attach(Future * future) noexcept
{
    future->condVar = this;
    future->inWaiters = true;

    spinLock.lock();
    waiters.push_back(future);
    spinLock.unlock();
}

void FiberCondVar::notify_one() noexcept
{
    spinLock.lock();

    Future * future = waiters.pop_front();
    if (future)
    {
        future->inWaiters = false;
    }

    spinLock.unlock();

    if (future)
    {
        future->set(0);
    }
}

void FiberCondVar::notify_all() noexcept
{
    spinLock.lock();

    WaiterList snapshot;
    snapshot.splice(&waiters);

    for (Future * future = snapshot.front(); future; future = snapshot.next(future))
    {
        future->inWaiters = false;
    }

    spinLock.unlock();

    while (Future * future = snapshot.pop_front())
    {
        future->set(0);
    }
}

void FiberCondVar::cancelWait(Future * future) noexcept
{
    spinLock.lock();

    bool inWaiters = future->inWaiters;
    if (inWaiters)
    {
        waiters.remove(future);
        future->inWaiters = false;
    }

    spinLock.unlock();

    if (inWaiters)
    {
        future->set(ECANCELED);
    }
}

} // namespace silk
