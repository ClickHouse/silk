#include <silk/fibers/mutex.h>

#include <silk/fibers/fiber.h>
#include <silk/fibers/futex.h>
#include <silk/fibers/future.h>
#include <silk/util/platform.h>

#include <gtest/gtest.h>

namespace silk
{

TEST(FiberMutex, tryLock)
{
    FiberMutex mutex;

    bool b = mutex.try_lock();
    ASSERT_TRUE(b);

    b = mutex.try_lock();
    ASSERT_TRUE(!b);

    mutex.unlock();

    b = mutex.try_lock();
    ASSERT_TRUE(b);
}

TEST(FiberMutex, lockFromFiber)
{
    struct Params
    {
        FiberMutex * mutex;
        FiberFuture * started;
        FiberFuture * acquired;

        static int fiberMain(Params * p) noexcept
        {
            p->started->set(0);
            p->mutex->lock();
            p->acquired->set(0);
            p->mutex->unlock();
            return 0;
        }
    };

    FiberMutex mutex;
    mutex.lock();

    FiberFuture future, started, acquired;
    int r = FiberScheduler::run(Params::fiberMain, {&mutex, &started, &acquired}, &future);
    ASSERT_FALSE(r);

    started.wait();
    mutex.unlock();
    acquired.wait();

    future.wait();
}

TEST(FiberMutex, lockFromThread)
{
    struct HolderParams
    {
        FiberMutex * mutex;
        FiberFuture * locked;
        FiberFuture * release;

        static int fiberMain(HolderParams * p) noexcept
        {
            p->mutex->lock();
            p->locked->set(0);
            p->release->wait();
            p->mutex->unlock();
            return 0;
        }
    };

    struct ReleaserParams
    {
        FiberFuture * release;

        static int fiberMain(ReleaserParams * p) noexcept
        {
            FiberScheduler::yield();
            p->release->set(0);
            return 0;
        }
    };

    FiberMutex mutex;
    FiberFuture locked, release;
    FiberFuture holder, releaser;

    int r = FiberScheduler::run(HolderParams::fiberMain, {&mutex, &locked, &release}, &holder);
    ASSERT_FALSE(r);
    locked.wait();

    r = FiberScheduler::run(ReleaserParams::fiberMain, {&release}, &releaser);
    ASSERT_FALSE(r);

    mutex.lock();
    mutex.unlock();

    holder.wait();
    releaser.wait();
}

TEST(FiberMutex, lockContended)
{
    struct ParamsA
    {
        FiberMutex * mutex;
        FiberFuture * locked;
        FiberFuture * release;

        static int fiberMain(ParamsA * p) noexcept
        {
            p->mutex->lock();
            p->locked->set(0);
            p->release->wait();
            p->mutex->unlock();
            return 0;
        }
    };

    struct ParamsB
    {
        FiberMutex * mutex;
        FiberFuture * acquired;

        static int fiberMain(ParamsB * p) noexcept
        {
            p->mutex->lock();
            p->acquired->set(0);
            p->mutex->unlock();
            return 0;
        }
    };

    FiberMutex mutex;
    FiberFuture locked, release, acquired;
    FiberFuture futureA, futureB;

    int r = FiberScheduler::run(ParamsA::fiberMain, {&mutex, &locked, &release}, &futureA);
    ASSERT_FALSE(r);
    locked.wait();

    r = FiberScheduler::run(ParamsB::fiberMain, {&mutex, &acquired}, &futureB);
    ASSERT_FALSE(r);
    release.set(0);
    acquired.wait();

    futureA.wait();
    futureB.wait();
}

TEST(FiberMutex, mutualExclusion)
{
    static constexpr int N_FIBERS = 8;
    static constexpr int N_ITER = 1000;

    struct Params
    {
        FiberMutex * mutex;
        int * counter;

        static int fiberMain(Params * p) noexcept
        {
            for (int i = 0; i < N_ITER; ++i)
            {
                p->mutex->lock();
                ++(*p->counter);
                p->mutex->unlock();
            }
            return 0;
        }
    };

    FiberMutex mutex;
    int counter = 0;

    FiberFuture futures[N_FIBERS];
    for (int i = 0; i < N_FIBERS; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, {&mutex, &counter}, &futures[i]);
        ASSERT_FALSE(r);
    }

