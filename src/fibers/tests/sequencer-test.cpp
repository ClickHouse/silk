#include <silk/fibers/sequencer.h>

#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>

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
    EXPECT_EQ(sequencer.wait(1), 0);
}

TEST(FiberSequencer, stopCancelsUnreachedWaiters)
{
    FiberSequencer sequencer;
    sequencer.increment();

    // Registered before stop: an unreached waiter completes with ECANCELED, a reached one with 0.
    FiberSequencer::Future unreached;
    sequencer.wait(2, &unreached);
    FiberSequencer::Future reached;
    sequencer.wait(1, &reached);

    EXPECT_FALSE(sequencer.stopped());
    sequencer.stop();
    EXPECT_TRUE(sequencer.stopped());

    int err;
    ASSERT_TRUE(unreached.isSet(&err));
    EXPECT_EQ(err, ECANCELED);
    ASSERT_TRUE(reached.isSet(&err));
    EXPECT_EQ(err, 0);

    // Registered after stop: an unreached wait completes with ECANCELED without suspending, a reached one with 0.
    FiberSequencer::Future late;
    sequencer.wait(2, &late);
    ASSERT_TRUE(late.isSet(&err));
    EXPECT_EQ(err, ECANCELED);
    EXPECT_EQ(sequencer.wait(2), ECANCELED);
    EXPECT_EQ(sequencer.wait(1), 0);

    // The counter keeps working after stop; a wait at the newly reached token returns 0.
    EXPECT_EQ(sequencer.increment(), 2u);
    EXPECT_EQ(sequencer.get(), 2u);
    EXPECT_EQ(sequencer.wait(2), 0);

    // Idempotent.
    sequencer.stop();
    EXPECT_TRUE(sequencer.stopped());
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

// Regression guard for the StoreLoad (Dekker) handshakes in drain() - the producer/combiner half that the
// fences in advance/increment/drain protect. Several incrementer threads contend for the single flat-combiner
// while one waiter sits at the FINAL token: the wakeup it depends on is the very last increment, so if a
// contended combiner reads a stale counter (its relaxed BUSY restore reordered past the counter re-read, or
// the advance reordered past the combinerState observe) and skips the waiter, nothing later re-wakes it and
// the loss is permanent. Registering at the final token is the crux: concurrentIncrement sits at token 1,
// which any later increment re-wakes, so it cannot see this. Driven from raw OS threads pinned across cores so
// the increments genuinely contend. This reproduces on x86 (the relaxed BUSY store is the reordering one);
// with the drain fences removed the lost-wakeup count is non-zero, with them in place it is exactly zero.
TEST(FiberSequencer, lostWakeupUnderContention)
{
    const unsigned cores = std::thread::hardware_concurrency();
    if (cores < 3)
    {
        GTEST_SKIP() << "needs >= 3 cores to contend for the combiner";
    }
    const uint32_t threads = std::min(8u, cores); // incrementers, and the waiter's (final) token

    uint64_t iters = 200'000;
    if (const char * env = std::getenv("SILK_SEQ_LITMUS_ITERS"))
    {
        iters = std::strtoull(env, nullptr, 10);
    }

    std::atomic<uint64_t> go{0};
    std::atomic<uint64_t> done{0};
    std::atomic<FiberSequencer *> seqPtr{nullptr};

    std::vector<std::thread> incrementers;
    for (uint32_t t = 0; t < threads; ++t)
    {
        incrementers.emplace_back(
            [&, t]
            {
                cpu_set_t set;
                CPU_ZERO(&set);
                CPU_SET(int(t % cores), &set);
                pthread_setaffinity_np(pthread_self(), sizeof(set), &set);

                for (uint64_t i = 1; i <= iters; ++i)
                {
                    while (go.load(std::memory_order_acquire) != i)
                    {
                    }
                    seqPtr.load(std::memory_order_relaxed)->increment();
                    done.fetch_add(1, std::memory_order_release);
                }
            });
    }

    uint64_t lost = 0;
    for (uint64_t i = 1; i <= iters; ++i)
    {
        FiberSequencer sequencer;
        FiberSequencer::Future future;
        // Token == final counter value: the waiter's wakeup rests on the last of the contended increments,
        // so a wakeup lost in that drain is never repaired by a later one.
        sequencer.wait(threads, &future);
        seqPtr.store(&sequencer, std::memory_order_relaxed);
        done.store(0, std::memory_order_relaxed);
        go.store(i, std::memory_order_release); // release publishes the fresh sequencer to the incrementers
        while (done.load(std::memory_order_acquire) != threads)
        {
        }

        int err;
        if (!future.isSet(&err))
        {
            ++lost; // counter reached the token but the waiter was never woken
            // The lost future is still linked in the sequencer's tree/queue; unlink it (all incrementers are
            // parked, so this drain is uncontended) before it is destroyed, else safe_link asserts in debug.
            future.cancel();
        }
        // sequencer and future are destroyed here; all incrementers are parked on go for the next iteration.
    }

    for (std::thread & thread : incrementers)
    {
        thread.join();
    }

    RecordProperty("iterations", std::to_string(iters));
    RecordProperty("threads", std::to_string(threads));
    RecordProperty("lost_wakeups", std::to_string(lost));
    ASSERT_EQ(lost, 0u) << lost << " permanent lost wakeups over " << iters << " iterations with " << threads << " contending incrementers";
}

} // namespace silk
