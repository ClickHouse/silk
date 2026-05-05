#include <silk/fibers/fiber.h>
#include <silk/util/perf.h>
#include <silk/util/queue.h>

#include <benchmark/benchmark.h>

int main(int argc, char ** argv)
{
    silk::initRseq();
    silk::Perf::initialize();
    silk::QueueBase::initialize();
    silk::FiberScheduler::initialize();

    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();

    silk::FiberScheduler::destroy();
    silk::QueueBase::destroy();
    silk::Perf::destroy();
    return 0;
}
