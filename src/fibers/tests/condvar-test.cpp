#include <silk/fibers/condvar.h>

#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>
#include <silk/fibers/mutex.h>

#include <gtest/gtest.h>

#include <cerrno>

namespace silk
{

namespace
{

// After @p registered fires, block until the test fiber has called cv.wait()
// and released the mutex. Locking + unlocking the mutex here proves the fiber
// is past the unlock inside wait().
void waitUntilSuspended(FiberMutex & mutex, FiberFuture & registered) noexcept
{
    registered.wait();
    mutex.lock();
    mutex.unlock();
}

} // namespace

TEST(FiberCondVar, notifyOneWakesWaiter)
{
    struct Params
    {
        FiberCondVar * cv;
        FiberMutex * mutex;
        FiberFuture * registered;

        static int fiberMain(Params * p) noexcept
        {
            p->mutex->lock();
            p->registered->set(0);
            p->cv->wait(*p->mutex);
            p->mutex->unlock();
            return 0;
        }
    };

    FiberCondVar cv;
    FiberMutex mutex;
    FiberFuture fiberFuture, registered;
    int r = FiberScheduler::run(Params::fiberMain, {&cv, &mutex, &registered}, &fiberFuture);
    ASSERT_FALSE(r);

    waitUntilSuspended(mutex, registered);

    cv.notify_one();
    fiberFuture.wait();
}

TEST(FiberCondVar, notifyAllWakesAll)
{
    static constexpr int N = 4;

    struct Params
    {
        FiberCondVar * cv;
        FiberMutex * mutex;
        FiberFuture * registered;

        static int fiberMain(Params * p) noexcept
        {
            p->mutex->lock();
            p->registered->set(0);
            p->cv->wait(*p->mutex);
            p->mutex->unlock();
            return 0;
        }
    };

    FiberCondVar cv;
    FiberMutex mutex;
    FiberFuture fiberFutures[N], registered[N];

    for (int i = 0; i < N; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, {&cv, &mutex, &registered[i]}, &fiberFutures[i]);
        ASSERT_FALSE(r);
        waitUntilSuspended(mutex, registered[i]);
    }

    cv.notify_all();

    for (int i = 0; i < N; ++i)
    {
        fiberFutures[i].wait();
    }
}

TEST(FiberCondVar, notifyOneWakesOneAtATime)
{
    static constexpr int N = 4;

    struct Params
    {
        FiberCondVar * cv;
        FiberMutex * mutex;
        FiberFuture * registered;

        static int fiberMain(Params * p) noexcept
        {
            p->mutex->lock();
            p->registered->set(0);
            p->cv->wait(*p->mutex);
            p->mutex->unlock();
            return 0;
        }
    };

    FiberCondVar cv;
    FiberMutex mutex;
    FiberFuture fiberFutures[N], registered[N];

    for (int i = 0; i < N; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, {&cv, &mutex, &registered[i]}, &fiberFutures[i]);
        ASSERT_FALSE(r);
        waitUntilSuspended(mutex, registered[i]);
    }

    for (int i = 0; i < N; ++i)
    {
        cv.notify_one();
        fiberFutures[i].wait();
    }
}

TEST(FiberCondVar, waitReacquiresMutex)
{
    struct Params
    {
        FiberCondVar * cv;
        FiberMutex * mutex;
        FiberFuture * registered;
        FiberFuture * done;

        static int fiberMain(Params * p) noexcept
        {
            p->mutex->lock();
            p->registered->set(0);
            p->cv->wait(*p->mutex);
            // Mutex must be re-held on return: unlocking proves it.
            p->mutex->unlock();
            p->done->set(0);
            return 0;
        }
    };

    FiberCondVar cv;
    FiberMutex mutex;
    FiberFuture fiberFuture, registered, done;
    int r = FiberScheduler::run(Params::fiberMain, {&cv, &mutex, &registered, &done}, &fiberFuture);
    ASSERT_FALSE(r);

    waitUntilSuspended(mutex, registered);

    cv.notify_one();
    done.wait();
    fiberFuture.wait();
}

TEST(FiberCondVar, notifyOneNoWaiterNoCrash)
{
    FiberCondVar cv;
    cv.notify_one();
}

TEST(FiberCondVar, notifyAllNoWaiterNoCrash)
{
    FiberCondVar cv;
    cv.notify_all();
}

TEST(FiberCondVar, syncWaitWithStdUniqueLock)
{
    struct Params
    {
        FiberCondVar * cv;
        FiberMutex * mutex;
        FiberFuture * registered;

        static int fiberMain(Params * p) noexcept
        {
            std::unique_lock lock(*p->mutex);
            p->registered->set(0);
            p->cv->wait(lock);
            return 0;
        }
    };

    FiberCondVar cv;
    FiberMutex mutex;
    FiberFuture fiberFuture, registered;
    int r = FiberScheduler::run(Params::fiberMain, {&cv, &mutex, &registered}, &fiberFuture);
    ASSERT_FALSE(r);

    waitUntilSuspended(mutex, registered);

    cv.notify_one();
    fiberFuture.wait();
}

