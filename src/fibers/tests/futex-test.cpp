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
            int r = p->event->wait(token);
            return r;
        }
    };

    FiberFutex event;
    FiberFuture future, waiting;
    int r = FiberScheduler::run(Params::fiberMain, {&event, &waiting}, &future);
    ASSERT_FALSE(r);

    waiting.wait();
    event.post();
    EXPECT_EQ(future.wait(), 0);
}

TEST(FiberFutex, waitAlreadyFired)
{
    FiberFutex event;
    event.post();

    uint64_t token = event.get();
    EXPECT_EQ(event.wait(token), 0);
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
            int r = p->event->wait(p->token);
            p->done->set(0);
            return r;
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
            int r = p->event->wait(token);
            p->done->set(0);
            return r;
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
                int r = p->event->wait(i + 1);
                if (r)
                {
                    return r;
                }
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

// stop() makes wait return ECANCELED for currently-suspended waiters.
TEST(FiberFutex, stopWakesSuspendedWaiter)
{
    struct Params
    {
        FiberFutex * event;
        FiberFuture * waiting;

        static int fiberMain(Params * p) noexcept
        {
            uint64_t token = p->event->get() + 1;
            p->waiting->set(0);
            return p->event->wait(token);
        }
    };

    FiberFutex event;
    FiberFuture future, waiting;
    int r = FiberScheduler::run(Params::fiberMain, {&event, &waiting}, &future);
    ASSERT_FALSE(r);

    waiting.wait();
    event.stop();
    EXPECT_EQ(future.wait(), ECANCELED);
    EXPECT_TRUE(event.stopped());
}

// stop() takes effect immediately; subsequent wait calls do not suspend.
TEST(FiberFutex, stopBeforeWaitReturnsImmediately)
{
    FiberFutex event;
    event.stop();

    EXPECT_TRUE(event.stopped());
    EXPECT_EQ(event.wait(event.get() + 1), ECANCELED);
}

// Calling stop twice is a no-op.
TEST(FiberFutex, stopIdempotent)
{
    FiberFutex event;
    event.stop();
    event.stop();
    EXPECT_TRUE(event.stopped());
    EXPECT_EQ(event.wait(event.get() + 1), ECANCELED);
}

// post() after stop() is inert: counter does not advance, wait still returns ECANCELED.
TEST(FiberFutex, postAfterStopIsNoOp)
{
    FiberFutex event;
    uint64_t before = event.get();
    event.stop();
    event.post();
    EXPECT_EQ(event.get(), before);
    EXPECT_EQ(event.wait(before + 1), ECANCELED);
}

// A waiter satisfied by post() right before stop() must observe success, not
// ECANCELED -- the token-satisfied case wins so callers do not lose work.
TEST(FiberFutex, satisfiedTokenBeatsStop)
{
    FiberFutex event;
    event.post();
    event.stop();
    EXPECT_EQ(event.wait(1), 0);
}

// stop() wakes every suspended fiber, not just one.
TEST(FiberFutex, stopWakesAllWaiters)
{
    static constexpr int N = 4;

    struct Params
    {
        FiberFutex * event;
        FiberFuture * ready;

        static int fiberMain(Params * p) noexcept
        {
            uint64_t token = p->event->get() + 1;
            p->ready->set(0);
            return p->event->wait(token);
        }
    };

    FiberFutex event;
    FiberFuture futures[N], ready[N];
    for (int i = 0; i < N; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, {&event, &ready[i]}, &futures[i]);
        ASSERT_FALSE(r);
    }
    for (int i = 0; i < N; ++i)
    {
        ready[i].wait();
    }

    event.stop();

    for (int i = 0; i < N; ++i)
    {
        EXPECT_EQ(futures[i].wait(), ECANCELED);
    }
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
        EXPECT_EQ(event.wait(uint64_t(i)), 0);
    }

    for (int i = 0; i < N; ++i)
    {
        producers[i].wait();
    }
}

} // namespace silk
