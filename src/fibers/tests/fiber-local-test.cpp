#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>

#include <gtest/gtest.h>

#include <thread>

#ifdef SILK_FIBER_LOCAL_STORAGE

namespace silk
{

namespace
{
int getValue() noexcept
{
    return *static_cast<int *>(FiberScheduler::getLocalStorage());
}
void saveValue(int value) noexcept
{
    *static_cast<int *>(FiberScheduler::getLocalStorage()) = value;
}
}

// A fiber inherits its launching thread's values on entry
// and keeps its own values across suspend/resume.
TEST(FiberLocalStorage, inheritsSurvivesYieldAndIsolated)
{
    struct Params
    {
        int expectedInitialValue;
        int fiberId;

        static int fiberMain(Params * p) noexcept
        {
            EXPECT_EQ(getValue(), p->expectedInitialValue);
            saveValue(p->fiberId);
            FiberScheduler::yield();
            EXPECT_EQ(getValue(), p->fiberId);
            return 0;
        }
    };

    auto launcher = [](int threadIndex)
    {
        int launcherValue = threadIndex;

        // Non-fibers should be able to use the buffer.
        saveValue(launcherValue);

        static constexpr int FIBERS_PER_THREAD = 16;
        FiberFuture futures[FIBERS_PER_THREAD];
        for (int i = 0; i < FIBERS_PER_THREAD; ++i)
        {
            int fiberId = threadIndex * FIBERS_PER_THREAD + i;
            int r = FiberScheduler::run(Params::fiberMain, Params{launcherValue, fiberId}, &futures[i]);
            EXPECT_EQ(r, 0);
        }
        for (auto & future : futures)
        {
            future.wait();
        }

        EXPECT_EQ(getValue(), launcherValue);
    };

    std::thread t0(launcher, 1);
    std::thread t1(launcher, 2);
    t0.join();
    t1.join();
}

} // namespace silk

#endif // SILK_FIBER_LOCAL_STORAGE