TEST(FiberCondVar, waitForTimesOut)
{
    struct Params
    {
        FiberCondVar * cv;
        FiberMutex * mutex;

        static int fiberMain(Params * p) noexcept
        {
            std::unique_lock lock(*p->mutex);
            return p->cv->wait_for(lock, 10'000'000); // 10 ms
        }
    };

    FiberCondVar cv;
    FiberMutex mutex;
    FiberFuture fiberFuture;
    int r = FiberScheduler::run(Params::fiberMain, {&cv, &mutex}, &fiberFuture);
    ASSERT_FALSE(r);
    EXPECT_EQ(fiberFuture.wait(), ETIMEDOUT);
}

TEST(FiberCondVar, waitForNotifyBeforeTimeout)
{
    struct Params
    {
        FiberCondVar * cv;
        FiberMutex * mutex;
        FiberFuture * registered;

        static int fiberMain(Params * p) noexcept
        {
            std::unique_lock lock(*p->mutex);
            p->registered->set(0);
            return p->cv->wait_for(lock, 60'000'000'000ull); // 60 s
        }
    };

    FiberCondVar cv;
    FiberMutex mutex;
    FiberFuture fiberFuture, registered;
    int r = FiberScheduler::run(Params::fiberMain, {&cv, &mutex, &registered}, &fiberFuture);
    ASSERT_FALSE(r);

    waitUntilSuspended(mutex, registered);

    cv.notify_one();
    EXPECT_EQ(fiberFuture.wait(), 0);
}

TEST(FiberCondVar, predicateLoop)
{
    static constexpr int TOTAL = 16;

    struct Shared
    {
        FiberCondVar cv;
        FiberMutex mutex;
        int produced = 0;
        int consumed = 0;
    };

    struct ConsumerParams
    {
        Shared * shared;

        static int fiberMain(ConsumerParams * p) noexcept
        {
            Shared * s = p->shared;
            while (true)
            {
                std::unique_lock lock(s->mutex);
                while (s->produced == s->consumed)
                {
                    s->cv.wait(lock);
                }
                ++s->consumed;
                if (s->consumed == TOTAL)
                {
                    return 0;
                }
            }
        }
    };

    Shared shared;
    FiberFuture consumerDone;
    int r = FiberScheduler::run(ConsumerParams::fiberMain, {&shared}, &consumerDone);
    ASSERT_FALSE(r);

    for (int i = 0; i < TOTAL; ++i)
    {
        {
            std::unique_lock lock(shared.mutex);
            ++shared.produced;
        }
        shared.cv.notify_one();
    }

    consumerDone.wait();
    EXPECT_EQ(shared.consumed, TOTAL);
}

// Stress the cancel-vs-notify race: many waiters call wait_for with short
// timeouts so most will hit the timeout path and self-cancel, while a producer
// fiber hammers notify_all in parallel. Without the inWaiters flag this would
// race on Future::listEntry (cancel removing from waiters while notify_all
// iterates a splice into its own snapshot) — TSAN would catch it.
TEST(FiberCondVar, waitForCancellationRace)
{
    static constexpr int N_WAITERS = 8;
    static constexpr int ITERATIONS = 256;

    struct Shared
    {
        FiberCondVar cv;
        FiberMutex mutex;
        std::atomic<int> completed{0};
    };

    struct WaiterParams
    {
        Shared * shared;

        static int fiberMain(WaiterParams * p) noexcept
        {
            Shared * s = p->shared;
            for (int i = 0; i < ITERATIONS; ++i)
            {
                std::unique_lock lock(s->mutex);
                (void)s->cv.wait_for(lock, 1'000); // 1 us; usually times out
            }
            s->completed.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
    };

    struct NotifierParams
    {
        Shared * shared;

        static int fiberMain(NotifierParams * p) noexcept
        {
            Shared * s = p->shared;
            while (s->completed.load(std::memory_order_relaxed) < N_WAITERS)
            {
                s->cv.notify_all();
                FiberScheduler::yield();
            }
            return 0;
        }
    };

    Shared shared;
    FiberFuture waiters[N_WAITERS];
    FiberFuture notifier;

    for (int i = 0; i < N_WAITERS; ++i)
    {
        int r = FiberScheduler::run(WaiterParams::fiberMain, {&shared}, &waiters[i]);
        ASSERT_FALSE(r);
    }
    int r = FiberScheduler::run(NotifierParams::fiberMain, {&shared}, &notifier);
    ASSERT_FALSE(r);

    for (int i = 0; i < N_WAITERS; ++i)
    {
        waiters[i].wait();
    }
    notifier.wait();

    EXPECT_EQ(shared.completed.load(), N_WAITERS);
}

} // namespace silk
