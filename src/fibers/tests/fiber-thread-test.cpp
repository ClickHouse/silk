#include <silk/fibers/condvar.h>
#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>
#include <silk/fibers/mutex.h>
#include <silk/util/assert.h>
#include <silk/util/init.h>

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace silk
{

TEST(FiberThreads, mutexWithoutScheduler)
{
    FiberMutex mutex;
    int counter = 0;

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
    {
        threads.emplace_back(
            [&]
            {
                for (int j = 0; j < 10000; ++j)
                {
                    mutex.lock();
                    ++counter;
                    mutex.unlock();
                }
            });
    }
    for (auto & thread : threads)
    {
        thread.join();
    }

    EXPECT_EQ(counter, 40000);
}

TEST(FiberThreads, condVarWithoutScheduler)
{
    FiberMutex mutex;
    FiberCondVar condVar;
    bool ready = false;

    std::thread waiter(
        [&]
        {
            mutex.lock();
            while (!ready)
            {
                condVar.wait(mutex);
            }
            mutex.unlock();
        });

    mutex.lock();
    ready = true;
    mutex.unlock();
    condVar.notify_one();
    waiter.join();
}

TEST(FiberThreads, waitForTimesOutWithoutScheduler)
{
    FiberMutex mutex;
    FiberCondVar condVar;

    mutex.lock();
    EXPECT_EQ(condVar.wait_for(mutex, 1'000'000), ETIMEDOUT);
    mutex.unlock();
}

TEST(FiberThreads, waitWithTimeoutRacesSet)
{
    for (int i = 0; i < 1000; ++i)
    {
        FiberFuture future;
        std::thread setter([&] { future.set(EIO); });
        int r = FiberFuture::waitWithTimeout(&future, i % 2 ? 0 : 1'000);
        EXPECT_TRUE(r == EIO || r == ETIMEDOUT);
        EXPECT_EQ(future.wait(), EIO);
        setter.join();
    }
}

TEST(FiberThreads, acrossSchedulerLifecycle)
{
    struct Params
    {
        FiberMutex * mutex;
        FiberCondVar * condVar;
        bool * ready;

        static int fiberMain(Params * p) noexcept
        {
            p->mutex->lock();
            *p->ready = true;
            p->mutex->unlock();
            p->condVar->notify_one();
            return 0;
        }
    };

    FiberMutex mutex;
    FiberCondVar condVar;
    bool ready = false;

    std::thread waiter(
        [&]
        {
            mutex.lock();
            while (!ready)
            {
                condVar.wait(mutex);
            }
            mutex.unlock();
        });

    silk::initialize();
    FiberScheduler::initialize();

    FiberFuture future;
    ASSERT_FALSE(FiberScheduler::run(Params::fiberMain, {&mutex, &condVar, &ready}, &future));
    EXPECT_EQ(future.wait(), 0);

    FiberScheduler::destroy();
    waiter.join();

    mutex.lock();
    mutex.unlock();
}

namespace
{

void pingPong()
{
    FiberMutex mutex;
    FiberCondVar condVar;
    bool ping = false;

    mutex.lock();
    SILK_ASSERT(condVar.wait_for(mutex, 1'000'000) == ETIMEDOUT);
    mutex.unlock();

    std::thread waiter(
        [&]
        {
            mutex.lock();
            while (!ping)
            {
                condVar.wait(mutex);
            }
            mutex.unlock();
        });

    mutex.lock();
    ping = true;
    mutex.unlock();
    condVar.notify_one();
    waiter.join();
}

struct LastStaticDestructor
{
    bool armed = false;

    ~LastStaticDestructor()
    {
        if (armed)
        {
            pingPong();
            std::fputs("done\n", stderr);
        }
    }
};

__attribute__((init_priority(101))) LastStaticDestructor lastStaticDestructor;

} // namespace

TEST(FiberThreadsDeathTest, usableInLastStaticDestructor)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            lastStaticDestructor.armed = true;
            std::exit(0);
        },
        ::testing::ExitedWithCode(0),
        "done");
}

} // namespace silk
