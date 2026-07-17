#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>
#include <silk/util/tsc.h>

#include <gtest/gtest.h>

namespace silk
{

// Basic sleep: fiber sleeps for 10ms and wakes normally.
TEST(FiberSleep, sleep)
{
    static constexpr uint64_t WAIT_NS = 10'000'000; // 10ms

    struct Params
    {
        static int fiberMain(Params *) noexcept
        {
            uint64_t before = Tsc::getCycles();
            FiberScheduler::sleep(WAIT_NS);
            uint64_t elapsedNs = Tsc::cyclesToNanoseconds(Tsc::getCycles() - before);
            EXPECT_GE(elapsedNs, WAIT_NS);
            return 0;
        }
    };

    int r = FiberScheduler::run(Params::fiberMain, {});
    ASSERT_EQ(r, 0);
}

// Async sleep API: future completes with 0 on normal expiry.
TEST(FiberSleep, sleepExpiry)
{
    static constexpr uint64_t WAIT_NS = 1'000'000; // 1ms

    struct Params
    {
        static int fiberMain(Params *) noexcept
        {
            FiberScheduler::SleepFuture future;
            FiberScheduler::sleep(WAIT_NS, &future);
            int r = future.wait();
            EXPECT_EQ(r, 0);
            return 0;
        }
    };

    int r = FiberScheduler::run(Params::fiberMain, {});
    ASSERT_EQ(r, 0);
}

// Cancel before the entry is inserted into the sleep tree (cancel-before-insert
// path): the same fiber calls cancel() immediately after sleep(), before
// suspending on wait().  handleSleepQueue sees CANCELLED during phase 1 and
// completes the future without inserting it into the tree.
TEST(FiberSleep, cancelSleep)
{
    static constexpr uint64_t WAIT_NS = 60'000'000'000; // 60s

    struct Params
    {
        static int fiberMain(Params *) noexcept
        {
            FiberScheduler::SleepFuture future;
            FiberScheduler::sleep(WAIT_NS, &future);
            future.cancel();
            int r = future.wait();
            EXPECT_EQ(r, ECANCELED);
            return 0;
        }
    };

    int r = FiberScheduler::run(Params::fiberMain, {});
    ASSERT_EQ(r, 0);
}

// Cancel before sleep() is called: cancel() sets CANCELLED before the future is
// even registered; sleep() must detect this and complete immediately with ECANCELED
// rather than registering in the sleep tree.
TEST(FiberSleep, cancelSleepBeforeRegister)
{
    static constexpr uint64_t WAIT_NS = 60'000'000'000; // 60s

    FiberScheduler::SleepFuture future;
    future.cancel();
    FiberScheduler::sleep(WAIT_NS, &future);
    int err;
    ASSERT_TRUE(future.isSet(&err));
    ASSERT_EQ(err, ECANCELED);
}

// Cancel after the entry is in the sleep tree (cancel-after-insert path): the
// sleeper suspends on wait(), which lets the scheduler run handleSleepQueue and
// set IN_TABLE before the canceller fiber executes.
TEST(FiberSleep, cancelSleepAfterInsert)
{
    static constexpr uint64_t WAIT_NS = 60'000'000'000; // 60s

    struct Params
    {
        FiberScheduler::SleepFuture * future;

        static int sleeperMain(Params * p) noexcept
        {
            FiberScheduler::sleep(WAIT_NS, p->future);
            return p->future->wait();
        }

        static int cancellerMain(Params * p) noexcept
        {
            p->future->cancel();
            return 0;
        }
    };

    FiberScheduler::SleepFuture sleepFuture;
    FiberFuture sleeper, canceller;
    int r = FiberScheduler::run(Params::sleeperMain, {&sleepFuture}, &sleeper);
    ASSERT_FALSE(r);
    r = FiberScheduler::run(Params::cancellerMain, {&sleepFuture}, &canceller);
    ASSERT_FALSE(r);

    canceller.wait();
    r = sleeper.wait();
    ASSERT_EQ(r, ECANCELED);
}

// SleepFuture reuse: reset() between calls allows the same future to be used
// for successive sleeps.
TEST(FiberSleep, sleepReuse)
{
    static constexpr uint64_t WAIT_NS = 1'000'000; // 1ms

    struct Params
    {
        static int fiberMain(Params *) noexcept
        {
            FiberScheduler::SleepFuture future;
            for (int i = 0; i < 3; ++i)
            {
                FiberScheduler::sleep(WAIT_NS, &future);
                int r = future.wait();
                EXPECT_EQ(r, 0);
                future.reset();
            }
            return 0;
        }
    };

    int r = FiberScheduler::run(Params::fiberMain, {});
    ASSERT_EQ(r, 0);
}

// A sleep with a deadline LONGER than a pending one must not delay the shorter sleep's wakeup.
TEST(FiberSleep, sleepLongerDeadlineSkipped)
{
    static constexpr uint64_t SHORT_NS = 20'000'000; //  20ms
    static constexpr uint64_t LONG_NS = 200'000'000; // 200ms

    struct Params
    {
        uint64_t sleepNs;

        static int fiberMain(Params * p) noexcept
        {
            FiberScheduler::sleep(p->sleepNs);
            return 0;
        }
    };

    // Arm the scheduler with a SHORT timeout first.
    FiberFuture futureA;
    int r = FiberScheduler::run(Params::fiberMain, {SHORT_NS}, &futureA);
    ASSERT_FALSE(r);
    ::usleep(5'000); // let scheduler process and arm the short timeout

    // Register a LONGER sleep; it must not disturb the pending shorter wakeup.
    FiberFuture futureB;
    r = FiberScheduler::run(Params::fiberMain, {LONG_NS}, &futureB);
    ASSERT_FALSE(r);

    futureA.wait();
    futureB.wait();
}

// A fiber with a shorter deadline registered after a longer one must still wake on
// time, not sleep until the longer deadline.
TEST(FiberSleep, sleepDeadlineUpdate)
{
    static constexpr uint64_t LONG_NS = 200'000'000; // 200ms
    static constexpr uint64_t SHORT_NS = 20'000'000; //  20ms

    struct Params
    {
        uint64_t sleepNs;

        static int fiberMain(Params * p) noexcept
        {
            FiberScheduler::sleep(p->sleepNs);
            return 0;
        }
    };

    // Arm the scheduler's wakeup timeout with LONG_NS.
    FiberFuture futureA;
    int r = FiberScheduler::run(Params::fiberMain, {LONG_NS}, &futureA);
    ASSERT_FALSE(r);

    // Wait until the scheduler has processed the sleep entry and parked for 200ms.
    ::usleep(5'000);

    // Register a shorter sleep.  futureB must wake after SHORT_NS, not LONG_NS.
    uint64_t t0 = Tsc::getCycles();
    FiberFuture futureB;
    r = FiberScheduler::run(Params::fiberMain, {SHORT_NS}, &futureB);
    ASSERT_FALSE(r);
    futureB.wait();
    uint64_t elapsedNs = Tsc::cyclesToNanoseconds(Tsc::getCycles() - t0);

    EXPECT_GE(elapsedNs, SHORT_NS);
    EXPECT_LT(elapsedNs, LONG_NS);

    futureA.wait();
}

// Multiple fibers sleeping concurrently: all must wake after their deadline.
TEST(FiberSleep, sleepConcurrent)
{
    static constexpr int N = 8;
    static constexpr uint64_t WAIT_NS = 10'000'000; // 10ms

    struct Params
    {
        int index;

        static int fiberMain(Params * p) noexcept
        {
            uint64_t before = Tsc::getCycles();
            FiberScheduler::sleep(WAIT_NS);
            uint64_t elapsedNs = Tsc::cyclesToNanoseconds(Tsc::getCycles() - before);
            EXPECT_GE(elapsedNs, WAIT_NS);
            return p->index;
        }
    };

    FiberFuture futures[N];
    for (int i = 0; i < N; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, {i}, &futures[i]);
        ASSERT_FALSE(r);
    }
    for (int i = 0; i < N; ++i)
    {
        int r = futures[i].wait();
        ASSERT_EQ(r, i);
    }
}

} // namespace silk
