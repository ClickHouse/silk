#include <silk/fibers/fiber.h>
#include <silk/util/crash-dumper.h>
#include <silk/util/init.h>

#include <gtest/gtest.h>

#include <atomic>

namespace
{

std::atomic<bool> hooksOnSchedulerStack{true};
std::atomic<unsigned> resumeCount{0};
std::atomic<unsigned> suspendCount{0};

void onFiberResume(silk::Fiber *) noexcept
{
    if (!silk::FiberScheduler::isOnSchedulerStack())
        hooksOnSchedulerStack.store(false, std::memory_order_relaxed);
    resumeCount.fetch_add(1, std::memory_order_relaxed);
}

void onFiberSuspend(silk::Fiber *) noexcept
{
    if (!silk::FiberScheduler::isOnSchedulerStack())
        hooksOnSchedulerStack.store(false, std::memory_order_relaxed);
    suspendCount.fetch_add(1, std::memory_order_relaxed);
}

}

TEST(FiberScheduler, switchHooksRunOnSchedulerStack)
{
    const auto previousResumes = resumeCount.load(std::memory_order_relaxed);
    const auto previousSuspends = suspendCount.load(std::memory_order_relaxed);
    auto task = +[](int *) noexcept { return silk::FiberScheduler::isOnSchedulerStack() ? 1 : 0; };

    EXPECT_EQ(silk::FiberScheduler::run(task, 0), 0);
    EXPECT_GT(resumeCount.load(std::memory_order_relaxed), previousResumes);
    EXPECT_GT(suspendCount.load(std::memory_order_relaxed), previousSuspends);
    EXPECT_TRUE(hooksOnSchedulerStack.load(std::memory_order_relaxed));
}

int main(int argc, char ** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    if (::testing::GTEST_FLAG(list_tests))
    {
        return RUN_ALL_TESTS();
    }

    silk::installCrashDumper();

    silk::initialize();
    silk::FiberScheduler::Options options{
        .enableProfiler = true,
        .fiberSuspend = &onFiberSuspend,
        .fiberResume = &onFiberResume,
    };
    silk::FiberScheduler::initialize(&options);

    int r = RUN_ALL_TESTS();

    silk::FiberScheduler::destroy();
    silk::destroy();
    return r;
}
