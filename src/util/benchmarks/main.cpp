#include <silk/util/init.h>

#include <benchmark/benchmark.h>

int main(int argc, char ** argv)
{
    silk::initialize();

    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();

    silk::destroy();
    return 0;
}
