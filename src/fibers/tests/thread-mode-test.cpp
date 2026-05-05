#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>
#include <silk/fibers/mutex.h>
#include <silk/util/platform.h>

#include <gtest/gtest.h>

#include <atomic>

#include <unistd.h>

// Basic enter/exit: fiber enters thread mode, does work, exits, then continues
// as a cooperative fiber and returns a result.

namespace silk
{

TEST(ThreadMode, enterExit)
{
    struct Params
    {
        static int fiberMain(Params *) noexcept
        {
            FiberScheduler::ThreadModeScope scope;
            // In thread mode: blocking work is safe here.
            ::usleep(1);
            return 42;
        }
    };

    int r = FiberScheduler::run(Params::fiberMain, {});
    ASSERT_EQ(r, 42);
}

// A fiber in thread mode making a real blocking syscall does not stall other
// fibers: a concurrent fiber must complete while the thread-mode fiber is
// blocked inside usleep.
TEST(ThreadMode, doesNotStallScheduler)
{
    struct Blocker
    {
        std::atomic<bool> * done;

        static int fiberMain(Blocker * p) noexcept
        {
            FiberScheduler::ThreadModeScope scope;
            ::usleep(10'000); // 10ms real block
            p->done->store(true, std::memory_order_release);
            return 0;
        }
    };

    struct Counter
    {
        std::atomic<bool> * blockerDone;
        std::atomic<int> * count;

        static int fiberMain(Counter * p) noexcept
        {
            // Spin cooperatively until the blocker finishes.
            while (!p->blockerDone->load(std::memory_order_acquire))
            {
                FiberScheduler::yield();
            }
            p->count->fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
    };

    std::atomic<bool> done{};
    std::atomic<int> count{};

    FiberFuture blocker, counter;
    int r = FiberScheduler::run(Blocker::fiberMain, {&done}, &blocker);
    ASSERT_FALSE(r);
    r = FiberScheduler::run(Counter::fiberMain, {&done, &count}, &counter);
    ASSERT_FALSE(r);

    blocker.wait();
    counter.wait();

    ASSERT_EQ(count.load(), 1);
}

// A fiber in thread mode can wait on a FiberFuture set by another fiber,
// exercising the full suspend/reschedule path through the worker pool.
TEST(ThreadMode, waitOnFuture)
{
    struct Waiter
    {
        FiberFuture * future;

        static int fiberMain(Waiter * p) noexcept
        {
            FiberScheduler::ThreadModeScope scope;
            int r = p->future->wait();
            return r;
        }
    };

    struct Setter
    {
        FiberFuture * future;

        static int fiberMain(Setter * p) noexcept
        {
            p->future->set(99);
            return 0;
        }
    };

    FiberFuture sharedFuture;
    FiberFuture waiter, setter;
    int r = FiberScheduler::run(Waiter::fiberMain, {&sharedFuture}, &waiter);
    ASSERT_FALSE(r);
    r = FiberScheduler::run(Setter::fiberMain, {&sharedFuture}, &setter);
    ASSERT_FALSE(r);

    setter.wait();
    r = waiter.wait();
    ASSERT_EQ(r, 99);
}

// A fiber in thread mode can acquire a FiberMutex locked by another fiber,
// verifying that contended lock/unlock works across the thread-mode boundary.
TEST(ThreadMode, mutex)
{
    struct Params
    {
        FiberMutex * mutex;
        std::atomic<int> * counter;

        static int fiberMain(Params * p) noexcept
        {
            FiberScheduler::ThreadModeScope scope;
            std::lock_guard lock(*p->mutex);
            p->counter->fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
    };

    static constexpr int N = 16;
    FiberMutex mutex;
    std::atomic<int> counter{};

    FiberFuture futures[N];
    for (int i = 0; i < N; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, {&mutex, &counter}, &futures[i]);
        ASSERT_FALSE(r);
    }
    for (int i = 0; i < N; ++i)
    {
        futures[i].wait();
    }

    ASSERT_EQ(counter.load(), N);
}

// Multiple enter/exit cycles in a single fiber all complete correctly.
TEST(ThreadMode, multipleCycles)
{
    struct Params
    {
        static int fiberMain(Params *) noexcept
        {
            for (int i = 0; i < 10; ++i)
            {
                FiberScheduler::ThreadModeScope scope;
                ::usleep(1);
            }
            return 0;
        }
    };

    int r = FiberScheduler::run(Params::fiberMain, {});
    ASSERT_EQ(r, 0);
}

// Many fibers in thread mode concurrently: all must complete correctly.
TEST(ThreadMode, concurrent)
{
    static constexpr int N = 64;

    struct Params
    {
        int index;

        static int fiberMain(Params * p) noexcept
        {
            FiberScheduler::ThreadModeScope scope;
            ::usleep(1);
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

// Ready queue overflow: schedule enough fibers to exceed READY_QUEUE_CAPACITY
// so some spill to the worker thread pool. All must complete correctly.
TEST(ThreadMode, readyQueueOverflow)
{
    static constexpr int N = 2048; // > READY_QUEUE_CAPACITY (1024)

    struct Params
    {
        int index;
        static int fiberMain(Params * p) noexcept { return p->index; }
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
