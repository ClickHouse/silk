#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>
#include <silk/fibers/mutex.h>
#include <silk/util/platform.h>
#include <silk/util/sharded-stack.h>
#include <silk/util/tsc.h>

#include <gtest/gtest.h>

#include <atomic>
#include <unordered_set>

#include <fcntl.h>
#include <unistd.h>

#include <sys/uio.h>

// Basic enter/exit: fiber enters thread mode, does work, exits, then continues
// as a cooperative fiber and returns a result.

namespace silk
{

TEST(FiberThreadMode, enterExit)
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
TEST(FiberThreadMode, doesNotStallScheduler)
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
TEST(FiberThreadMode, waitOnFuture)
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
TEST(FiberThreadMode, mutex)
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
TEST(FiberThreadMode, multipleCycles)
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

// Nested scopes: only the outermost one migrates. The inner scope keeps the fiber on the
// worker thread it already runs on, and leaving it does not return the fiber to the
// scheduler before the outer scope ends.
TEST(FiberThreadMode, nested)
{
    struct Params
    {
        static int fiberMain(Params *) noexcept
        {
            pid_t schedulerThread = ::gettid();
            pid_t workerThread;
            {
                FiberScheduler::ThreadModeScope outer;
                workerThread = ::gettid();
                EXPECT_NE(workerThread, schedulerThread);
                {
                    FiberScheduler::ThreadModeScope inner;
                    EXPECT_EQ(::gettid(), workerThread);
                }
                EXPECT_EQ(::gettid(), workerThread);
            }
            EXPECT_NE(::gettid(), workerThread);
            return 0;
        }
    };

    int r = FiberScheduler::run(Params::fiberMain, {});
    ASSERT_EQ(r, 0);
}

// Many fibers in thread mode concurrently: all must complete correctly.
TEST(FiberThreadMode, concurrent)
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
TEST(FiberThreadMode, readyQueueOverflow)
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

/** Bound on a read from /dev/zero: a read the kernel received completes in microseconds. */
static constexpr uint64_t READ_TIMEOUT_NS = 1'000'000'000;

/** Read from fd and wait for the read without a bound, the way a socket writer waits for its write. */
static int readZeroes(int fd) noexcept
{
    char buffer[8];
    iovec part{buffer, sizeof(buffer)};
    uint64_t bytesRead;
    FiberScheduler::IoFuture future;
    FiberScheduler::read(fd, &part, 1, 0, &bytesRead, &future);

    int r = future.wait();
    if (r)
    {
        return r;
    }

    return bytesRead == sizeof(buffer) ? 0 : EIO;
}

/** Poll the future from the test thread until it is set; abort when its read never reaches the kernel. */
static void waitUntilSet(FiberFuture * future) noexcept
{
    // A wait through the scheduler would arm a sleep that wakes the processor holding the read, so the test
    // thread polls instead.
    uint64_t deadlineCycles = Tsc::getCycles() + Tsc::nanosecondsToCycles(READ_TIMEOUT_NS);
    int r;
    while (!future->isSet(&r))
    {
        SILK_ASSERT(Tsc::getCycles() < deadlineCycles, "a read did not complete within %lu ns", READ_TIMEOUT_NS);
        ::usleep(100);
    }
}

// A thread-mode fiber runs on a worker thread, whose CPU's ring need not be the one runFiber submits after
// the fiber suspends: every read it issues must still reach the kernel while the idle processors park.
TEST(FiberThreadMode, readCompletes)
{
    static constexpr int ITERATIONS = 256;

    struct Reader
    {
        int fd;

        static int fiberMain(Reader * params) noexcept
        {
            FiberScheduler::ThreadModeScope scope;
            return readZeroes(params->fd);
        }
    };

    int fd = ::open("/dev/zero", O_RDONLY | O_CLOEXEC);
    ASSERT_GE(fd, 0);

    for (int i = 0; i < ITERATIONS; ++i)
    {
        FiberFuture future;
        int r = FiberScheduler::run(Reader::fiberMain, {fd}, &future);
        ASSERT_FALSE(r);

        waitUntilSet(&future);
        r = future.wait();
        ASSERT_EQ(r, 0) << "iteration " << i;
    }

    ::close(fd);
}

// Fibers pop on a scheduler thread and push from a worker thread in thread mode; the drain must
// return every entry exactly once.
TEST(FiberThreadMode, shardedStackAcrossThreadSwitch)
{
    struct Entry
    {
        StackEntry stackEntry;
    };
    using EntryStack = ShardedStack<Entry, &Entry::stackEntry>;

    static constexpr int N_FIBERS = 1000;
    static constexpr int N_ROUNDS = 200;

    struct Params
    {
        EntryStack * stack;

        static int fiberMain(Params * params) noexcept
        {
            // On ARM and with LTO enabled, the thread local storage base pointer is hoisted and used
            // in (inlined) pop and push rseq TLS accesses. Even when we yield the fiber.
            // The compiler is not aware that yield returns on another thread.
            // That breaks rseq invariants: namely, we can work with one rseq area concurrently.
            // Therefore, we need to make sure rseq re-reads the TLS base pointer internally.
            for (int i = 0; i < N_ROUNDS; ++i)
            {
                // Entries cached on another CPU's list are invisible to this pop,
                // so it can return nullptr.
                Entry * entry = params->stack->pop();
                if (!entry)
                {
                    continue;
                }

                FiberScheduler::ThreadModeScope scope;
                params->stack->push(entry);
            }
            return 0;
        }
    };

    EntryStack stack{1};
    Entry entries[N_FIBERS];
    for (Entry & entry : entries)
    {
        stack.push(&entry);
    }

    FiberFuture futures[N_FIBERS];
    for (FiberFuture & future : futures)
    {
        int r = FiberScheduler::run(Params::fiberMain, {&stack}, &future);
        ASSERT_EQ(r, 0);
    }

    for (FiberFuture & future : futures)
    {
        ASSERT_EQ(future.wait(), 0);
    }

    std::unordered_set<Entry *> recovered;
    stack.drain(
        [](Entry * entry, void * raw) noexcept
        { EXPECT_TRUE(static_cast<std::unordered_set<Entry *> *>(raw)->insert(entry).second) << "duplicate: " << entry; },
        &recovered);
    ASSERT_EQ(recovered.size(), N_FIBERS);
}

} // namespace silk
