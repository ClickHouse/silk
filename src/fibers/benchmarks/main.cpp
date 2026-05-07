#include <silk/fibers/fiber.h>
#include <silk/util/init.h>

#include <benchmark/benchmark.h>

int main(int argc, char ** argv)
{
    silk::initialize();
    silk::FiberScheduler::initialize();

    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();

    silk::FiberScheduler::destroy();
    silk::destroy();
    return 0;
}
