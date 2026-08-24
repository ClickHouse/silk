#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>
#include <silk/util/assert.h>
#include <silk/util/crash-dumper.h>
#include <silk/util/init.h>
#include <silk/util/perf.h>
#include <silk/util/platform.h>

#include <benchmark/benchmark.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <vector>

#include <sched.h>

#include <fibers/cpu.h>

namespace silk
{

/**
 * The scheduler is restricted to a strict subset of the available CPUs, leaving
 * excludedCpu online but off the active set. The benchmarks inject work from a
 * thread pinned to an active core and from one pinned to the reserved core: the
 * pair compares the two operating points a cpuMask user chooses between. The arms
 * differ in park and wake behavior and in core sharing, not only in the injection
 * redirect, so the delta prices the operating point, not the redirect.
 */
class CpuSetBench : public benchmark::Fixture
{
protected:
    /** Move the calling thread onto @p cpu. */
    static void pinToCpu(int cpu) noexcept
    {
        int r = pinThreadToCpu(static_cast<uint16_t>(cpu));
        SILK_ASSERT(!r, "could not pin the injecting thread: r=%d", r);
    }

    /** Accumulated value of the named simple counter across every CPU slot, or 0 when no counter carries that name. */
    static uint64_t readCounter(const char * name) noexcept;

    /**
     * Ring of numberOfFibers in-flight no-op fibers injected from the calling
     * thread: each iteration joins the oldest and injects a replacement, so the
     * cost measured is one injection plus one join. Mirrors
     * WorkStealingThreadProducer, with the caller's CPU as the only variable.
     *
     * Reports, per iteration, the scheduler parks and wakes the loop caused.
     * Parks counts threads that actually parked, each paying the park syscall
     * itself; wakes counts signals sent to a thread that advertised itself as
     * sleeping, each costing the signaling side a wakeup syscall. A thread
     * backing out of the park path makes wakes exceed parks. Counters are
     * process-wide, so idle-thread background activity rides along.
     */
    static void injectFromCurrentCpu(benchmark::State & state, uint64_t numberOfFibers) noexcept;

public:
    /** CPUs in the scheduler's active set. Populated by main. */
    static std::vector<int> activeCpus;

    /** The reserved CPU. Populated by main; -1 when there are too few CPUs to reserve one, and every benchmark skips. */
    static int excludedCpu;
};

std::vector<int> CpuSetBench::activeCpus;
int CpuSetBench::excludedCpu = -1;

uint64_t CpuSetBench::readCounter(const char * name) noexcept
{
    uint32_t count = Perf::getSimpleCounterCount();
    std::vector<Perf::SimpleCounter> out(count);
    count = Perf::getSimpleCounters(0, out.data(), count);

    for (uint32_t i = 0; i < count; ++i)
    {
        int r = std::strcmp(Perf::getSimpleCounterInfo(i).name, name);
        if (!r)
        {
            return out[i].value.load(std::memory_order_relaxed);
        }
    }

    return 0;
}

void CpuSetBench::injectFromCurrentCpu(benchmark::State & state, uint64_t numberOfFibers) noexcept
{
    struct Params
    {
        static int fiberMain(Params *) noexcept { return 0; }
    };

    auto futures = std::make_unique<FiberFuture[]>(numberOfFibers);
    for (uint64_t i = 0; i < numberOfFibers; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, {}, &futures[i]);
        SILK_ASSERT(!r);
    }

    uint64_t parkedBefore = readCounter("SchedulerThreadParked");
    uint64_t wakedBefore = readCounter("SchedulerThreadWaked");

    uint64_t pos = 0;
    for (auto _ : state)
    {
        futures[pos % numberOfFibers].wait();
        futures[pos % numberOfFibers].reset();
        int r = FiberScheduler::run(Params::fiberMain, {}, &futures[pos % numberOfFibers]);
        SILK_ASSERT(!r);
        ++pos;
    }

    uint64_t parked = readCounter("SchedulerThreadParked") - parkedBefore;
    uint64_t waked = readCounter("SchedulerThreadWaked") - wakedBefore;

    for (uint64_t i = 0; i < numberOfFibers; ++i)
    {
        futures[(pos + i) % numberOfFibers].wait();
    }

    state.counters["parks"] = benchmark::Counter(static_cast<double>(parked), benchmark::Counter::kAvgIterations);
    state.counters["wakes"] = benchmark::Counter(static_cast<double>(waked), benchmark::Counter::kAvgIterations);
    state.SetItemsProcessed(state.iterations());
}

// Injection from an active core: homeProcessor maps the caller's CPU to its own
// processor, so the work lands on the ring of the scheduler thread pinned to that
// same core. The injecting thread shares the logical CPU with that scheduler
// thread.
BENCHMARK_DEFINE_F(CpuSetBench, InjectFromActiveCpu)(benchmark::State & state)
{
    if (excludedCpu < 0)
    {
        state.SkipWithMessage("needs at least two available CPUs");
        return;
    }

    pinToCpu(activeCpus.front());
    injectFromCurrentCpu(state, static_cast<uint64_t>(state.range(0)));
}
BENCHMARK_REGISTER_F(CpuSetBench, InjectFromActiveCpu)->Arg(1)->Arg(16)->Arg(64)->UseRealTime();

// Injection from the reserved core: the caller owns a core silk schedules nothing
// on, and homeProcessor routes the work to an active processor's ring on another
// core. Pairs with InjectFromActiveCpu at the same ring depth.
BENCHMARK_DEFINE_F(CpuSetBench, InjectFromReservedCpu)(benchmark::State & state)
{
    if (excludedCpu < 0)
    {
        state.SkipWithMessage("needs at least two available CPUs");
        return;
    }

    pinToCpu(excludedCpu);
    injectFromCurrentCpu(state, static_cast<uint64_t>(state.range(0)));
}
BENCHMARK_REGISTER_F(CpuSetBench, InjectFromReservedCpu)->Arg(1)->Arg(16)->Arg(64)->UseRealTime();

} // namespace silk

// Whether the command line asks for the benchmark list. bb enumerates every
// *-bench binary with the bare --benchmark_list_tests flag, and listing must not
// pay for (or fail on) scheduler initialization.
static bool isListOnlyRun(int argc, char ** argv) noexcept
{
    for (int i = 1; i < argc; ++i)
    {
        int r = std::strcmp(argv[i], "--benchmark_list_tests");
        if (!r)
        {
            return true;
        }
    }

    return false;
}

int main(int argc, char ** argv)
{
    if (isListOnlyRun(argc, argv))
    {
        benchmark::Initialize(&argc, argv);
        benchmark::RunSpecifiedBenchmarks();
        benchmark::Shutdown();
        return 0;
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
    // it stays online so the benchmarks can migrate onto it.
    if (available.size() >= 2)
    {
        silk::CpuSetBench::excludedCpu = available.back();
        CPU_CLR(silk::CpuSetBench::excludedCpu, &options.cpuMask);
        for (int cpu : available)
        {
            if (cpu != silk::CpuSetBench::excludedCpu)
            {
                silk::CpuSetBench::activeCpus.push_back(cpu);
            }
        }
    }

    silk::FiberScheduler::initialize(&options);

    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();

    silk::FiberScheduler::destroy();
    silk::destroy();
    return 0;
}
