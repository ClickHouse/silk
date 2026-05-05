#include <silk/util/perf.h>

#include <benchmark/benchmark.h>

#include <vector>

// clang-format off
#define BENCH_SIMPLE_COUNTERS(x) \
    x(SIMPLE_COUNTER_A, "SimpleCounterA") \
    x(SIMPLE_COUNTER_B, "SimpleCounterB")

#define BENCH_MEM_COUNTERS(x) \
    x(MEM_COUNTER_A, "MemCounterA") \
    x(MEM_COUNTER_B, "MemCounterB")
// clang-format on

namespace silk
{

DECLARE_SIMPLE_COUNTERS(BENCH_SIMPLE_COUNTERS);
DECLARE_MEM_COUNTERS(BENCH_MEM_COUNTERS);

class PerfBench : public benchmark::Fixture
{
public:
    void SetUp(const benchmark::State & state) override
    {
        if (state.thread_index() == 0)
        {
            Perf::initialize();

            simpleGroup = {};
            memGroup = {};
            globalTotal.reset();

            REGISTER_SIMPLE_COUNTERS(&simpleGroup, BENCH_SIMPLE_COUNTERS);
            REGISTER_MEM_COUNTERS(&memGroup, BENCH_MEM_COUNTERS);
        }
    }

    void TearDown(const benchmark::State & state) override
    {
        if (state.thread_index() == 0)
        {
            Perf::destroy();
        }
    }

    Perf::CounterGroup simpleGroup;
    Perf::CounterGroup memGroup;

    Perf::SimpleCounter globalTotal;
};

BENCHMARK_DEFINE_F(PerfBench, SimpleCounterIncrement)(benchmark::State & state)
{
    for (auto _ : state)
    {
        Perf::getSimpleCounter(simpleGroup[SIMPLE_COUNTER_A]).increment();
    }
}
BENCHMARK_REGISTER_F(PerfBench, SimpleCounterIncrement)->ThreadRange(1, getAvailableProcessorCount());

// Per-CPU increment + swap-and-flush to a shared global counter every 16 iterations,
// plus a read of the global on every iteration. Models checking an approximate total
// (e.g. memory limit) on every allocation without full aggregation.
BENCHMARK_DEFINE_F(PerfBench, SimpleCounterIncrementWithFlush)(benchmark::State & state)
{
    static constexpr uint32_t FLUSH_INTERVAL = 64;
    uint32_t pending = 0;

    for (auto _ : state)
    {
        // benchmark::DoNotOptimize(globalTotal.value.load(std::memory_order_relaxed));

        Perf::SimpleCounter & c = Perf::getSimpleCounter(simpleGroup[SIMPLE_COUNTER_A]);
        c.increment();

        if (++pending % FLUSH_INTERVAL == 0)
        {
            c.flush(globalTotal);
        }
    }
}
BENCHMARK_REGISTER_F(PerfBench, SimpleCounterIncrementWithFlush)->ThreadRange(1, getAvailableProcessorCount());

BENCHMARK_F(PerfBench, GetSimpleCounters)(benchmark::State & state)
{
    uint32_t count = Perf::getSimpleCounterCount();
    std::vector<Perf::SimpleCounter> out(count);

    for (auto _ : state)
    {
        benchmark::DoNotOptimize(Perf::getSimpleCounters(0, out.data(), count));
    }
}

BENCHMARK_F(PerfBench, MemCounterAlloc)(benchmark::State & state)
{
    for (auto _ : state)
    {
        Perf::getMemCounter(memGroup[MEM_COUNTER_A]).alloc(64);
    }
}

BENCHMARK_F(PerfBench, GetMemCounters)(benchmark::State & state)
{
    uint32_t count = Perf::getMemCounterCount();
    std::vector<Perf::MemCounter> out(count);

    for (auto _ : state)
    {
        benchmark::DoNotOptimize(Perf::getMemCounters(0, out.data(), count));
    }
}

} // namespace silk
