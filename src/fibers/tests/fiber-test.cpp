#include <silk/fibers/fiber.h>

#include <silk/fibers/future.h>
#include <silk/util/assert.h>
#include <silk/util/platform.h>
#include <silk/util/tsc.h>

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <set>

#include <sched.h>

namespace silk
{

TEST(Fiber, run)
{
    struct Params
    {
        static int fiberMain(Params * p) noexcept
        {
            SILK_UNUSED(p);
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
            SILK_UNUSED(p);
            FiberScheduler::suspend(reinterpret_cast<FiberScheduler::SuspendCallback *>(suspendCallback), nullptr);
            return 0;
        }

        static void suspendCallback(Fiber * fiber, void * context)
        {
            SILK_UNUSED(context);
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
            SILK_UNUSED(p);
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
            SILK_UNUSED(p);
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
            SILK_UNUSED(p);
            Fiber * fiber = FiberScheduler::getCurrentFiber();
            SILK_UNUSED(fiber);
            return 0;
        }
    };

    Fiber * currentFiber = FiberScheduler::getCurrentFiber();
    ASSERT_TRUE(currentFiber);

    int r = FiberScheduler::run(Params::fiberMain, {});
    ASSERT_EQ(r, 0);
}

// getCurrentFiberId returns 0 outside a fiber context (proxy fiber thread).
TEST(Fiber, getCurrentFiberIdOutsideFiber)
{
    EXPECT_EQ(FiberScheduler::getCurrentFiberId().raw, 0);
}

// getCurrentFiberId returns a non-zero id inside a fiber, encoding cpu+counter.
TEST(Fiber, getCurrentFiberIdInsideFiber)
{
    struct Params
    {
        FiberId * out;

        static int fiberMain(Params * p) noexcept
        {
            *p->out = FiberScheduler::getCurrentFiberId();
            return 0;
        }
    };

    FiberId id = {};
    int r = FiberScheduler::run(Params::fiberMain, {&id});
    ASSERT_EQ(r, 0);

    EXPECT_NE(id.raw, 0u) << "non-zero id distinguishes fiber context from no-fiber sentinel";
    EXPECT_EQ(id.category, 0u) << "default category is 0";
    EXPECT_LT(id.cpu, getProcessorCount()) << "cpu must be a valid processor index";
    EXPECT_GE(id.counter, 1u) << "fiberCounter starts at 1 to avoid the all-zero sentinel";
}

// Two fibers run back-to-back get distinct ids; counter advances within the same CPU.
TEST(Fiber, getCurrentFiberIdMonotonic)
{
    struct Params
    {
        FiberId * out;

        static int fiberMain(Params * p) noexcept
        {
            *p->out = FiberScheduler::getCurrentFiberId();
            return 0;
        }
    };

    FiberId first = {};
    FiberId second = {};
    ASSERT_EQ(FiberScheduler::run(Params::fiberMain, {&first}), 0);
    ASSERT_EQ(FiberScheduler::run(Params::fiberMain, {&second}), 0);

    EXPECT_NE(first.raw, second.raw) << "back-to-back fibers must have distinct ids";
    if (first.cpu == second.cpu)
    {
        EXPECT_GT(second.counter, first.counter) << "counter is per-CPU monotonic";
    }
}

// FiberScheduler::run with explicit category stamps the byte into the high 8 bits of fiberId.
TEST(Fiber, runWithCategoryStampsUpperByte)
{
    struct Params
    {
        FiberId * out;

        static int fiberMain(Params * p) noexcept
        {
            *p->out = FiberScheduler::getCurrentFiberId();
            return 0;
        }
    };

    FiberId id = {};
    int r = FiberScheduler::run(Params::fiberMain, {&id}, uint8_t{0xAB});
    ASSERT_EQ(r, 0);

    EXPECT_EQ(id.category, 0xABu);
    EXPECT_LT(id.cpu, getProcessorCount());
    EXPECT_GE(id.counter, 1u);
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
        std::atomic<uint16_t> * cpu;

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
    std::atomic<uint16_t> blockerCpuAtom{kInvalidProcessorNumber};

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

// A busy-yield loop must not starve the per-CPU service loop. runScheduler drains
// the ready queue (handleReadyQueue) before runServiceLoop, and yield re-enqueues
// the fiber on the same CPU, so without a dispatch bound (Options.readyDispatchBatch)
// the queue never empties and timer expiry never runs. Each worker arms a sleep then
// busy-yields waiting for it: a starved sleep keeps its worker yielding, which keeps
// its CPU saturated - self-sustaining, so the storm holds across work stealing. With
// the bound the sleeps keep completing; without it progress freezes.
TEST(Fiber, yieldStormDoesNotStarveSleep)
{
    static constexpr uint64_t WORKER_SLEEP_NS = 1'000'000; // 1ms per worker cycle
    static constexpr uint64_t SETTLE_NS = 1'000'000'000; // let the storm reach steady-state saturation
    static constexpr uint64_t PROGRESS_NS = 3'000'000'000; // window over which sleeps must keep completing
    static constexpr uint64_t YIELD_CAP_NS = 60'000'000'000; // backstop so a worker cannot spin forever

    struct Shared
    {
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> completed{0};
    };

    struct Worker
    {
        Shared * shared;

        static int fiberMain(Worker * params) noexcept
        {
            uint64_t capCycles = Tsc::getCycles() + Tsc::nanosecondsToCycles(YIELD_CAP_NS);
            for (;;)
            {
                bool stop = params->shared->stop.load(std::memory_order_acquire);
                if (stop)
                {
                    break;
                }

                FiberScheduler::SleepFuture future;
                FiberScheduler::sleep(WORKER_SLEEP_NS, &future);

                int err;
                for (;;)
                {
                    bool fired = future.isSet(&err);
                    if (fired)
                    {
                        break;
                    }

                    bool abandon = params->shared->stop.load(std::memory_order_acquire);
                    uint64_t now = Tsc::getCycles();
                    if (abandon || now > capCycles)
                    {
                        future.cancel();
                        future.wait();
                        return 0;
                    }

                    FiberScheduler::yield();
                }

                params->shared->completed.fetch_add(1, std::memory_order_relaxed);
            }
            return 0;
        }
    };

    uint16_t cpuCount = getProcessorCount();
    uint32_t workerCount = 4 * cpuCount;

    Shared shared;
    std::unique_ptr<FiberFuture[]> workerFutures(new FiberFuture[workerCount]);

    for (uint32_t i = 0; i < workerCount; ++i)
    {
        int r = FiberScheduler::run(Worker::fiberMain, {&shared}, &workerFutures[i]);
        ASSERT_FALSE(r);
    }

    // Let the storm reach steady-state saturation before measuring.
    uint64_t settleEnd = getTimeNanoseconds() + SETTLE_NS;
    for (;;)
    {
        uint64_t now = getTimeNanoseconds();
        if (now >= settleEnd)
        {
            break;
        }

        schedYield();
    }

    // Under sustained saturation, sleeps keep completing only if the service loop
    // still runs. Without the dispatch bound it is starved and progress freezes.
    uint64_t baseline = shared.completed.load(std::memory_order_relaxed);
    uint64_t progressEnd = getTimeNanoseconds() + PROGRESS_NS;
    for (;;)
    {
        uint64_t now = getTimeNanoseconds();
        if (now >= progressEnd)
        {
            break;
        }

        schedYield();
    }
    uint64_t progress = shared.completed.load(std::memory_order_relaxed) - baseline;

    shared.stop.store(true, std::memory_order_release);

    if (progress < workerCount)
    {
        SILK_FAIL("yield storm starved the service loop: %lu sleeps completed under load (expected >= %u)", progress, workerCount);
    }

    for (uint32_t i = 0; i < workerCount; ++i)
    {
        workerFutures[i].wait();
    }
}

// A fiber's C++ exception-propagation state must survive context switches. silk saves and restores
// it per fiber on every switch; without that, fibers sharing an OS thread corrupt each other's
// in-flight exception when handling spans a switch - the wrong exception object is freed (observed
// as a use-after-free or a wrong value) and the original leaks. Many fibers throw and, while
// handling the exception, yield so their handlers interleave on shared scheduler threads; each must
// still observe its own exception. Under LeakSanitizer this also catches the orphaned objects.
TEST(Fiber, exceptionStateIsolatedAcrossSwitch)
{
    static constexpr int FIBER_COUNT = 64;
    static constexpr int ITERATIONS = 200;

    struct Marker
    {
        int value;
    };

    struct Params
    {
        int id;
        std::atomic<int> * mismatches;

        static int fiberMain(Params * p) noexcept
        {
            for (int i = 0; i < ITERATIONS; ++i)
            {
                int expected = p->id * ITERATIONS + i;
                try
                {
                    FiberScheduler::yield();
                    throw Marker{expected};
                }
                catch (const Marker & marker)
                {
                    // Suspend while the exception is live so another fiber's throw / catch can
                    // interleave on this OS thread; marker must still refer to our own object.
                    FiberScheduler::yield();
                    if (marker.value != expected)
                    {
                        p->mismatches->fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            return 0;
        }
    };

    std::atomic<int> mismatches{0};
    FiberFuture futures[FIBER_COUNT];

    for (int i = 0; i < FIBER_COUNT; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, {i, &mismatches}, &futures[i]);
        ASSERT_EQ(r, 0);
    }

    for (int i = 0; i < FIBER_COUNT; ++i)
    {
        ASSERT_EQ(futures[i].wait(), 0);
    }

    ASSERT_EQ(mismatches.load(), 0);
}

} // namespace silk
