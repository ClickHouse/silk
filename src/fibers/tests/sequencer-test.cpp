#include <silk/fibers/sequencer.h>

#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>

#include <gtest/gtest.h>

namespace silk
{

TEST(FiberSequencer, incrementReturnValue)
{
    FiberSequencer sequencer;
    ASSERT_EQ(sequencer.get(), 0u);
    ASSERT_EQ(sequencer.increment(), 1u);
    ASSERT_EQ(sequencer.increment(), 2u);
    ASSERT_EQ(sequencer.get(), 2u);
}

TEST(FiberSequencer, waitAlreadySatisfied)
{
    FiberSequencer sequencer;
    sequencer.increment();

    FiberSequencer::Future future;
    sequencer.wait(1, &future);

    // counter >= token: future must be set immediately, no suspension
    int err;
    EXPECT_TRUE(future.isSet(&err));
    EXPECT_EQ(err, 0);

    // blocking form; must return immediately
    sequencer.wait(1);
}

TEST(FiberSequencer, waitSuspends)
{
    struct WaiterParams
    {
        FiberSequencer * sequencer;
        FiberFuture * waiting;
        FiberFuture * done;

        static int fiberMain(WaiterParams * p) noexcept
        {
            FiberSequencer::Future future;
            p->sequencer->wait(1, &future);
            p->waiting->set(0);
            future.wait();
            p->done->set(0);
            return 0;
        }
    };

    FiberSequencer sequencer;
    FiberFuture future, waiting, done;
    int r = FiberScheduler::run(WaiterParams::fiberMain, {&sequencer, &waiting, &done}, &future);
    ASSERT_FALSE(r);

    waiting.wait();
    sequencer.increment();
    done.wait();

    future.wait();
}

TEST(FiberSequencer, waitBlockingSuspends)
{
    struct WaiterParams
    {
        FiberSequencer * sequencer;
        FiberFuture * waiting;
        FiberFuture * done;

        static int fiberMain(WaiterParams * p) noexcept
        {
            p->waiting->set(0);
            p->sequencer->wait(1); // blocking; must suspend then wake
            p->done->set(0);
            return 0;
        }
    };

    FiberSequencer sequencer;
    FiberFuture future, waiting, done;
    int r = FiberScheduler::run(WaiterParams::fiberMain, {&sequencer, &waiting, &done}, &future);
    ASSERT_FALSE(r);

    waiting.wait();
    sequencer.increment();
    done.wait();

    future.wait();
}

TEST(FiberSequencer, multipleWaiters)
{
    static constexpr int N = 4;

    struct Params
    {
        FiberSequencer * sequencer;
        FiberFuture * ready;
        FiberFuture * done;

        static int fiberMain(Params * p) noexcept
        {
            FiberSequencer::Future future;
            p->sequencer->wait(1, &future);
            p->ready->set(0);
            future.wait();
            p->done->set(0);
            return 0;
        }
    };

    FiberSequencer sequencer;
    FiberFuture futures[N], ready[N], done[N];

    for (int i = 0; i < N; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, {&sequencer, &ready[i], &done[i]}, &futures[i]);
        ASSERT_FALSE(r);
    }
    for (int i = 0; i < N; ++i)
    {
        ready[i].wait();
    }

    sequencer.increment();

    for (int i = 0; i < N; ++i)
    {
        done[i].wait();
        futures[i].wait();
    }
}

TEST(FiberSequencer, differentTokens)
{
    struct Params
    {
        FiberSequencer * sequencer;
        uint64_t token;
        FiberFuture * ready;
        FiberFuture * done;

        static int fiberMain(Params * p) noexcept
        {
            FiberSequencer::Future future;
            p->sequencer->wait(p->token, &future);
            p->ready->set(0);
            future.wait();
            p->done->set(0);
            return 0;
        }
    };

    FiberSequencer sequencer;
    FiberFuture futures[3], ready[3], done[3];

    for (int i = 0; i < 3; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, {&sequencer, uint64_t(i + 1), &ready[i], &done[i]}, &futures[i]);
        ASSERT_FALSE(r);
        ready[i].wait();
    }

    // first increment: only token=1 waiter wakes
    sequencer.increment();
    done[0].wait();
    futures[0].wait();
    int err;
    ASSERT_FALSE(done[1].isSet(&err));
    ASSERT_FALSE(done[2].isSet(&err));

    // second increment: only token=2 waiter wakes
    sequencer.increment();
    done[1].wait();
    futures[1].wait();
    ASSERT_FALSE(done[2].isSet(&err));

    sequencer.increment();
    done[2].wait();
    futures[2].wait();
}

