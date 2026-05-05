#include <silk/fibers/futex.h>

#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>

#include <gtest/gtest.h>

namespace silk
{

TEST(FiberFutex, get)
{
    FiberFutex event;
    ASSERT_EQ(event.get(), 0u);
    event.post();
    ASSERT_EQ(event.get(), 1u);
    event.post();
    ASSERT_EQ(event.get(), 2u);
}

// wait() with no arguments waits for the NEXT post from the current counter.
TEST(FiberFutex, waitNoArg)
{
    struct Params
    {
        FiberFutex * event;
        FiberFuture * waiting;

        static int fiberMain(Params * p) noexcept
        {
            // Capture token before signaling waiting; otherwise post() can fire
            // between set() and wait(), causing wait() to block on token+1 forever.
            uint64_t token = p->event->get() + 1;
            p->waiting->set(0);
            p->event->wait(token);
            return 0;
        }
    };

    FiberFutex event;
    FiberFuture future, waiting;
    int r = FiberScheduler::run(Params::fiberMain, {&event, &waiting}, &future);
    ASSERT_FALSE(r);

    waiting.wait();
    event.post();
    future.wait();
}

TEST(FiberFutex, waitAlreadyFired)
{
    FiberFutex event;
    event.post();

    uint64_t token = event.get();
    event.wait(token);
}

TEST(FiberFutex, waitSuspended)
{
    struct WaiterParams
    {
        FiberFutex * event;
        uint64_t token;
        FiberFuture * waiting;
        FiberFuture * done;

        static int fiberMain(WaiterParams * p) noexcept
        {
            p->waiting->set(0);
            p->event->wait(p->token);
            p->done->set(0);
            return 0;
        }
    };

    FiberFutex event;
    uint64_t token = event.get() + 1;

    FiberFuture future, waiting, done;
    int r = FiberScheduler::run(WaiterParams::fiberMain, {&event, token, &waiting, &done}, &future);
    ASSERT_FALSE(r);

    waiting.wait();
    event.post();
    done.wait();

    future.wait();
}

TEST(FiberFutex, multipleWaiters)
{
    static constexpr int N = 4;

    struct Params
    {
        FiberFutex * event;
        FiberFuture * ready;
        FiberFuture * done;

        static int fiberMain(Params * p) noexcept
        {
            uint64_t token = p->event->get() + 1;
            p->ready->set(0);
            p->event->wait(token);
            p->done->set(0);
            return 0;
        }
    };

    FiberFutex event;
    FiberFuture futures[N], ready[N], done[N];

    for (int i = 0; i < N; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, {&event, &ready[i], &done[i]}, &futures[i]);
        ASSERT_FALSE(r);
    }

    for (int i = 0; i < N; ++i)
    {
        ready[i].wait();
    }

    event.post();

    for (int i = 0; i < N; ++i)
    {
        done[i].wait();
        futures[i].wait();
    }
}

TEST(FiberFutex, multiplePost)
{
    static constexpr int N_ITER = 8;

    struct Params
    {
        FiberFutex * event;
        FiberFuture * done;

        static int fiberMain(Params * p) noexcept
        {
            for (uint64_t i = 0; i < N_ITER; ++i)
            {
                p->event->wait(i + 1);
            }
            p->done->set(0);
            return 0;
        }
    };

    FiberFutex event;
    FiberFuture future, done;

    int r = FiberScheduler::run(Params::fiberMain, {&event, &done}, &future);
    ASSERT_FALSE(r);

    for (int i = 0; i < N_ITER; ++i)
    {
        event.post();
    }

    done.wait();
    future.wait();
}

// Stress: N fibers post concurrently; verify no increment is lost.
// The test thread waits for each token in sequence; if any post were
// dropped the final wait would hang.
TEST(FiberFutex, concurrentPostStress)
{
    static constexpr int N = 8;
    static constexpr int ITER = 200;

    struct Producer
    {
        FiberFutex * event;

        static int fiberMain(Producer * p) noexcept
        {
            for (int i = 0; i < ITER; ++i)
            {
                p->event->post();
            }
            return 0;
        }
    };

    FiberFutex event;
    FiberFuture producers[N];
    for (int i = 0; i < N; ++i)
    {
        int r = FiberScheduler::run(Producer::fiberMain, {&event}, &producers[i]);
        ASSERT_FALSE(r);
    }

    // Each wait(i) returns as soon as counter >= i. Posts may arrive in
    // bursts so multiple waits may return immediately in a row.
    for (int i = 1; i <= N * ITER; ++i)
    {
        event.wait(uint64_t(i));
    }

    for (int i = 0; i < N; ++i)
    {
        producers[i].wait();
    }
}

} // namespace silk
