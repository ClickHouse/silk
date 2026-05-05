#include <silk/util/tsc.h>

#include <silk/util/platform.h>

#include <benchmark/benchmark.h>

namespace silk
{

class TscBench : public benchmark::Fixture
{
};

BENCHMARK_F(TscBench, GetCycles)(benchmark::State & state)
{
    for (auto _ : state)
    {
        benchmark::DoNotOptimize(Tsc::getCycles());
    }
}

BENCHMARK_F(TscBench, GetNanoseconds)(benchmark::State & state)
{
    for (auto _ : state)
    {
        benchmark::DoNotOptimize(Tsc::cyclesToNanoseconds(Tsc::getCycles()));
    }
}

BENCHMARK_F(TscBench, GetTimeNanoseconds)(benchmark::State & state)
{
    for (auto _ : state)
    {
        benchmark::DoNotOptimize(getTimeNanoseconds());
    }
}

} // namespace silk
