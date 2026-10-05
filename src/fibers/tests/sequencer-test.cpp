#include <silk/fibers/sequencer.h>

#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>
#include <silk/util/assert.h>
#include <silk/util/platform.h>
#include <silk/util/sanitizers.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <sched.h>

#include <fibers/cpu.h>

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
    int r;
    EXPECT_TRUE(future.isSet(&r));
    EXPECT_EQ(r, 0);

    // blocking form; must return immediately
    EXPECT_EQ(sequencer.wait(1), 0);
}

TEST(FiberSequencer, resetRebasesBelowCounter)
{
    FiberSequencer sequencer;
    bool advanced = sequencer.advance(10);
    ASSERT_TRUE(advanced);

    sequencer.reset(3);
    ASSERT_EQ(sequencer.get(), 3u);

    // A wait at or below the rebased counter completes immediately.
    int r = sequencer.wait(3);
    ASSERT_EQ(r, 0);

    // A wait above it parks until advance reaches the token again.
    FiberSequencer::Future future;
    sequencer.wait(4, &future);
    ASSERT_FALSE(future.isSet(&r));

    advanced = sequencer.advance(4);
    ASSERT_TRUE(advanced);
    ASSERT_TRUE(future.isSet(&r));
    ASSERT_EQ(r, 0);
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

    int r;
    ASSERT_TRUE(unreached.isSet(&r));
    EXPECT_EQ(r, ECANCELED);
    ASSERT_TRUE(reached.isSet(&r));
    EXPECT_EQ(r, 0);

    // Registered after stop: an unreached wait completes with ECANCELED without suspending, a reached one with 0.
    FiberSequencer::Future late;
    sequencer.wait(2, &late);
    ASSERT_TRUE(late.isSet(&r));
    EXPECT_EQ(r, ECANCELED);
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

TEST(FiberSequencer, cancelWaitersCancelsUnreachedWaitersAndKeepsRunning)
{
    FiberSequencer sequencer;

    // Registered before the call: an unreached waiter completes with ECANCELED, whether the drain in increment has
    // moved it into the tree or it still sits in the request queue.
    FiberSequencer::Future treeResident;
    sequencer.wait(2, &treeResident);
    sequencer.increment();
    FiberSequencer::Future queued;
    sequencer.wait(2, &queued);

    sequencer.cancelWaiters();
    ASSERT_FALSE(sequencer.stopped());

    int r;
    ASSERT_TRUE(treeResident.isSet(&r));
    ASSERT_EQ(r, ECANCELED);
    ASSERT_TRUE(queued.isSet(&r));
    ASSERT_EQ(r, ECANCELED);

    // Registered after the call: an unreached wait parks as usual and completes with 0 once the counter reaches it.
    FiberSequencer::Future later;
    sequencer.wait(2, &later);
    ASSERT_FALSE(later.isSet(&r));

    uint64_t current = sequencer.increment();
    ASSERT_EQ(current, 2u);
    ASSERT_TRUE(later.isSet(&r));
    ASSERT_EQ(r, 0);
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
    int r;
    ASSERT_FALSE(done[1].isSet(&r));
    ASSERT_FALSE(done[2].isSet(&r));

    // second increment: only token=2 waiter wakes
    sequencer.increment();
    done[1].wait();
    futures[1].wait();
    ASSERT_FALSE(done[2].isSet(&r));

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
            int r;
            EXPECT_TRUE(p->future->isSet(&r));
            EXPECT_EQ(r, ECANCELED);
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
            int r;
            EXPECT_TRUE(p->future->isSet(&r));
            EXPECT_EQ(r, 0); // set with 0, not ECANCELED
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
    int r;
    EXPECT_TRUE(future.isSet(&r));
    EXPECT_EQ(r, 0);
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
    struct IncrementerParams
    {
        uint64_t iterations;
        const uint16_t * cpus;
        uint32_t cpuCount;
        std::atomic<uint64_t> go{0};
        std::atomic<uint32_t> done{0};
        FiberSequencer * sequencer = nullptr;

        static void threadMain(IncrementerParams * params, uint32_t threadIndex) noexcept
        {
            int r = pinThreadToCpu(params->cpus[threadIndex % params->cpuCount]);
            SILK_ASSERT(r == 0);

            for (uint64_t i = 1; i <= params->iterations; ++i)
            {
                while (params->go.load(std::memory_order_acquire) != i)
                {
                    cpuPause();
                }

                params->sequencer->increment();
                params->done.fetch_add(1, std::memory_order_release);
            }
        }
    };

    cpu_set_t affinity;
    int r = sched_getaffinity(0, sizeof(cpu_set_t), &affinity);
    ASSERT_EQ(r, 0);

    uint16_t cpus[CPU_SETSIZE];
    uint32_t cpuCount = 0;
    for (uint16_t cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    {
        if (CPU_ISSET(cpu, &affinity))
        {
            cpus[cpuCount++] = cpu;
        }
    }

    if (cpuCount < 3)
    {
        GTEST_SKIP() << "needs >= 3 cores to contend for the combiner";
    }
    const uint32_t numThreads = std::min<uint32_t>(8, cpuCount); // incrementers, and the waiter's (final) token

    uint64_t iterations = 200'000;
#if defined(__SANITIZE_THREAD__)
    // The high count is what reproduces the StoreLoad reordering this test
    // asserts on, but TSan cannot observe that reordering: it detects data races
    // through a happens-before model, and a missing StoreLoad fence between
    // atomics is a memory-model bug, not a race. So the full count buys no
    // coverage under TSan while costing about 6 ms per iteration on an 8-CPU box,
    // pushing the run past the ctest timeout. A small count still drives the same
    // paths for TSan's race detection, which saturates in far fewer iterations.
    iterations = 2'000;
#endif
    if (const char * env = std::getenv("SILK_SEQ_LITMUS_ITERS"))
    {
        iterations = std::strtoull(env, nullptr, 10);
    }

    IncrementerParams params{iterations, cpus, cpuCount};

    std::vector<std::thread> threads;
    for (uint32_t threadIndex = 0; threadIndex < numThreads; ++threadIndex)
    {
        threads.emplace_back(IncrementerParams::threadMain, &params, threadIndex);
    }

    uint64_t lost = 0;
    for (uint64_t i = 1; i <= iterations; ++i)
    {
        FiberSequencer sequencer;
        FiberSequencer::Future future;
        // Token == final counter value: the waiter's wakeup rests on the last of the contended increments,
        // so a wakeup lost in that drain is never repaired by a later one.
        sequencer.wait(numThreads, &future);
        params.sequencer = &sequencer;
        params.done.store(0, std::memory_order_relaxed);
        params.go.store(i, std::memory_order_release); // release publishes the fresh sequencer to the incrementers
        while (params.done.load(std::memory_order_acquire) != numThreads)
        {
            cpuPause();
        }

        int r;
        if (!future.isSet(&r))
        {
            ++lost; // counter reached the token but the waiter was never woken
            // The lost future is still linked in the sequencer's tree/queue; unlink it (all incrementers are
            // parked, so this drain is uncontended) before it is destroyed, else safe_link asserts in debug.
            future.cancel();
        }
        // sequencer and future are destroyed here; all incrementers are parked on go for the next iteration.
    }

    for (std::thread & thread : threads)
    {
        thread.join();
    }

    RecordProperty("iterations", std::to_string(iterations));
    RecordProperty("threads", std::to_string(numThreads));
    RecordProperty("lost_wakeups", std::to_string(lost));
    ASSERT_EQ(lost, 0u) << lost << " permanent lost wakeups over " << iterations << " iterations with " << numThreads
                        << " contending incrementers";
}

// cancelWaiters returns only once its flush is done, even while a burst of increments on other threads keeps a combiner
// running: every unreached future registered before the call is set by the time it returns, never left to a pass the
// call merely signalled. A future at a reachable token that an increment completes first completes through that
// increment, possibly after the return, and never stays pending. The burst is finite, so every combiner loop ends.
TEST(FiberSequencer, cancelWaitersFlushesBeforeItReturns)
{
    static constexpr uint32_t INCREMENT_BURST = 64;

    struct IncrementerParams
    {
        FiberSequencer * sequencer;
        uint64_t iterations;
        const uint16_t * cpus;
        uint32_t cpuCount;
        std::atomic<uint64_t> go{0};
        std::atomic<uint32_t> done{0};

        static void threadMain(IncrementerParams * params, uint32_t threadIndex) noexcept
        {
            int r = pinThreadToCpu(params->cpus[(threadIndex + 1) % params->cpuCount]);
            SILK_ASSERT(r == 0);

            for (uint64_t i = 1; i <= params->iterations; ++i)
            {
                while (params->go.load(std::memory_order_acquire) != i)
                {
                    cpuPause();
                }

                for (uint32_t increment = 0; increment < INCREMENT_BURST; ++increment)
                {
                    params->sequencer->increment();
                }

                params->done.fetch_add(1, std::memory_order_release);
            }
        }
    };

    cpu_set_t affinity;
    int r = sched_getaffinity(0, sizeof(cpu_set_t), &affinity);
    ASSERT_EQ(r, 0);

    uint16_t cpus[CPU_SETSIZE];
    uint32_t cpuCount = 0;
    for (uint16_t cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    {
        if (CPU_ISSET(cpu, &affinity))
        {
            cpus[cpuCount++] = cpu;
        }
    }

    if (cpuCount < 3)
    {
        GTEST_SKIP() << "needs >= 3 cores to keep a combiner running on another core";
    }
    const uint32_t numThreads = std::min<uint32_t>(4, cpuCount - 1);

    uint64_t iterations = 20'000;
#if defined(__SANITIZE_THREAD__)
    iterations = 1'000;
#endif

    FiberSequencer sequencer;
    IncrementerParams params{&sequencer, iterations, cpus, cpuCount};

    std::vector<std::thread> threads;
    for (uint32_t threadIndex = 0; threadIndex < numThreads; ++threadIndex)
    {
        threads.emplace_back(IncrementerParams::threadMain, &params, threadIndex);
    }

    uint64_t late = 0;
    for (uint64_t i = 1; i <= iterations; ++i)
    {
        FiberSequencer::Future unreachable[4];
        FiberSequencer::Future reachable[4];
        uint64_t token = sequencer.get() + 1;
        for (uint32_t index = 0; index < 4; ++index)
        {
            sequencer.wait(UINT64_MAX, &unreachable[index]);
            sequencer.wait(token + index, &reachable[index]);
        }

        params.done.store(0, std::memory_order_relaxed);
        params.go.store(i, std::memory_order_release);

        // Call once the burst runs, so another thread is usually the combiner.
        while (sequencer.get() < token)
        {
            cpuPause();
        }

        sequencer.cancelWaiters();

        for (FiberSequencer::Future & future : unreachable)
        {
            int r;
            if (!future.isSet(&r))
            {
                ++late;
                future.cancel();
                future.wait();
            }
        }

        for (FiberSequencer::Future & future : reachable)
        {
            int r = future.wait();
            SILK_ASSERT(r == 0 || r == ECANCELED, "r=%d", r);
        }

        while (params.done.load(std::memory_order_acquire) != numThreads)
        {
            cpuPause();
        }
    }

    for (std::thread & thread : threads)
    {
        thread.join();
    }

    RecordProperty("iterations", std::to_string(iterations));
    RecordProperty("late_flushes", std::to_string(late));
    ASSERT_EQ(late, 0u) << late << " futures still pending when cancelWaiters returned over " << iterations << " iterations";
}

// A registration racing cancelWaiters: the canceller stores a flag and issues a seq_cst fence, then calls cancelWaiters,
// and each registrar issues a seq_cst fence after its registration, re-checks the flag and cancels its own future when
// set. Every future must complete - woken by the call or cancelled by its registrar - and none stays pending because
// both missed. The canceller waits for a varying number of registrations first, so some land before the call and some
// after; raw OS threads pinned across cores make the rest genuinely race the call's drain. On x86 the locked
// instructions of the queue push and pop already order the flag, so this test cannot see a missing fence there.
TEST(FiberSequencer, cancelWaitersRacesRegistration)
{
    struct RegistrarParams
    {
        uint64_t iterations;
        const uint16_t * cpus;
        uint32_t cpuCount;
        std::atomic<uint64_t> go{0};
        std::atomic<uint32_t> registered{0};
        std::atomic<uint32_t> done{0};
        FiberSequencer * sequencer = nullptr;
        std::atomic<bool> * flag = nullptr;
        FiberSequencer::Future * futures = nullptr;

        static void threadMain(RegistrarParams * params, uint32_t threadIndex) noexcept
        {
            int r = pinThreadToCpu(params->cpus[(threadIndex + 1) % params->cpuCount]);
            SILK_ASSERT(r == 0);

            for (uint64_t i = 1; i <= params->iterations; ++i)
            {
                while (params->go.load(std::memory_order_acquire) != i)
                {
                    cpuPause();
                }

                FiberSequencer::Future * future = &params->futures[threadIndex];
                params->sequencer->wait(UINT64_MAX, future);
                params->registered.fetch_add(1, std::memory_order_release);

                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (params->flag->load(std::memory_order_relaxed))
                {
                    future->cancel();
                }

                params->done.fetch_add(1, std::memory_order_release);
            }
        }
    };

    cpu_set_t affinity;
    int r = sched_getaffinity(0, sizeof(cpu_set_t), &affinity);
    ASSERT_EQ(r, 0);

    uint16_t cpus[CPU_SETSIZE];
    uint32_t cpuCount = 0;
    for (uint16_t cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    {
        if (CPU_ISSET(cpu, &affinity))
        {
            cpus[cpuCount++] = cpu;
        }
    }

    if (cpuCount < 3)
    {
        GTEST_SKIP() << "needs >= 3 cores to race registrations against the call";
    }
    const uint32_t numThreads = std::min<uint32_t>(8, cpuCount) - 1;

    uint64_t iterations = 20'000;
#if defined(__SANITIZE_THREAD__)
    // TSan cannot observe a missing StoreLoad fence, so a small count drives the same paths for race detection.
    iterations = 1'000;
#endif

    RegistrarParams params{iterations, cpus, cpuCount};

    std::vector<std::thread> threads;
    for (uint32_t threadIndex = 0; threadIndex < numThreads; ++threadIndex)
    {
        threads.emplace_back(RegistrarParams::threadMain, &params, threadIndex);
    }

    uint64_t lost = 0;
    for (uint64_t i = 1; i <= iterations; ++i)
    {
        FiberSequencer sequencer;
        std::atomic<bool> flag{false};
        FiberSequencer::Future futures[8];

        params.sequencer = &sequencer;
        params.flag = &flag;
        params.futures = futures;
        params.registered.store(0, std::memory_order_relaxed);
        params.done.store(0, std::memory_order_relaxed);
        params.go.store(i, std::memory_order_release); // release publishes the fresh sequencer, flag and futures

        uint32_t registeredBefore = i % (numThreads + 1);
        while (params.registered.load(std::memory_order_acquire) < registeredBefore)
        {
            cpuPause();
        }

        flag.store(true, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        sequencer.cancelWaiters();

        while (params.done.load(std::memory_order_acquire) != numThreads)
        {
            cpuPause();
        }

        for (uint32_t threadIndex = 0; threadIndex < numThreads; ++threadIndex)
        {
            int r;
            if (!futures[threadIndex].isSet(&r))
            {
                ++lost;
                futures[threadIndex].cancel();
            }
        }
    }

    for (std::thread & thread : threads)
    {
        thread.join();
    }

    RecordProperty("iterations", std::to_string(iterations));
    RecordProperty("threads", std::to_string(numThreads));
    RecordProperty("lost_cancels", std::to_string(lost));
    ASSERT_EQ(lost, 0u) << lost << " futures left pending over " << iterations << " iterations with " << numThreads << " registrar threads";
}

} // namespace silk
