#include <silk/fibers/fiber.h>
#include <silk/util/assert.h>
#include <silk/util/crash-dumper.h>
#include <silk/util/init.h>
#include <silk/util/platform.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <vector>

#include <poll.h>
#include <sched.h>
#include <unistd.h>

#include <fibers/cpu.h>

namespace silk
{

/**
 * The scheduler is restricted to a strict subset of the available CPUs, leaving
 * excludedCpu online but off the active set. The tests migrate onto it and drive
 * silk from there, exercising the injection redirect for a thread on a reserved
 * core.
 */
class CpuSetTest : public ::testing::Test
{
protected:
    /** Fiber entry: record the CPU the fiber runs on. */
    static int recordCpuFiber(int ** recordedCpu) noexcept
    {
        **recordedCpu = getCurrentProcessor();
        return 0;
    }

    /** Fiber entry: enter thread mode (moving to the worker pool) and record the CPU the worker runs the fiber on. */
    static int threadModeCpuFiber(int ** recordedCpu) noexcept
    {
        FiberScheduler::ThreadModeScope scope;
        **recordedCpu = getCurrentProcessor();
        return 0;
    }

    /** Move the calling thread onto the excluded core. */
    static void pinToExcludedCpu() noexcept
    {
        int r = pinThreadToCpu(static_cast<uint16_t>(excludedCpu));
        ASSERT_EQ(r, 0);
    }

    /** Whether @p cpu is in the scheduler's active set. */
    static bool isActiveCpu(int cpu) noexcept { return std::find(activeCpus.begin(), activeCpus.end(), cpu) != activeCpus.end(); }

public:
    /** CPUs in the scheduler's active set. Populated by main. */
    static std::vector<int> activeCpus;

    /** The reserved CPU. Populated by main; -1 when there are too few CPUs to reserve one, and every test skips. */
    static int excludedCpu;
};

std::vector<int> CpuSetTest::activeCpus;
int CpuSetTest::excludedCpu = -1;

// Spawning a fiber from a thread on an excluded core must complete and the fiber
// must run on an active core, never the excluded one - proving the injection
// redirect avoids the uninitialized ring / out-of-bounds processor index.
TEST_F(CpuSetTest, runFromExcludedCoreRunsOnActiveCore)
{
    if (excludedCpu < 0)
    {
        GTEST_SKIP() << "needs at least two available CPUs";
    }

    ASSERT_NO_FATAL_FAILURE(pinToExcludedCpu());

    int fiberCpu = -1;
    int r = FiberScheduler::run(recordCpuFiber, &fiberCpu);
    ASSERT_EQ(r, 0);
    ASSERT_NE(fiberCpu, excludedCpu);

    bool b = isActiveCpu(fiberCpu);
    ASSERT_TRUE(b);
}

// A proxy thread on an excluded core can sleep through silk: sleep routes the
// SleepFuture to an active home processor and is woken from there.
TEST_F(CpuSetTest, sleepFromExcludedCoreCompletes)
{
    if (excludedCpu < 0)
    {
        GTEST_SKIP() << "needs at least two available CPUs";
    }

    ASSERT_NO_FATAL_FAILURE(pinToExcludedCpu());

    FiberScheduler::sleep(1'000'000);
}

// Blocking pipe IO driven from a proxy thread on an excluded core: enqueueIo
// redirects the SQEs to an active home ring and force-submits them there.
TEST_F(CpuSetTest, ioFromExcludedCoreCompletes)
{
    if (excludedCpu < 0)
    {
        GTEST_SKIP() << "needs at least two available CPUs";
    }

    ASSERT_NO_FATAL_FAILURE(pinToExcludedCpu());

    int fds[2];
    int r = ::pipe(fds);
    ASSERT_EQ(r, 0);

    const char message[] = "cpuset";
    uint64_t bytesWritten = 0;
    r = FiberScheduler::write(fds[1], message, sizeof(message), 0, &bytesWritten);
    ASSERT_EQ(r, 0);
    ASSERT_EQ(bytesWritten, sizeof(message));

    char buf[sizeof(message)] = {};
    uint64_t bytesRead = 0;
    r = FiberScheduler::read(fds[0], buf, sizeof(buf), 0, &bytesRead);
    ASSERT_EQ(r, 0);
    ASSERT_EQ(bytesRead, sizeof(message));
    ASSERT_STREQ(buf, message);

    ::close(fds[0]);
    ::close(fds[1]);
}

// cancelIo from an excluded core must reach the same active ring that holds the
// poll SQE; a cross-ring cancel fails with -ENOENT and leaves wait hung.
TEST_F(CpuSetTest, cancelIoFromExcludedCoreCompletes)
{
    if (excludedCpu < 0)
    {
        GTEST_SKIP() << "needs at least two available CPUs";
    }

    ASSERT_NO_FATAL_FAILURE(pinToExcludedCpu());

    int fds[2];
    int r = ::pipe(fds);
    ASSERT_EQ(r, 0);

    FiberScheduler::IoFuture pollFuture;
    FiberScheduler::poll(fds[0], POLLIN, nullptr, &pollFuture);
    pollFuture.cancel();

    // POLLIN on an empty pipe with the write end open can only complete through
    // the cancellation.
    r = pollFuture.wait();
    ASSERT_EQ(r, ECANCELED);

    ::close(fds[0]);
    ::close(fds[1]);
}

// Thread-mode fibers run on the worker pool, which is pinned to the active set -
// a thread-mode slice never lands on the excluded core.
TEST_F(CpuSetTest, threadModeFromExcludedCoreStaysOnActiveCores)
{
    if (excludedCpu < 0)
    {
        GTEST_SKIP() << "needs at least two available CPUs";
    }

    ASSERT_NO_FATAL_FAILURE(pinToExcludedCpu());

    for (int i = 0; i < 32; ++i)
    {
        int fiberCpu = -1;
        int r = FiberScheduler::run(threadModeCpuFiber, &fiberCpu);
        ASSERT_EQ(r, 0);
        ASSERT_NE(fiberCpu, excludedCpu);

        bool b = isActiveCpu(fiberCpu);
        ASSERT_TRUE(b);
    }
}

// setAll from an excluded core batches the wakeups through scheduleAll: the
// doorbell SQEs are filled and submitted on the caller's home ring, and every
// woken fiber resumes on an active core.
TEST_F(CpuSetTest, setAllFromExcludedCoreWakesFibersOnActiveCores)
{
    if (excludedCpu < 0)
    {
        GTEST_SKIP() << "needs at least two available CPUs";
    }

    ASSERT_NO_FATAL_FAILURE(pinToExcludedCpu());

    struct Params
    {
        FiberFuture * waitFuture;
        int * resumedCpu;

        static int fiberMain(Params * params) noexcept
        {
            params->waitFuture->wait();
            *params->resumedCpu = getCurrentProcessor();
            return 0;
        }
    };

    constexpr uint64_t count = 16;
    FiberFuture waitFutures[count];
    FiberFuture doneFutures[count];
    FiberFuture * waitPointers[count];
    int resumedCpus[count] = {};

    for (uint64_t i = 0; i < count; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, Params{&waitFutures[i], &resumedCpus[i]}, &doneFutures[i]);
        ASSERT_EQ(r, 0);
        waitPointers[i] = &waitFutures[i];
    }

