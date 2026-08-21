#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>
#include <silk/util/assert.h>
#include <silk/util/crash-dumper.h>
#include <silk/util/init.h>
#include <silk/util/platform.h>
#include <silk/util/tsc.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <vector>

#include <sched.h>
#include <unistd.h>

#include <fibers/cpu.h>

namespace silk
{

/**
 * The scheduler is restricted to two whole physical cores (both HT siblings of
 * each). The tests check where stolen work comes online and what it costs: two
 * compute fibers must never share a physical core while a whole core sits
 * parked, and queued work must be discovered at doorbell latency.
 */
class StealTest : public ::testing::Test
{
protected:
    /** Placement trials per test. */
    static constexpr int TRIAL_COUNT = 10;

    /** Discovery trials per test. */
    static constexpr int LATENCY_TRIAL_COUNT = 5;

    /** Latency probes queued behind the stall in each discovery trial. */
    static constexpr int QUEUED_COUNT = 8;

    /** Solo runs of the compute loop; the minimum is the uncontended duration. */
    static constexpr int SOLO_ATTEMPTS = 3;

    /** Target uncontended duration of one compute fiber. */
    static constexpr uint64_t STALL_NS = 20'000'000;

    /** Idle time between trials letting the poller prefix decay and settle. */
    static constexpr uint64_t SETTLE_US = 200'000;

    /** In/out block for one fixed-work compute fiber. */
    struct ComputeContext
    {
        /** Iterations of the compute loop to run. */
        uint64_t iterations;

        /** CPU the fiber ran on. */
        int cpu;

        /** Measured wall duration of the compute loop in TSC cycles. */
        uint64_t durationCycles;
    };

    /** In/out block for one enqueue-to-run latency probe fiber. */
    struct LatencyContext
    {
        /** TSC timestamp taken immediately before the fiber is scheduled. */
        uint64_t spawnCycles;

        /** TSC timestamp taken as the first statement of the fiber. */
        uint64_t runCycles;
    };

    /** In/out block for one placement trial: two equal compute fibers spawned back to back. */
    struct TrialContext
    {
        /** Iterations for both compute fibers. */
        uint64_t iterations;

        /** The compute fiber that occupies the spawning processor. */
        ComputeContext first;

        /** The compute fiber that must come online elsewhere. */
        ComputeContext second;
    };

    /** In/out block for one discovery trial: a stall fiber with latency probes queued behind it. */
    struct LatencyTrialContext
    {
        /** Iterations for the stall fiber. */
        uint64_t stallIterations;

        /** The stall fiber occupying the spawning processor. */
        ComputeContext stall;

        /** The probes queued behind the stall. */
        LatencyContext queued[QUEUED_COUNT];
    };

    /** Fiber entry: run the fixed-work loop, record the CPU and the measured duration. */
    static int computeFiber(ComputeContext ** context) noexcept;

    /** Fiber entry: record the dispatch timestamp. */
    static int recordLatencyFiber(LatencyContext ** context) noexcept;

    /** Fiber entry: spawn the trial's two compute fibers back to back and wait for both. */
    static int spawnTrialFiber(TrialContext ** context) noexcept;

    /** Fiber entry: spawn the stall fiber, queue the latency probes behind it, wait for all. */
    static int spawnLatencyTrialFiber(LatencyTrialContext ** context) noexcept;

    /** Burn CPU for a fixed amount of work the optimizer cannot elide. */
    static void runComputeLoop(uint64_t iterations) noexcept;

    /** Return the compute-loop iteration count whose uncontended duration is about STALL_NS. */
    static uint64_t calibrateIterations() noexcept;

    /** Return the uncontended duration of the compute loop as the minimum of solo runs. */
    static uint64_t measureSoloCycles(uint64_t iterations) noexcept;

    /** Return the index in siblingPairs of the physical core @p cpu belongs to, -1 if none. */
    static int findSiblingPair(int cpu) noexcept;

public:
    /** The four active CPUs as two sibling pairs; a pair shares one physical core. Populated by main. */
    static int siblingPairs[2][2];