TEST(FiberSequencer, cancelDirectly)
{
    struct Params
    {
        FiberSequencer * sequencer;
        FiberSequencer::Future * future;
        FiberFuture * registered;
        FiberFuture * done;

        static int fiberMain(Params * p) noexcept
        {
            p->sequencer->wait(1, p->future);
            p->registered->set(0);
            p->future->wait();
            int err;
            EXPECT_TRUE(p->future->isSet(&err));
            EXPECT_EQ(err, ECANCELED);
            p->done->set(0);
            return 0;
        }
    };

    FiberSequencer sequencer;
    FiberSequencer::Future seqFuture;
    FiberFuture future, registered, done;
    int r = FiberScheduler::run(Params::fiberMain, {&sequencer, &seqFuture, &registered, &done}, &future);
    ASSERT_FALSE(r);

    registered.wait();
    seqFuture.cancel();
    done.wait();
    future.wait();
}

// cancelWait after IN_TABLE: the future has been promoted to the combiner's
// tree before cancel is called. Tests the cancelQueue drain path in drain().
TEST(FiberSequencer, cancelAfterInTable)
{
    struct Waiter
    {
        FiberSequencer * sequencer;
        FiberSequencer::Future * future;
        FiberFuture * inTable;
        FiberFuture * done;

        static int fiberMain(Waiter * p) noexcept
        {
            // Register for a token far in the future so we don't complete naturally.
            p->sequencer->wait(1000, p->future);

            // Increment once: drain() will promote our future from requestQueue
            // into the waiter tree (setting IN_TABLE) without satisfying it.
            p->sequencer->increment();
            p->inTable->set(0);

            // Now cancel while IN_TABLE is set.
            p->future->wait();
            int err;
            EXPECT_TRUE(p->future->isSet(&err));
            EXPECT_EQ(err, ECANCELED);
            p->done->set(0);
            return 0;
        }
    };

    struct Canceller
    {
        FiberSequencer::Future * future;
        FiberFuture * inTable;

        static int fiberMain(Canceller * p) noexcept
        {
            p->inTable->wait();
            p->future->cancel();
            return 0;
        }
    };

    FiberSequencer sequencer;
    FiberSequencer::Future seqFuture;
    FiberFuture future, inTable, done;
    FiberFuture canceller;

    int r = FiberScheduler::run(Waiter::fiberMain, {&sequencer, &seqFuture, &inTable, &done}, &future);
    ASSERT_FALSE(r);
    r = FiberScheduler::run(Canceller::fiberMain, {&seqFuture, &inTable}, &canceller);
    ASSERT_FALSE(r);

    done.wait();
    future.wait();
    canceller.wait();
}

TEST(FiberSequencer, cancelAlreadySatisfied)
{
    struct Params
    {
        FiberSequencer * sequencer;
        FiberSequencer::Future * future;
        FiberFuture * done;

        static int fiberMain(Params * p) noexcept
        {
            p->sequencer->increment();
            p->sequencer->wait(1, p->future); // satisfied immediately
            p->future->cancel(); // no-op: already set
            int err;
            EXPECT_TRUE(p->future->isSet(&err));
            EXPECT_EQ(err, 0); // set with 0, not ECANCELED
            p->done->set(0);
            return 0;
        }
    };

    FiberSequencer sequencer;
    FiberSequencer::Future seqFuture;
    FiberFuture future, done;
    int r = FiberScheduler::run(Params::fiberMain, {&sequencer, &seqFuture, &done}, &future);
    ASSERT_FALSE(r);
    done.wait();
    future.wait();
}