    for (int i = 0; i < N_FIBERS; ++i)
    {
        futures[i].wait();
    }

    ASSERT_EQ(counter, N_FIBERS * N_ITER);
}

TEST(FiberMutex, tryLockShared)
{
    FiberMutex mutex;

    bool b = mutex.try_lock_shared();
    ASSERT_TRUE(b);

    b = mutex.try_lock_shared();
    ASSERT_TRUE(b);

    mutex.unlock_shared();
    mutex.unlock_shared();

    b = mutex.try_lock();
    ASSERT_TRUE(b);
    mutex.unlock();
}

TEST(FiberMutex, tryLockSharedExcludesExclusive)
{
    FiberMutex mutex;
    mutex.lock_shared();

    bool b = mutex.try_lock();
    ASSERT_FALSE(b);

    mutex.unlock_shared();

    b = mutex.try_lock();
    ASSERT_TRUE(b);
    mutex.unlock();
}

TEST(FiberMutex, tryLockExcludesShared)
{
    FiberMutex mutex;
    mutex.lock();

    bool b = mutex.try_lock_shared();
    ASSERT_FALSE(b);

    mutex.unlock();

    b = mutex.try_lock_shared();
    ASSERT_TRUE(b);
    mutex.unlock_shared();
}

TEST(FiberMutex, sharedDoesNotExcludeShared)
{
    static constexpr int N_FIBERS = 8;

    struct Params
    {
        FiberMutex * mutex;
        FiberFuture * acquired;
        FiberFutex * release;
        uint64_t releaseToken;

        static int fiberMain(Params * p) noexcept
        {
            p->mutex->lock_shared();
            p->acquired->set(0);
            int r = p->release->wait(p->releaseToken);
            SILK_UNUSED(r);
            p->mutex->unlock_shared();
            return 0;
        }
    };

    FiberMutex mutex;
    FiberFuture acquired[N_FIBERS];
    FiberFutex release;
    uint64_t releaseToken = release.get() + 1;
    FiberFuture futures[N_FIBERS];

    for (int i = 0; i < N_FIBERS; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, {&mutex, &acquired[i], &release, releaseToken}, &futures[i]);
        ASSERT_FALSE(r);
    }

    // All readers must reach "acquired" before we release them, proving they
    // hold the lock simultaneously.
    for (int i = 0; i < N_FIBERS; ++i)
    {
        acquired[i].wait();
    }

    release.post();

    for (int i = 0; i < N_FIBERS; ++i)
    {
        futures[i].wait();
    }
}

TEST(FiberMutex, sharedWakesAfterExclusiveRelease)
{
    struct ReaderParams
    {
        FiberMutex * mutex;
        FiberFuture * acquired;

        static int fiberMain(ReaderParams * p) noexcept
        {
            p->mutex->lock_shared();
            p->acquired->set(0);
            p->mutex->unlock_shared();
            return 0;
        }
    };

    FiberMutex mutex;
    mutex.lock();

    FiberFuture acquired;
    FiberFuture reader;
    int r = FiberScheduler::run(ReaderParams::fiberMain, {&mutex, &acquired}, &reader);
    ASSERT_FALSE(r);

    mutex.unlock();
    acquired.wait();
    reader.wait();
}

TEST(FiberMutex, exclusiveWakesAfterSharedRelease)
{
    struct WriterParams
    {
        FiberMutex * mutex;
        FiberFuture * acquired;

        static int fiberMain(WriterParams * p) noexcept
        {
            p->mutex->lock();
            p->acquired->set(0);
            p->mutex->unlock();
            return 0;
        }
    };

    FiberMutex mutex;
    mutex.lock_shared();
    mutex.lock_shared();

    FiberFuture acquired;
    FiberFuture writer;
    int r = FiberScheduler::run(WriterParams::fiberMain, {&mutex, &acquired}, &writer);
    ASSERT_FALSE(r);

    mutex.unlock_shared();
    // After one unlock_shared the counter is still > 0; writer must still be blocked.
    mutex.unlock_shared();
    acquired.wait();
    writer.wait();
}

