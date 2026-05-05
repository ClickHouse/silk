#include <silk/fibers/mutex.h>

#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>

#include <gtest/gtest.h>

namespace silk
{

TEST(FiberMutex, tryLock)
{
    FiberMutex mutex;

    bool b = mutex.try_lock();
    ASSERT_TRUE(b);

    b = mutex.try_lock();
    ASSERT_TRUE(!b);

    mutex.unlock();

    b = mutex.try_lock();
    ASSERT_TRUE(b);
}

TEST(FiberMutex, lockFromFiber)
{
    struct Params
    {
        FiberMutex * mutex;
        FiberFuture * started;
        FiberFuture * acquired;

        static int fiberMain(Params * p) noexcept
        {
            p->started->set(0);
            p->mutex->lock();
            p->acquired->set(0);
            p->mutex->unlock();
            return 0;
        }
    };

    FiberMutex mutex;
    mutex.lock();

    FiberFuture future, started, acquired;
    int r = FiberScheduler::run(Params::fiberMain, {&mutex, &started, &acquired}, &future);
    ASSERT_FALSE(r);

    started.wait();
    mutex.unlock();
    acquired.wait();

    future.wait();
}

TEST(FiberMutex, lockFromThread)
{
    struct HolderParams
    {
        FiberMutex * mutex;
        FiberFuture * locked;
        FiberFuture * release;

        static int fiberMain(HolderParams * p) noexcept
        {
            p->mutex->lock();
            p->locked->set(0);
            p->release->wait();
            p->mutex->unlock();
            return 0;
        }
    };

    struct ReleaserParams
    {
        FiberFuture * release;

        static int fiberMain(ReleaserParams * p) noexcept
        {
            FiberScheduler::yield();
            p->release->set(0);
            return 0;
        }
    };

    FiberMutex mutex;
    FiberFuture locked, release;
    FiberFuture holder, releaser;

    int r = FiberScheduler::run(HolderParams::fiberMain, {&mutex, &locked, &release}, &holder);
    ASSERT_FALSE(r);
    locked.wait();

    r = FiberScheduler::run(ReleaserParams::fiberMain, {&release}, &releaser);
    ASSERT_FALSE(r);

    mutex.lock();
    mutex.unlock();

    holder.wait();
    releaser.wait();
}

TEST(FiberMutex, lockContended)
{
    struct ParamsA
    {
        FiberMutex * mutex;
        FiberFuture * locked;
        FiberFuture * release;

        static int fiberMain(ParamsA * p) noexcept
        {
            p->mutex->lock();
            p->locked->set(0);
            p->release->wait();
            p->mutex->unlock();
            return 0;
        }
    };

    struct ParamsB
    {
        FiberMutex * mutex;
        FiberFuture * acquired;

        static int fiberMain(ParamsB * p) noexcept
        {
            p->mutex->lock();
            p->acquired->set(0);
            p->mutex->unlock();
            return 0;
        }
    };

    FiberMutex mutex;
    FiberFuture locked, release, acquired;
    FiberFuture futureA, futureB;

    int r = FiberScheduler::run(ParamsA::fiberMain, {&mutex, &locked, &release}, &futureA);
    ASSERT_FALSE(r);
    locked.wait();

    r = FiberScheduler::run(ParamsB::fiberMain, {&mutex, &acquired}, &futureB);
    ASSERT_FALSE(r);
    release.set(0);
    acquired.wait();

    futureA.wait();
    futureB.wait();
}

TEST(FiberMutex, mutualExclusion)
{
    static constexpr int N_FIBERS = 8;
    static constexpr int N_ITER = 1000;

    struct Params
    {
        FiberMutex * mutex;
        int * counter;

        static int fiberMain(Params * p) noexcept
        {
            for (int i = 0; i < N_ITER; ++i)
            {
                p->mutex->lock();
                ++(*p->counter);
                p->mutex->unlock();
            }
            return 0;
        }
    };

    FiberMutex mutex;
    int counter = 0;

    FiberFuture futures[N_FIBERS];
    for (int i = 0; i < N_FIBERS; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, {&mutex, &counter}, &futures[i]);
        ASSERT_FALSE(r);
    }

    for (int i = 0; i < N_FIBERS; ++i)
    {
        futures[i].wait();
    }

    ASSERT_EQ(counter, N_FIBERS * N_ITER);
}

} // namespace silk
