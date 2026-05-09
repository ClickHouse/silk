#include <silk/fibers/fiber.h>
#include <silk/util/init.h>

#include <gtest/gtest.h>

int main(int argc, char ** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    if (::testing::GTEST_FLAG(list_tests))
    {
        return RUN_ALL_TESTS();
    }

    silk::initialize();
    silk::FiberScheduler::Options options{.enableProfiler = true};
    silk::FiberScheduler::initialize(&options);

    int r = RUN_ALL_TESTS();

    silk::FiberScheduler::destroy();
    silk::destroy();
    return r;
}
