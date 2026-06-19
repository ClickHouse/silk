#include <silk/fibers/multi-lock.h>

#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>

#include <gtest/gtest.h>

#include <cstdint>

namespace silk
{

// Holds three distinct keys at once from the calling thread. Distinct keys never contend, so each lock
// returns without parking; were they not independent the second lock would suspend forever and the test
// would hang.
TEST(FiberMultiLock, distinctKeysProceedIndependently)
{
    FiberMultiLock multiLock;

    FiberMultiLock::ScopedLock scopedLock1;
    multiLock.lock(1, &scopedLock1);

    FiberMultiLock::ScopedLock scopedLock2;
    multiLock.lock(2, &scopedLock2);

    FiberMultiLock::ScopedLock scopedLock3;
    multiLock.lock(3, &scopedLock3);
}

// Acquires a key, releases it via scope exit, then acquires it again. The second lock can only succeed if
// the first release handed the key back; otherwise it parks forever and the test hangs.
TEST(FiberMultiLock, sameKeyReacquiredAfterRelease)
{
    FiberMultiLock multiLock;

    {
        FiberMultiLock::ScopedLock scopedLock;
        multiLock.lock(7, &scopedLock);
    }

    FiberMultiLock::ScopedLock scopedLock;
    multiLock.lock(7, &scopedLock);
}

// try_lock grants a free key without suspending, rejects a key already held (non-reentrant, so even the
// same caller is refused), and a rejected handle releases nothing - so the key can be taken again once
// the holder's scope exits.
TEST(FiberMultiLock, tryLock)
{
    FiberMultiLock multiLock;

    bool freeAcquired;
    bool heldRejected;
    {
        FiberMultiLock::ScopedLock held;
        freeAcquired = multiLock.try_lock(5, &held);

        FiberMultiLock::ScopedLock again;
        bool secondAttempt = multiLock.try_lock(5, &again);
        heldRejected = !secondAttempt;
    }

    FiberMultiLock::ScopedLock reacquire;
    bool reacquired = multiLock.try_lock(5, &reacquire);

    EXPECT_TRUE(freeAcquired);
    EXPECT_TRUE(heldRejected);
    EXPECT_TRUE(reacquired);
}

// Each iteration takes the key, reads the shared counter, yields to force interleaving, then writes back
// the incremented value. The read-modify-write is unsynchronized, so the final count equals the total
// iteration count only if the lock serializes the critical section; a broken lock loses updates.
TEST(FiberMultiLock, sameKeySerializesContendingFibers)
{
    static constexpr uint32_t FIBER_COUNT = 8;
    static constexpr uint32_t ITERATIONS = 50;
    static constexpr uint64_t KEY = 42;

    struct Params
    {
        FiberMultiLock * multiLock;
        uint64_t * counter;

        static int fiberMain(Params * params) noexcept
        {
            for (uint32_t i = 0; i < ITERATIONS; ++i)
            {
                FiberMultiLock::ScopedLock scopedLock;
                params->multiLock->lock(KEY, &scopedLock);

                uint64_t value = *params->counter;
                FiberScheduler::yield();
                *params->counter = value + 1;
            }
            return 0;
        }
    };

    FiberMultiLock multiLock;
    uint64_t counter = 0;
    FiberFuture futures[FIBER_COUNT];

    for (uint32_t i = 0; i < FIBER_COUNT; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, {&multiLock, &counter}, &futures[i]);
        ASSERT_FALSE(r);
    }

    for (uint32_t i = 0; i < FIBER_COUNT; ++i)
    {
        futures[i].wait();
    }

    ASSERT_EQ(counter, uint64_t{FIBER_COUNT} * ITERATIONS);
}

// Many fibers contend on distinct keys at once: every lock should grant immediately and every fiber should
// run to completion. Guards against a key collision wrongly serializing them.
TEST(FiberMultiLock, distinctKeysRunConcurrently)
{
    static constexpr uint32_t FIBER_COUNT = 8;
    static constexpr uint32_t ITERATIONS = 50;

    struct Params
    {
        FiberMultiLock * multiLock;
        uint64_t key;

        static int fiberMain(Params * params) noexcept
        {
            for (uint32_t i = 0; i < ITERATIONS; ++i)
            {
                FiberMultiLock::ScopedLock scopedLock;
                params->multiLock->lock(params->key, &scopedLock);

                FiberScheduler::yield();
            }
            return 0;
        }
    };

    FiberMultiLock multiLock;
    FiberFuture futures[FIBER_COUNT];

    for (uint32_t i = 0; i < FIBER_COUNT; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, {&multiLock, i}, &futures[i]);
        ASSERT_FALSE(r);
    }

    for (uint32_t i = 0; i < FIBER_COUNT; ++i)
    {
        int result = futures[i].wait();
        ASSERT_EQ(result, 0);
    }
}

} // namespace silk
