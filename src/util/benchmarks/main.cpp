#include <silk/util/platform.h>

#include <benchmark/benchmark.h>

int main(int argc, char ** argv)
{
    silk::initRseq();

    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