// Concurrent incrementers: N fibers all call increment() simultaneously,
// stressing the combiner PENDING state and the drain loop's repeat path.
// A waiter registered before the storm must wake exactly once.
TEST(FiberSequencer, concurrentIncrement)
{
    static constexpr int N = 8;
    static constexpr int ITER = 100;

    struct Incrementer
    {
        FiberSequencer * sequencer;
        FiberFuture * ready;

        static int fiberMain(Incrementer * p) noexcept
        {
            p->ready->set(0);
            for (int i = 0; i < ITER; ++i)
            {
                p->sequencer->increment();
            }
            return 0;
        }
    };

    FiberSequencer sequencer;
    FiberFuture ready[N], futures[N];

    // Register a waiter before any increments so the push→race path is exercised.
    FiberSequencer::Future waiterFuture;
    sequencer.wait(1, &waiterFuture);

    for (int i = 0; i < N; ++i)
    {
        int r = FiberScheduler::run(Incrementer::fiberMain, {&sequencer, &ready[i]}, &futures[i]);
        ASSERT_FALSE(r);
    }
    for (int i = 0; i < N; ++i)
    {
        ready[i].wait();
    }
    for (int i = 0; i < N; ++i)
    {
        futures[i].wait();
    }

    waiterFuture.wait();
    ASSERT_EQ(sequencer.get(), uint64_t(N * ITER));
}

// wait for token 0 on a fresh sequencer: counter(0) >= token(0), so the
// future must be satisfied immediately without suspending.
TEST(FiberSequencer, waitForTokenZero)
{
    FiberSequencer sequencer;
    FiberSequencer::Future future;
    sequencer.wait(0, &future);
    int err;
    EXPECT_TRUE(future.isSet(&err));
    EXPECT_EQ(err, 0);
}

// advance to a value <= current counter is a no-op; returns false.
TEST(FiberSequencer, advanceNoOp)
{
    FiberSequencer sequencer;
    sequencer.increment(); // counter = 1
    sequencer.increment(); // counter = 2

    ASSERT_FALSE(sequencer.advance(2)); // equal: no-op
    ASSERT_FALSE(sequencer.advance(1)); // less: no-op
    ASSERT_EQ(sequencer.get(), 2u);
}

// advance to a value > current counter moves the counter and returns true.
TEST(FiberSequencer, advanceForward)
{
    FiberSequencer sequencer;
    ASSERT_TRUE(sequencer.advance(5));
    ASSERT_EQ(sequencer.get(), 5u);

    ASSERT_FALSE(sequencer.advance(3)); // regression: no-op
    ASSERT_EQ(sequencer.get(), 5u);
}

// advance wakes a waiter whose token is now satisfied.
TEST(FiberSequencer, advanceWakesWaiter)
{
    struct Params
    {
        FiberSequencer * sequencer;
        FiberFuture * ready;
        FiberFuture * done;

        static int fiberMain(Params * p) noexcept
        {
            FiberSequencer::Future future;
            p->sequencer->wait(3, &future);
            p->ready->set(0);
            future.wait();
            p->done->set(0);
            return 0;
        }
    };

    FiberSequencer sequencer;
    FiberFuture ready, done, future;
    int r = FiberScheduler::run(Params::fiberMain, {&sequencer, &ready, &done}, &future);
    ASSERT_FALSE(r);

    ready.wait();
    ASSERT_TRUE(sequencer.advance(5)); // skips past token 3
    done.wait();
    future.wait();

    ASSERT_EQ(sequencer.get(), 5u);
}

// advance past multiple tokens at once wakes all of them.
TEST(FiberSequencer, advancePastMultipleTokens)
{
    struct Params
    {
        FiberSequencer * sequencer;
        uint64_t token;
        FiberFuture * ready;
        FiberFuture * done;

        static int fiberMain(Params * p) noexcept
        {
            FiberSequencer::Future future;
            p->sequencer->wait(p->token, &future);
            p->ready->set(0);
            future.wait();
            p->done->set(0);
            return 0;
        }
    };

    static constexpr int N = 4;
    FiberSequencer sequencer;
    FiberFuture futures[N], ready[N], done[N];

    for (int i = 0; i < N; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, {&sequencer, uint64_t(i + 1), &ready[i], &done[i]}, &futures[i]);
        ASSERT_FALSE(r);
        ready[i].wait();
    }

    ASSERT_TRUE(sequencer.advance(4)); // satisfies tokens 1, 2, 3, 4 at once

    for (int i = 0; i < N; ++i)
    {
        done[i].wait();
        futures[i].wait();
    }

    ASSERT_EQ(sequencer.get(), 4u);
}

} // namespace silk
