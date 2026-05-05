#include <silk/fibers/fair-mutex.h>

#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>

#include <gtest/gtest.h>

namespace silk
{

TEST(FairFiberMutex, lockUnlock)
{
    FairFiberMutex mutex;
    mutex.lock();
    mutex.unlock();
}

TEST(FairFiberMutex, mutualExclusion)
{
    static constexpr int N = 8;
    static constexpr int ITERATIONS = 100;

    struct Params
    {
        FairFiberMutex * mutex;
        int * counter;
        FiberFuture * done;

        static int fiberMain(Params * p) noexcept
        {
            for (int i = 0; i < ITERATIONS; ++i)
            {
                p->mutex->lock();
                int value = *p->counter;
                FiberScheduler::yield();
                *p->counter = value + 1;
                p->mutex->unlock();
            }
            p->done->set(0);
            return 0;
        }
    };

    FairFiberMutex mutex;
    int counter = 0;
    FiberFuture futures[N], done[N];

    for (int i = 0; i < N; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, {&mutex, &counter, &done[i]}, &futures[i]);
        ASSERT_FALSE(r);
    }
    for (int i = 0; i < N; ++i)
    {
        done[i].wait();
        futures[i].wait();
    }

    ASSERT_EQ(counter, N * ITERATIONS);
}

TEST(FairFiberMutex, fifoOrdering)
{
    static constexpr int N = 4;

    struct Params
    {
        FairFiberMutex * mutex;
        int index;
        int * order;
        int * orderCount;
        FiberFuture * registered;
        FiberFuture * done;

        static int fiberMain(Params * p) noexcept
        {
            FairFiberMutex::Future future;
            p->mutex->lock(&future); // take ticket (non-blocking)
            p->registered->set(0); // signal only after ticket is taken
            future.wait(); // block until our turn
            p->order[p->index] = (*p->orderCount)++;
            p->mutex->unlock();
            p->done->set(0);
            return 0;
        }
    };

    FairFiberMutex mutex;
    int order[N] = {};
    int orderCount = 0;
    FiberFuture futures[N], registered[N], done[N];

    // Hold the mutex so all fibers queue up before any runs.
    mutex.lock();

    for (int i = 0; i < N; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, {&mutex, i, order, &orderCount, &registered[i], &done[i]}, &futures[i]);
        ASSERT_FALSE(r);
        registered[i].wait(); // wait for each fiber to take its ticket before starting the next
    }

    mutex.unlock();

    for (int i = 0; i < N; ++i)
    {
        done[i].wait();
        futures[i].wait();
    }

    // Each fiber should have run in the order it registered (ticket order).
    for (int i = 0; i < N; ++i)
    {
        EXPECT_EQ(order[i], i);
    }
}

} // namespace silk
