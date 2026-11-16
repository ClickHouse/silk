#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>
#include <silk/util/assert.h>
#include <silk/util/crash-dumper.h>
#include <silk/util/init.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>

namespace silk
{

/**
 * Checks the fiber switch hooks against the parameters of the fiber they report, the way a hook that
 * keeps per-fiber state in the parameters uses them: every fiberResume and fiberSuspend, including the
 * fiberSuspend that follows the final switch, sees the parameters alive, and the parameters are destroyed
 * after that last fiberSuspend and before the fiber's result is delivered. The hooks are scheduler
 * options, so this binary initializes its own scheduler with them.
 */
class SwitchHookTest : public ::testing::Test
{
protected:
    /** Category of the fibers the hooks check; the hooks ignore every other fiber. */
    static constexpr uint8_t HOOK_CATEGORY = 1;

    /** Fibers the test runs at once. */
    static constexpr int FIBER_COUNT = 64;

    /** Yields of each fiber, so the hooks fire on many switches of the same fiber. */
    static constexpr int YIELD_COUNT = 8;

    /** What the hooks and the parameters destructor observe for one fiber. */
    struct HookRecord
    {
        /** fiberResume calls for the fiber. */
        int resumeCount;

        /** fiberSuspend calls for the fiber. */
        int suspendCount;

        /** Destructor calls of the fiber's copy of the parameters. */
        int destroyCount;

        /** suspendCount when the destructor ran. */
        int suspendCountAtDestroy;
    };

    /** Parameters of a checked fiber; the hooks find them through getFiberParameters. */
    struct HookParams
    {
        /** Point the parameters at the fiber's record. */
        HookParams(HookRecord * record) noexcept;

        /** Take the record of other, which keeps none. */
        HookParams(HookParams && other) noexcept;

        /** Count the destruction in the record, if the object has one. */
        ~HookParams() noexcept;

        /** Record of the fiber; null in a moved-from object. */
        HookRecord * record;
    };

    /** Fiber entry: yield YIELD_COUNT times. */
    static int yieldFiberMain(HookParams * params) noexcept;

    /** Return the record of a checked fiber after checking that its parameters are alive; null for other fibers. */
    static HookRecord * getHookRecord(Fiber * fiber) noexcept;

public:
    /** fiberResume hook: count the resume of a checked fiber. */
    static void onFiberResume(Fiber * fiber) noexcept;

    /** fiberSuspend hook: count the suspend of a checked fiber. */
    static void onFiberSuspend(Fiber * fiber) noexcept;
};

SwitchHookTest::HookParams::HookParams(HookRecord * record) noexcept
    : record(record)
{
}

SwitchHookTest::HookParams::HookParams(HookParams && other) noexcept
    : record(std::exchange(other.record, nullptr))
{
}

SwitchHookTest::HookParams::~HookParams() noexcept
{
    if (record)
    {
        ++record->destroyCount;
        record->suspendCountAtDestroy = record->suspendCount;
    }
}

int SwitchHookTest::yieldFiberMain(HookParams * params) noexcept
{
    SILK_UNUSED(params);

    for (int i = 0; i < YIELD_COUNT; ++i)
    {
        FiberScheduler::yield();
    }

    return 0;
}

void SwitchHookTest::onFiberResume(Fiber * fiber) noexcept
{
    HookRecord * record = getHookRecord(fiber);
    if (record)
    {
        ++record->resumeCount;
    }
}

void SwitchHookTest::onFiberSuspend(Fiber * fiber) noexcept
{
    HookRecord * record = getHookRecord(fiber);
    if (record)
    {
        ++record->suspendCount;
    }
}

SwitchHookTest::HookRecord * SwitchHookTest::getHookRecord(Fiber * fiber) noexcept
{
    if (FiberScheduler::getCurrentFiberId().category == HOOK_CATEGORY)
    {
        // The fiber pool never frees a fiber, so a hook that runs after the destructor still reads the
        // record pointer and finds the destructor's count.
        HookParams * params = static_cast<HookParams *>(FiberScheduler::getFiberParameters(fiber));
        SILK_ASSERT(!params->record->destroyCount, "a switch hook ran after the fiber's parameters were destroyed");
        return params->record;
    }

    return nullptr;
}

// Every hook call sees the parameters alive, the calls balance, and the destructor runs once - after the
// last fiberSuspend and before the result is delivered. The records are plain ints, so TSan reports a race
// if the destructor runs after the result is delivered.
TEST_F(SwitchHookTest, parametersOutliveLastSuspend)
{
    HookRecord records[FIBER_COUNT] = {};
    FiberFuture futures[FIBER_COUNT];

    for (int i = 0; i < FIBER_COUNT; ++i)
    {
        int r = FiberScheduler::run(yieldFiberMain, HookParams(&records[i]), HOOK_CATEGORY, &futures[i]);
        SILK_ASSERT(!r, "could not spawn the hooked fiber: r=%d", r);
    }

    for (int i = 0; i < FIBER_COUNT; ++i)
    {
        int r = futures[i].wait();
        SILK_ASSERT(!r, "the hooked fiber failed: r=%d", r);
    }

    for (int i = 0; i < FIBER_COUNT; ++i)
    {
        ASSERT_EQ(records[i].resumeCount, YIELD_COUNT + 1) << "fiber " << i;
        ASSERT_EQ(records[i].suspendCount, YIELD_COUNT + 1) << "fiber " << i;
        ASSERT_EQ(records[i].destroyCount, 1) << "fiber " << i;
        ASSERT_EQ(records[i].suspendCountAtDestroy, YIELD_COUNT + 1) << "fiber " << i;
    }
}

} // namespace silk

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
        .fiberSuspend = silk::SwitchHookTest::onFiberSuspend,
        .fiberResume = silk::SwitchHookTest::onFiberResume,
    };
    silk::FiberScheduler::initialize(&options);

    int r = RUN_ALL_TESTS();

    silk::FiberScheduler::destroy();
    silk::destroy();
    return r;
}
