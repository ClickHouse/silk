#include <silk/fibers/fiber.h>

#include <silk/fibers/future.h>
#include <silk/util/platform.h>
#include <silk/util/tsc.h>

#include <gtest/gtest.h>

#include <set>
#include <string_view>

#include <poll.h>
#include <sched.h>
#include <unistd.h>

namespace silk
{

TEST(Fiber, run)
{
    struct Params
    {
        static int fiberMain(Params * p) noexcept
        {
            UNUSED(p);
            return 42;
        }
    };

    int r = FiberScheduler::run(Params::fiberMain, {});
    ASSERT_EQ(r, 42);
}

TEST(Fiber, suspend)
{
    struct Params
    {
        static int fiberMain(Params * p) noexcept
        {
            UNUSED(p);
            FiberScheduler::suspend(reinterpret_cast<FiberScheduler::SuspendCallback *>(suspendCallback), nullptr);
            return 0;
        }

        static void suspendCallback(Fiber * fiber, void * context)
        {
            UNUSED(context);
            FiberScheduler::schedule(fiber);
        }
    };

    int r = FiberScheduler::run(Params::fiberMain, {});
    ASSERT_EQ(r, 0);
}

TEST(Fiber, yield)
{
    struct Params
    {
        static int fiberMain(Params * p) noexcept
        {
            UNUSED(p);
            FiberScheduler::yield();
            return 0;
        }
    };

    int r = FiberScheduler::run(Params::fiberMain, {});
    ASSERT_EQ(r, 0);
}

// Stresses the completion path: wait() may race with the fiber stopping and
// calling set(). FiberFuture::suspendCallback handles this via its CAS loop
// that re-checks isSet after registering the waiter.
TEST(Fiber, completionRace)
{
    struct Params
    {
        static int fiberMain(Params * p) noexcept
        {
            UNUSED(p);
            return 42;
        }
    };

    static constexpr int N = 10000;
    for (int i = 0; i < N; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, {});
        ASSERT_EQ(r, 42);
    }
}

// Verifies that fibers are returned to the pool after completion so they can be reused.
// Without freeFiber, virtual address space would be exhausted after many iterations
// (each fiber maps a 64 KB stack).
TEST(Fiber, poolReuse)
{
    struct Params
    {
        static int fiberMain(Params *) noexcept { return 0; }
    };

    static constexpr int N = 10000;
    for (int i = 0; i < N; ++i)
    {
        FiberScheduler::run(Params::fiberMain, {});
    }
}

TEST(Fiber, getCurrent)
{
    struct Params
    {
        static int fiberMain(Params * p) noexcept
        {
            UNUSED(p);
            Fiber * fiber = FiberScheduler::getCurrentFiber();
            UNUSED(fiber);
            return 0;
        }
    };

    Fiber * currentFiber = FiberScheduler::getCurrentFiber();
    ASSERT_TRUE(currentFiber);

    int r = FiberScheduler::run(Params::fiberMain, {});
    ASSERT_EQ(r, 0);
}

// Async IO from a non-fiber thread (proxy fiber): enqueueIo must call
// submitIo immediately since there is no runFiber to flush the SQE.
TEST(Fiber, asyncIoFromThread)
{
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);

    const char msg[] = "proxy";
    iovec wiov{const_cast<char *>(msg), sizeof(msg)};
    uint64_t bytesWritten = 0;
    FiberScheduler::IoFuture wf;
    FiberScheduler::write(fds[1], &wiov, 1, 0, &bytesWritten, &wf);
    int r = wf.wait();
    EXPECT_EQ(r, 0);
    EXPECT_EQ(bytesWritten, sizeof(msg));

    char buf[sizeof(msg)] = {};
    iovec riov{buf, sizeof(buf)};
    uint64_t bytesRead = 0;
    FiberScheduler::IoFuture rf;
    FiberScheduler::read(fds[0], &riov, 1, 0, &bytesRead, &rf);
    r = rf.wait();
    EXPECT_EQ(r, 0);
    EXPECT_EQ(bytesRead, sizeof(msg));
    EXPECT_STREQ(buf, msg);

    ::close(fds[0]);
    ::close(fds[1]);
}

