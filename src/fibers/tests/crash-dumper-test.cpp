#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>
#include <silk/util/assert.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>

#include <unistd.h>

namespace silk
{

// Manual check that the crash-dumper wired into the unit-test main fires inside the test binary itself. With
// CRASH_DUMPER_HANG set, park a few fibers and block forever, so a short ctest --timeout delivers SIGQUIT
// (TIMEOUT_SIGNAL_NAME) and the dumper prints every OS thread plus the silk fiber list before the process
// exits with the watchdog code. Skipped otherwise so it never stalls a normal run. Exercise it once with:
//   CRASH_DUMPER_HANG=1 ./bb test -R CrashDumperTest.HangDumpsThreadsAndFibers --timeout 5
TEST(CrashDumperTest, HangDumpsThreadsAndFibers)
{
    const char * hangRequested = std::getenv("CRASH_DUMPER_HANG");
    if (!hangRequested)
    {
        GTEST_SKIP() << "set CRASH_DUMPER_HANG and a short --timeout to exercise the crash-dumper";
    }

    struct HangFiber
    {
        // Parks forever on a never-set future, so the crash-dumper's fiber list has suspended fibers to report.
        static int fiberMain(HangFiber * params) noexcept
        {
            params->block->wait();
            return 0;
        }

        silk::FiberFuture * block;
    };

    constexpr uint32_t fiberCount = 4;
    silk::FiberFuture block[fiberCount];
    silk::FiberFuture join[fiberCount];

    // Park a few fibers fire-and-forget so the dump's fiber list is non-trivial.
    for (uint32_t i = 1; i < fiberCount; ++i)
    {
        int r = silk::FiberScheduler::run(HangFiber::fiberMain, {&block[i]}, &join[i]);
        SILK_ASSERT(r == 0);
    }

    for (;;)
    {
        sleep(1);
    }
}

// Companion check for the crash path: with CRASH_DUMPER_CRASH set, park a few fibers then dereference null. The
// crash-dumper catches SIGSEGV, prints every OS thread (with the crash site) plus the silk fiber list, then
// re-raises so the process still dies of the original signal. Skipped otherwise. Exercise it with:
//   CRASH_DUMPER_CRASH=1 ./bb test -R CrashDumperTest.SegfaultDumpsThreadsAndFibers
TEST(CrashDumperTest, SegfaultDumpsThreadsAndFibers)
{
    const char * crashRequested = std::getenv("CRASH_DUMPER_CRASH");
    if (!crashRequested)
    {
        GTEST_SKIP() << "set CRASH_DUMPER_CRASH to exercise the crash-dumper's segfault path";
    }

    struct HangFiber
    {
        static int fiberMain(HangFiber * params) noexcept
        {
            params->block->wait();
            return 0;
        }

        silk::FiberFuture * block;
    };

    constexpr uint32_t fiberCount = 4;
    silk::FiberFuture block[fiberCount];
    silk::FiberFuture join[fiberCount];

    for (uint32_t i = 0; i < fiberCount; ++i)
    {
        int r = silk::FiberScheduler::run(HangFiber::fiberMain, {&block[i]}, &join[i]);
        SILK_ASSERT(r == 0);
    }

    // Let the fibers park so they show in the dump, then crash on this thread. The pointer is volatile so the
    // compiler cannot fold the null dereference away.
    sleep(1);
    int * volatile crashPtr = nullptr;
    *crashPtr = 0;
}

} // namespace silk
