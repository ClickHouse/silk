#include <silk/util/assert.h>

#include <gtest/gtest.h>

#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include <signal.h>
#include <unistd.h>

#include <sys/wait.h>

namespace silk
{

/** Wait at start until every crashing thread is there, then raise SIGABRT, so the threads crash at the same time. */
static void crashAfterBarrier(std::barrier<> * start) noexcept
{
    start->arrive_and_wait();
    ::raise(SIGABRT);
}

// Manual check that a crash on two threads at once still gives a complete dump: the first thread owns the dump
// and the second must not end the process before the dump is done. The test runs this binary as a child in which
// two threads raise SIGABRT, and checks that the child's output reaches the last line of the dump. Exercise it with:
//   CRASH_DUMPER_TWO_THREADS=1 ./bb test -R CrashDumperTest.TwoThreadCrashKeepsTheDump
TEST(CrashDumperTest, TwoThreadCrashKeepsTheDump)
{
    if (std::getenv("CRASH_DUMPER_TWO_THREADS_CHILD"))
    {
        std::barrier start(2);
        std::thread(crashAfterBarrier, &start).detach();
        crashAfterBarrier(&start);
        SILK_FAIL("the crash did not end the process");
    }

    const char * twoThreadsRequested = std::getenv("CRASH_DUMPER_TWO_THREADS");
    if (!twoThreadsRequested)
    {
        GTEST_SKIP() << "set CRASH_DUMPER_TWO_THREADS to exercise the crash-dumper's two-thread crash path";
    }

    char exePath[4096];
    ssize_t length = ::readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    ASSERT_GT(length, 0);
    exePath[length] = '\0';

    char command[4200];
    std::snprintf(
        command,
        sizeof(command),
        "CRASH_DUMPER_TWO_THREADS_CHILD=1 exec '%s' --gtest_filter=CrashDumperTest.TwoThreadCrashKeepsTheDump 2>&1",
        exePath);
    FILE * child = ::popen(command, "r");
    ASSERT_TRUE(child);

    std::string output;
    char buffer[4096];

    for (;;)
    {
        size_t got = std::fread(buffer, 1, sizeof(buffer), child);
        if (!got)
        {
            break;
        }

        output.append(buffer, got);
    }

    int status = ::pclose(child);

    ASSERT_TRUE(WIFSIGNALED(status)) << output;
    ASSERT_EQ(WTERMSIG(status), SIGABRT) << output;
    ASSERT_NE(output.find("crash-dumper: no silk frame found; skipping state dumps"), std::string::npos)
        << "the process ended before the dump was complete:\n"
        << output;
}

} // namespace silk