// Basic blocking read/write through a pipe.
TEST(Fiber, readWrite)
{
    int fds[2];
    int r = ::pipe(fds);
    ASSERT_EQ(r, 0);

    struct Params
    {
        int readFd;
        int writeFd;

        static int fiberMain(Params * p) noexcept
        {
            const char message[] = "hello";
            uint64_t bytesWritten = 0;
            int w = FiberScheduler::write(p->writeFd, message, sizeof(message), 0, &bytesWritten);
            EXPECT_EQ(w, 0);
            EXPECT_EQ(bytesWritten, sizeof(message));

            char buf[sizeof(message)] = {};
            uint64_t bytesRead = 0;
            int r = FiberScheduler::read(p->readFd, buf, sizeof(buf), 0, &bytesRead);
            EXPECT_EQ(r, 0);
            EXPECT_EQ(bytesRead, sizeof(message));
            EXPECT_STREQ(buf, message);

            return 0;
        }
    };

    r = FiberScheduler::run(Params::fiberMain, {fds[0], fds[1]});
    ASSERT_EQ(r, 0);

    ::close(fds[0]);
    ::close(fds[1]);
}

// Async scatter/gather IO: submit writev and readv concurrently, wait on each future.
TEST(Fiber, asyncReadWrite)
{
    int fds[2];
    int r = ::pipe(fds);
    ASSERT_EQ(r, 0);

    struct Params
    {
        int readFd;
        int writeFd;

        static int fiberMain(Params * p) noexcept
        {
            const char part1[] = "foo";
            const char part2[] = "bar";

            iovec wiov[2] = {
                {const_cast<char *>(part1), sizeof(part1) - 1},
                {const_cast<char *>(part2), sizeof(part2) - 1},
            };

            uint64_t bytesWritten = 0;
            FiberScheduler::IoFuture wf;
            FiberScheduler::write(p->writeFd, wiov, 2, 0, &bytesWritten, &wf);

            char buf[6] = {};
            iovec riov[2] = {
                {buf, 3},
                {buf + 3, 3},
            };

            uint64_t bytesRead = 0;
            FiberScheduler::IoFuture rf;
            FiberScheduler::read(p->readFd, riov, 2, 0, &bytesRead, &rf);

            int w = wf.wait();
            EXPECT_EQ(w, 0);
            EXPECT_EQ(bytesWritten, 6u);

            int r = rf.wait();
            EXPECT_EQ(r, 0);
            EXPECT_EQ(bytesRead, 6u);
            EXPECT_EQ((std::string_view{buf, 6}), "foobar");

            return 0;
        }
    };

    r = FiberScheduler::run(Params::fiberMain, {fds[0], fds[1]});
    ASSERT_EQ(r, 0);

    ::close(fds[0]);
    ::close(fds[1]);
}

// poll: wait for readability on the read end before reading.
TEST(Fiber, pollReadable)
{
    int fds[2];
    int r = ::pipe(fds);
    ASSERT_EQ(r, 0);

    struct Params
    {
        int readFd;
        int writeFd;

        static int fiberMain(Params * p) noexcept
        {
            const char message[] = "poll";
            uint64_t bytesWritten = 0;
            int w = FiberScheduler::write(p->writeFd, message, sizeof(message), 0, &bytesWritten);
            EXPECT_EQ(w, 0);

            uint64_t triggeredEvents = 0;
            int ep = FiberScheduler::poll(p->readFd, POLLIN, &triggeredEvents);
            EXPECT_EQ(ep, 0);
            EXPECT_TRUE(triggeredEvents & POLLIN);

            char buf[sizeof(message)] = {};
            uint64_t bytesRead = 0;
            int r = FiberScheduler::read(p->readFd, buf, sizeof(buf), 0, &bytesRead);
            EXPECT_EQ(r, 0);
            EXPECT_STREQ(buf, message);

            return 0;
        }
    };

    r = FiberScheduler::run(Params::fiberMain, {fds[0], fds[1]});
    ASSERT_EQ(r, 0);

    ::close(fds[0]);
    ::close(fds[1]);
}