// Once an exclusive waiter is queued behind a shared holder, try_lock_shared
// must start failing - that's the writer-priority guarantee. Without it (the
// old single hasWaiters bit), try_lock_shared would happily keep succeeding
// while the writer starves.
TEST(FiberMutex, writerPriorityBlocksNewReaders)
{
    struct Writer
    {
        FiberMutex * mutex;

        static int fiberMain(Writer * p) noexcept
        {
            p->mutex->lock();
            p->mutex->unlock();
            return 0;
        }
    };

    FiberMutex mutex;
    mutex.lock_shared();

    FiberFuture writerDone;
    int r = FiberScheduler::run(Writer::fiberMain, {&mutex}, &writerDone);
    ASSERT_FALSE(r);

    // The work-stealing scheduler picks up the writer on another worker
    // promptly; the writer hits the slow path, arms hasExclusiveWaiters, and
    // suspends. Spin try_lock_shared until we observe the bit (bounded so a
    // regression that drops writer-priority surfaces as a test failure rather
    // than a hang).
    bool sawFailure = false;
    for (int i = 0; i < 10000; ++i)
    {
        if (!mutex.try_lock_shared())
        {
            sawFailure = true;
            break;
        }
        mutex.unlock_shared();
        schedYield();
    }
    ASSERT_TRUE(sawFailure) << "writer-priority not enforced: try_lock_shared kept succeeding while a writer was queued";

    // While we still hold the shared lock the writer cannot acquire, so
    // hasExclusiveWaiters stays set and try_lock_shared keeps failing.
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_FALSE(mutex.try_lock_shared());
        schedYield();
    }

    mutex.unlock_shared();
    writerDone.wait();
}

TEST(FiberMutex, mixedReadersWritersCounter)
{
    static constexpr int N_READERS = 6;
    static constexpr int N_WRITERS = 2;
    static constexpr int N_ITER = 500;

    struct WriterParams
    {
        FiberMutex * mutex;
        int * counter;

        static int fiberMain(WriterParams * p) noexcept
        {
            for (int i = 0; i < N_ITER; ++i)
            {
                p->mutex->lock();
                ++(*p->counter);
                p->mutex->unlock();
            }
            return 0;
        }
    };

    struct ReaderParams
    {
        FiberMutex * mutex;
        const int * counter;
        int * readSum;

        static int fiberMain(ReaderParams * p) noexcept
        {
            int local = 0;
            for (int i = 0; i < N_ITER; ++i)
            {
                p->mutex->lock_shared();
                local += *p->counter;
                p->mutex->unlock_shared();
            }
            *p->readSum = local;
            return 0;
        }
    };

    FiberMutex mutex;
    int counter = 0;
    int readSums[N_READERS] = {};

    FiberFuture writerFutures[N_WRITERS];
    FiberFuture readerFutures[N_READERS];

    for (int i = 0; i < N_WRITERS; ++i)
    {
        int r = FiberScheduler::run(WriterParams::fiberMain, {&mutex, &counter}, &writerFutures[i]);
        ASSERT_FALSE(r);
    }
    for (int i = 0; i < N_READERS; ++i)
    {
        int r = FiberScheduler::run(ReaderParams::fiberMain, {&mutex, &counter, &readSums[i]}, &readerFutures[i]);
        ASSERT_FALSE(r);
    }

    for (int i = 0; i < N_WRITERS; ++i)
    {
        writerFutures[i].wait();
    }
    for (int i = 0; i < N_READERS; ++i)
    {
        readerFutures[i].wait();
    }

    ASSERT_EQ(counter, N_WRITERS * N_ITER);
    // Readers observe values in [0, N_WRITERS * N_ITER] - no torn reads possible
    // because shared lock excludes writers. Each individual read of *counter is
    // a value that some writer published; we just check sums are sensible.
    for (int i = 0; i < N_READERS; ++i)
    {
        ASSERT_GE(readSums[i], 0);
        ASSERT_LE(readSums[i], N_WRITERS * N_ITER * N_ITER);
    }
}

} // namespace silk