    /** Whether main found two whole cores in the affinity mask; every test skips otherwise. */
    static bool topologyReady;
};

int StealTest::siblingPairs[2][2];
bool StealTest::topologyReady = false;

int StealTest::computeFiber(ComputeContext ** context) noexcept
{
    ComputeContext * compute = *context;

    uint64_t startCycles = Tsc::getCycles();
    runComputeLoop(compute->iterations);
    compute->durationCycles = Tsc::getCycles() - startCycles;
    compute->cpu = getCurrentProcessor();
    return 0;
}

int StealTest::recordLatencyFiber(LatencyContext ** context) noexcept
{
    LatencyContext * latency = *context;
    latency->runCycles = Tsc::getCycles();
    return 0;
}

int StealTest::spawnTrialFiber(TrialContext ** context) noexcept
{
    TrialContext * trial = *context;
    trial->first.iterations = trial->iterations;
    trial->second.iterations = trial->iterations;

    FiberFuture firstFuture;
    FiberFuture secondFuture;
    int r = FiberScheduler::run(computeFiber, &trial->first, &firstFuture);
    SILK_ASSERT(!r);
    r = FiberScheduler::run(computeFiber, &trial->second, &secondFuture);
    SILK_ASSERT(!r);

    int firstResult = firstFuture.wait();
    int secondResult = secondFuture.wait();
    SILK_ASSERT(!firstResult && !secondResult);
    return 0;
}

int StealTest::spawnLatencyTrialFiber(LatencyTrialContext ** context) noexcept
{
    LatencyTrialContext * trial = *context;
    trial->stall.iterations = trial->stallIterations;

    FiberFuture stallFuture;
    int r = FiberScheduler::run(computeFiber, &trial->stall, &stallFuture);
    SILK_ASSERT(!r);

    FiberFuture queuedFutures[QUEUED_COUNT];
    for (int i = 0; i < QUEUED_COUNT; ++i)
    {
        trial->queued[i].spawnCycles = Tsc::getCycles();
        r = FiberScheduler::run(recordLatencyFiber, &trial->queued[i], &queuedFutures[i]);
        SILK_ASSERT(!r);
    }

    int stallResult = stallFuture.wait();
    SILK_ASSERT(!stallResult);

    for (int i = 0; i < QUEUED_COUNT; ++i)
    {
        int queuedResult = queuedFutures[i].wait();
        SILK_ASSERT(!queuedResult);
    }

    return 0;
}

void StealTest::runComputeLoop(uint64_t iterations) noexcept
{
    volatile uint64_t accumulator = 0;

    for (uint64_t i = 0; i < iterations; ++i)
    {
        accumulator = accumulator + i;
    }
}

uint64_t StealTest::calibrateIterations() noexcept
{
    ComputeContext probe;
    probe.iterations = 1 << 22;

    int r = FiberScheduler::run(computeFiber, &probe);
    SILK_ASSERT(!r);
    SILK_ASSERT(probe.durationCycles > 0);

    uint64_t targetCycles = Tsc::nanosecondsToCycles(STALL_NS);
    return probe.iterations * targetCycles / probe.durationCycles;
}

uint64_t StealTest::measureSoloCycles(uint64_t iterations) noexcept
{
    uint64_t soloCycles = UINT64_MAX;

    for (int attempt = 0; attempt < SOLO_ATTEMPTS; ++attempt)
    {
        ComputeContext solo;
        solo.iterations = iterations;

        int r = FiberScheduler::run(computeFiber, &solo);
        SILK_ASSERT(!r);

        soloCycles = std::min(soloCycles, solo.durationCycles);
    }

    return soloCycles;
}

int StealTest::findSiblingPair(int cpu) noexcept
{
    for (int pair = 0; pair < 2; ++pair)
    {
        if (siblingPairs[pair][0] == cpu || siblingPairs[pair][1] == cpu)
        {
            return pair;
        }
    }

    return -1;
}

// Two equal compute fibers spawned back to back must come online on different
// physical cores and run without SMT stretch - a whole core sits parked, so
// engaging the busy core's sibling pays interference for capacity that is free.
TEST_F(StealTest, computeFibersNeverSharePhysicalCore)
{
    if (!topologyReady)
    {
        GTEST_SKIP() << "needs two whole physical cores with both HT siblings";
    }

    uint64_t iterations = calibrateIterations();
    uint64_t soloCycles = measureSoloCycles(iterations);
    ASSERT_GT(soloCycles, 0u);

    // Let every processor park before the first trial - the wake-target choice is
    // meaningful only against settled processors.
    ::usleep(SETTLE_US);

    int sharedCoreTrials = 0;
    uint64_t maxStretchPercent = 0;

    for (int trial = 0; trial < TRIAL_COUNT; ++trial)
    {
        TrialContext context;
        context.iterations = iterations;

        int r = FiberScheduler::run(spawnTrialFiber, &context);
        ASSERT_EQ(r, 0);

        int firstPair = findSiblingPair(context.first.cpu);
        int secondPair = findSiblingPair(context.second.cpu);
        ASSERT_GE(firstPair, 0);
        ASSERT_GE(secondPair, 0);

        uint64_t firstStretchPercent = context.first.durationCycles * 100 / soloCycles;
        uint64_t secondStretchPercent = context.second.durationCycles * 100 / soloCycles;
        maxStretchPercent = std::max({maxStretchPercent, firstStretchPercent, secondStretchPercent});

        bool sharedCore = firstPair == secondPair;
        if (sharedCore)
        {
            sharedCoreTrials++;
        }

        printf(
            "trial %d: cpus %d/%d%s stretch %lu%%/%lu%%\n",
            trial,
            context.first.cpu,
            context.second.cpu,
            sharedCore ? " (shared core)" : "",
            firstStretchPercent,
            secondStretchPercent);

        ::usleep(SETTLE_US);
    }

    printf("shared-core trials: %d/%d, max stretch: %lu%%\n", sharedCoreTrials, TRIAL_COUNT, maxStretchPercent);

    ASSERT_EQ(sharedCoreTrials, 0);
    ASSERT_LT(maxStretchPercent, 125u);
}

// Probes queued behind a stalled processor must be discovered within the
// width-adaptation time constant - the standby's probe cadence bounds a cold
// rescue at roughly two of them; a lost wakeup would show up here as an
// unbounded stall.
TEST_F(StealTest, backlogDiscoveryLatency)
{
    if (!topologyReady)
    {
        GTEST_SKIP() << "needs two whole physical cores with both HT siblings";
    }

    uint64_t stallIterations = calibrateIterations();
    ::usleep(SETTLE_US);

    uint64_t latenciesNs[LATENCY_TRIAL_COUNT * QUEUED_COUNT];

    for (int trial = 0; trial < LATENCY_TRIAL_COUNT; ++trial)
    {
        LatencyTrialContext context;
        context.stallIterations = stallIterations;

        int r = FiberScheduler::run(spawnLatencyTrialFiber, &context);
        ASSERT_EQ(r, 0);

        for (int i = 0; i < QUEUED_COUNT; ++i)
        {
            uint64_t latencyCycles = context.queued[i].runCycles - context.queued[i].spawnCycles;
            latenciesNs[trial * QUEUED_COUNT + i] = Tsc::cyclesToNanoseconds(latencyCycles);
        }

        ::usleep(SETTLE_US);
    }

    std::sort(latenciesNs, latenciesNs + LATENCY_TRIAL_COUNT * QUEUED_COUNT);
    uint64_t medianNs = latenciesNs[LATENCY_TRIAL_COUNT * QUEUED_COUNT / 2];
    uint64_t maxNs = latenciesNs[LATENCY_TRIAL_COUNT * QUEUED_COUNT - 1];

    printf("backlog discovery latency: median %lu us, max %lu us\n", medianNs / 1000, maxNs / 1000);

    ASSERT_LT(medianNs, 30'000'000u);
    ASSERT_LT(maxNs, 100'000'000u);
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

    uint32_t processorCount = silk::getProcessorCount();
    std::vector<silk::CpuTopology> topologies(processorCount);
    silk::readCpuTopologies(topologies.data(), processorCount);

    // Pick two whole physical cores: the first two distinct (package, core)
    // groups with at least two available HT siblings each.
    int pairsFound = 0;
    for (uint32_t cpu = 0; cpu < processorCount && pairsFound < 2; ++cpu)
    {
        if (!CPU_ISSET(cpu, &affinity) || topologies[cpu].coreId == UINT32_MAX)
        {
            continue;
        }

        for (uint32_t other = cpu + 1; other < processorCount; ++other)
        {
            bool sameCore = topologies[other].packageId == topologies[cpu].packageId && topologies[other].coreId == topologies[cpu].coreId;
            if (CPU_ISSET(other, &affinity) && sameCore)
            {
                silk::StealTest::siblingPairs[pairsFound][0] = cpu;
                silk::StealTest::siblingPairs[pairsFound][1] = other;
                pairsFound++;
                break;
            }
        }
    }

    silk::FiberScheduler::Options options;
    if (pairsFound == 2)
    {
        CPU_ZERO(&options.cpuMask);

        for (int pair = 0; pair < 2; ++pair)
        {
            for (int side = 0; side < 2; ++side)
            {
                CPU_SET(silk::StealTest::siblingPairs[pair][side], &options.cpuMask);
            }
        }

        silk::StealTest::topologyReady = true;
    }

    silk::FiberScheduler::initialize(&options);

    r = RUN_ALL_TESTS();

    silk::FiberScheduler::destroy();
    silk::destroy();
    return r;
}