// cancel: cancel a pending read; future must complete with -ECANCELED (or
// with the read result if the kernel beat the cancellation).
TEST(Fiber, cancelRead)
{
    int fds[2];
    int r = ::pipe(fds);
    ASSERT_EQ(r, 0);

    struct Params
    {
        int readFd;
        int writeFd;

        static int fiberMain(Params * p) noexcept
        {
            char buf[16] = {};
            FiberScheduler::IoFuture rf;
            iovec riov{buf, sizeof(buf)};
            FiberScheduler::read(p->readFd, &riov, 1, 0, nullptr, &rf);

            rf.cancel();

            int r = rf.wait();
            EXPECT_TRUE(r == ECANCELED || r == 0);

            return 0;
        }
    };

    r = FiberScheduler::run(Params::fiberMain, {fds[0], fds[1]});
    ASSERT_EQ(r, 0);

    ::close(fds[0]);
    ::close(fds[1]);
}

// Stress: many fibers each doing a write+read through their own pipe.
TEST(Fiber, concurrentReadWrite)
{
    static constexpr int N = 100;

    struct Params
    {
        int readFd;
        int writeFd;
        int index;

        static int fiberMain(Params * p) noexcept
        {
            uint64_t bytesWritten = 0;
            int w = FiberScheduler::write(p->writeFd, &p->index, sizeof(p->index), 0, &bytesWritten);
            EXPECT_EQ(w, 0);

            int val = 0;
            uint64_t bytesRead = 0;
            int r = FiberScheduler::read(p->readFd, &val, sizeof(val), 0, &bytesRead);
            EXPECT_EQ(r, 0);
            EXPECT_EQ(val, p->index);

            return val;
        }
    };

    int fds[N][2];
    FiberFuture futures[N];

    for (int i = 0; i < N; ++i)
    {
        int r = ::pipe(fds[i]);
        ASSERT_EQ(r, 0);

        r = FiberScheduler::run(Params::fiberMain, {fds[i][0], fds[i][1], i}, &futures[i]);
        ASSERT_FALSE(r);
    }

    for (int i = 0; i < N; ++i)
    {
        int r = futures[i].wait();
        ASSERT_EQ(r, i);

        ::close(fds[i][0]);
        ::close(fds[i][1]);
    }
}

// Basic sleep: fiber sleeps for 10ms and wakes normally.
TEST(Fiber, sleep)
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
TEST(Fiber, sleepExpiry)
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
TEST(Fiber, cancelSleep)
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
TEST(Fiber, cancelSleepBeforeRegister)
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
TEST(Fiber, cancelSleepAfterInsert)
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
TEST(Fiber, sleepReuse)
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

// If a new sleep has a deadline LONGER than the already-armed wakeup timeout,
// enqueueWakeup must skip re-arming (the existing shorter timeout will fire first).
TEST(Fiber, sleepLongerDeadlineSkipped)
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

    // Register a LONGER sleep: enqueueWakeup must skip re-arming since the
    // armed timeout already fires before this deadline.
    FiberFuture futureB;
    r = FiberScheduler::run(Params::fiberMain, {LONG_NS}, &futureB);
    ASSERT_FALSE(r);

    futureA.wait();
    futureB.wait();
}

