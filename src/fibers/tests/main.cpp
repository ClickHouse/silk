#include <silk/fibers/fiber.h>
#include <silk/util/perf.h>
#include <silk/util/queue.h>

#include <gtest/gtest.h>

int main(int argc, char ** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    // Skip scheduler initialization
    if (::testing::GTEST_FLAG(list_tests))
    {
        return RUN_ALL_TESTS();
    }

    silk::initRseq();
    silk::Perf::initialize();
    silk::QueueBase::initialize();
    silk::FiberScheduler::initialize();

    int result = RUN_ALL_TESTS();

    silk::FiberScheduler::destroy();
    silk::QueueBase::destroy();
    silk::Perf::destroy();

    return result;
}