    FiberFuture::setAll(0, waitPointers, count);

    for (uint64_t i = 0; i < count; ++i)
    {
        int r = doneFutures[i].wait();
        ASSERT_EQ(r, 0);

        bool b = isActiveCpu(resumedCpus[i]);
        ASSERT_TRUE(b);
    }
}

// Many fibers injected from an excluded core all run on active cores - the
// only-allowed-cores property observed through the public API.
TEST_F(CpuSetTest, manyFibersFromExcludedCoreStayOnActiveCores)
{
    if (excludedCpu < 0)
    {
        GTEST_SKIP() << "needs at least two available CPUs";
    }

    ASSERT_NO_FATAL_FAILURE(pinToExcludedCpu());

    for (int i = 0; i < 128; ++i)
    {
        int fiberCpu = -1;
        int r = FiberScheduler::run(recordCpuFiber, &fiberCpu);
        ASSERT_EQ(r, 0);

        bool b = isActiveCpu(fiberCpu);
        ASSERT_TRUE(b);
    }
}

} // namespace silk

int main(int argc, char ** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    if (::testing::GTEST_FLAG(list_tests))
    {
        return RUN_ALL_TESTS();
    }

    silk::installCrashDumper();
    silk::initialize();

    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    int r = ::sched_getaffinity(0, sizeof(affinity), &affinity);
    if (r)
    {
        r = errno;
        SILK_FAIL("could not read the process affinity mask: r=%d", r);
    }

    // Silk indexes per-CPU state by raw CPU id below the configured processor count, so only
    // CPUs silk can address are considered.
    uint32_t processorCount = silk::getProcessorCount();
    std::vector<int> available;
    for (uint32_t cpu = 0; cpu < processorCount; ++cpu)
    {
        if (CPU_ISSET(cpu, &affinity))
        {
            available.push_back(cpu);
        }
    }

    silk::FiberScheduler::Options options;

    // Reserve the highest available CPU: exclude it from silk's active set while
    // it stays online so the tests can migrate onto it.
    if (available.size() >= 2)
    {
        silk::CpuSetTest::excludedCpu = available.back();
        CPU_CLR(silk::CpuSetTest::excludedCpu, &options.cpuMask);
        for (int cpu : available)
        {
            if (cpu != silk::CpuSetTest::excludedCpu)
            {
                silk::CpuSetTest::activeCpus.push_back(cpu);
            }
        }
    }

    silk::FiberScheduler::initialize(&options);

    r = RUN_ALL_TESTS();

    silk::FiberScheduler::destroy();
    silk::destroy();
    return r;
}