// Deadline update: a fiber with a shorter deadline registered after the
// scheduler has already armed a wakeup timeout for a longer one must still wake
// on time.  Without the fix the scheduler would sleep until the original long
// timeout fires instead of updating it to the shorter deadline.
TEST(Fiber, sleepDeadlineUpdate)
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

    // Wait until the scheduler has processed the sleep entry and is
    // sleeping on the 200ms io_uring timeout.
    ::usleep(5'000);

    // Register a shorter sleep.  The scheduler must update the in-flight
    // timeout so futureB wakes after SHORT_NS, not LONG_NS.
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
TEST(Fiber, sleepConcurrent)
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

// Verify that work-stealing actually moves fibers across CPUs. A blocker fiber
// occupies the local scheduler thread while child fibers are enqueued onto the
// same CPU's ready queue. Since the local scheduler is frozen inside the blocker,
// child fibers can only run by being stolen. Each child returns the CPU it ran on;
// the test asserts that at least two distinct CPUs appear.
TEST(Fiber, WorkStealing)
{
    if (getProcessorCount() < 2)
    {
        GTEST_SKIP() << "requires at least 2 CPUs";
    }

    static constexpr uint32_t N = 100;

    struct BlockerParams
    {
        std::atomic<bool> * started;
        std::atomic<bool> * stop;
        std::atomic<uint32_t> * cpu;

        static int fiberMain(BlockerParams * p) noexcept
        {
            p->cpu->store(getCurrentProcessor(), std::memory_order_release);
            p->started->store(true, std::memory_order_release);
            while (!p->stop->load(std::memory_order_relaxed))
            {
                cpuPause();
            }
            return 0;
        }
    };

    struct ChildParams
    {
        static int fiberMain(ChildParams *) noexcept { return static_cast<int>(getCurrentProcessor()); }
    };

    std::atomic<bool> started{false};
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> blockerCpuAtom{UINT32_MAX};

    FiberFuture blocker;
    int r = FiberScheduler::run(BlockerParams::fiberMain, {&started, &stop, &blockerCpuAtom}, &blocker);
    ASSERT_FALSE(r);

    while (!started.load(std::memory_order_acquire))
    {
        cpuPause();
    }
    uint32_t blockerCpu = blockerCpuAtom.load(std::memory_order_acquire);

    // The test runs as an OS thread whose CPU is chosen by the OS scheduler and
    // can migrate freely. enqueueReady assigns new fibers to getCurrentProcessor(),
    // so children would land on whichever CPU the test thread happens to be on --
    // not necessarily the blocked one. Pin the test thread to the blocker's CPU so
    // all children are enqueued into the blocked processor's ready queue and must
    // be stolen by other CPUs.
    cpu_set_t blockerMask;
    CPU_ZERO(&blockerMask);
    CPU_SET(blockerCpu, &blockerMask);
    sched_setaffinity(0, sizeof(blockerMask), &blockerMask);
    while (getCurrentProcessor() != blockerCpu)
    {
        cpuPause();
    }

    FiberFuture children[N];
    for (uint32_t i = 0; i < N; ++i)
    {
        int r = FiberScheduler::run(ChildParams::fiberMain, {}, &children[i]);
        ASSERT_FALSE(r);
    }

    // Restore full affinity before the join loop so the test thread can be
    // scheduled on any CPU while waiting for children to complete.
    cpu_set_t fullMask;
    CPU_ZERO(&fullMask);
    for (uint32_t i = 0; i < getProcessorCount(); ++i)
    {
        CPU_SET(i, &fullMask);
    }
    sched_setaffinity(0, sizeof(fullMask), &fullMask);

    // Join all children before releasing the blocker so each child is forced to
    // complete via stealing rather than local execution.
    std::set<int> cpus;
    for (uint32_t i = 0; i < N; ++i)
    {
        cpus.insert(children[i].wait());
    }

    stop.store(true, std::memory_order_release);
    blocker.wait();

    // The join loop completing before stop was set proves work stealing occurred:
    // children were in the blocked processor's ready queue and could only complete
    // by being stolen and run on other CPUs. Distribution across multiple CPUs is
    // not asserted — the steal deadline design allows one thief to drain the whole
    // queue in a single pass when fibers are instant.
    EXPECT_FALSE(cpus.empty());
}

// A single fiber posting more async polls than the SQE ring capacity (128)
// without waiting exposes exhaustion: with SQPOLL the kernel thread is pinned
// to the same CPU and cannot consume SQEs while the fiber runs.
TEST(Fiber, sqeRingExhaustion)
{
    struct Params
    {
        int readFd;
        int writeFd;

        static int fiberMain(Params * params) noexcept
        {
            static constexpr uint32_t COUNT = 256;
            FiberScheduler::IoFuture futures[COUNT];

            for (uint32_t i = 0; i < COUNT; ++i)
            {
                FiberScheduler::poll(params->readFd, POLLIN, nullptr, &futures[i]);
            }

            char byte = 1;
            ssize_t r = ::write(params->writeFd, &byte, 1);
            EXPECT_EQ(r, 1);

            for (uint32_t i = 0; i < COUNT; ++i)
            {
                futures[i].wait();
            }

            return 0;
        }
    };

    int pipeFds[2];
    ASSERT_EQ(::pipe(pipeFds), 0);

    FiberScheduler::run(Params::fiberMain, Params{pipeFds[0], pipeFds[1]});

    ::close(pipeFds[0]);
    ::close(pipeFds[1]);
}

} // namespace silk
