#include <silk/util/crash-dumper.h>

#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>
#include <silk/util/assert.h>
#include <silk/util/init.h>
#include <silk/util/platform.h>

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>

#include <sys/prctl.h>
#include <sys/wait.h>

namespace silk
{

/**
 * End-to-end check of the crash dumper: each test spawns this binary in a child mode that parks fibers and then
 * hangs or crashes, reads the child's output until the child and its dumper close it, and checks the dump and how
 * the child ended. The child is a process of its own, so its crash cannot take the test binary down.
 */
class CrashDumperTest : public ::testing::Test
{
protected:
    /** Fibers a child parks before it hangs or crashes, so the dump's fiber list has fibers to report. */
    static constexpr int FIBER_COUNT = 4;

    /** Exit code of a child that the hang signal stopped. */
    static constexpr int HANG_EXIT_CODE = 124;

    /** Line a child prints once its fibers are parked; the hang test sends the hang signal after it. */
    static constexpr char READY_LINE[] = "fibers parked\n";

    /** Line the dumper prints above the silk fiber list, followed by the number of fibers it found. */
    static constexpr char FIBER_LIST_HEADER[] = "===== silk fibers: ";

    /** Capacity of the captured child output; the dump of every thread of a large box fits. */
    static constexpr size_t OUTPUT_CAPACITY = 4 << 20;

    /** Spawn this binary in the child mode; the child's stdout and stderr go to the pipe returned in *outputFd. */
    static pid_t spawnChild(char * mode, int * outputFd) noexcept;

    /**
     * Read the child's output into output until every writer closes it, echoing it to stderr for the test log;
     * with hang set, send the child the hang signal once its ready line arrives.
     */
    static void readOutput(int outputFd, pid_t pid, bool hang, char * output) noexcept;

    /** Wait for the child and return its raw wait status. */
    static int waitChild(pid_t pid) noexcept;

    /** Return the fiber count of the dump's fiber list, or -1 when the output holds no fiber list. */
    static int getDumpedFiberCount(const char * output) noexcept;

    /** Captured output of the child under test. */
    static inline char childOutput[OUTPUT_CAPACITY];

public:
    /** Run the child mode body of main: park FIBER_COUNT fibers, print the ready line, then hang or crash. */
    static int runChild(const char * mode, pid_t parent) noexcept;

    /** First argument of child mode; the second is HANG_MODE or CRASH_MODE, the third the spawning pid. */
    static inline char CHILD_FLAG[] = "--child";

    /** Child mode that blocks until the hang signal. */
    static inline char HANG_MODE[] = "hang";

    /** Child mode that raises SIGSEGV. */
    static inline char CRASH_MODE[] = "crash";
};

int CrashDumperTest::runChild(const char * mode, pid_t parent) noexcept
{
    // Die with the parent: a ctest timeout reaches only the parent, and this signal makes the child dump its own
    // stack instead of surviving as an orphan that holds the output pipe open. It reports nothing about a death
    // before it is armed, hence the parent check right after it.
    int r = ::prctl(PR_SET_PDEATHSIG, SIGQUIT, 0, 0, 0);
    if (r)
    {
        r = errno;
        SILK_FAIL("could not arm the parent death signal: r=%d", r);
    }

    if (::getppid() != parent)
    {
        return 0;
    }

    installCrashDumper(SIGQUIT, HANG_EXIT_CODE);
    silk::initialize();
    FiberScheduler::initialize();

    struct ParkFiber
    {
        // Reports itself parked, then waits on a future nobody sets, so the dump's fiber list has suspended fibers.
        static int fiberMain(ParkFiber * params) noexcept
        {
            params->ready->set(0);
            params->block->wait();
            return 0;
        }

        FiberFuture * ready;
        FiberFuture * block;
    };

    FiberFuture ready[FIBER_COUNT];
    FiberFuture block[FIBER_COUNT];
    FiberFuture join[FIBER_COUNT];

    for (int i = 0; i < FIBER_COUNT; ++i)
    {
        r = FiberScheduler::run(ParkFiber::fiberMain, {&ready[i], &block[i]}, &join[i]);
        SILK_ASSERT(!r, "could not spawn the parked fiber: r=%d", r);
    }

    for (int i = 0; i < FIBER_COUNT; ++i)
    {
        r = ready[i].wait();
        SILK_ASSERT(!r, "the parked fiber failed: r=%d", r);
    }

    ssize_t readyLength = sizeof(READY_LINE) - 1;
    ssize_t written = ::write(STDOUT_FILENO, READY_LINE, readyLength);
    SILK_ASSERT(written == readyLength, "could not print the ready line");

    // A raise, not a null store: UBSan stops a null store itself and exits before any signal.
    if (std::strcmp(mode, CRASH_MODE) == 0)
    {
        ::raise(SIGSEGV);
    }

    for (;;)
    {
        ::pause();
    }
}

pid_t CrashDumperTest::spawnChild(char * mode, int * outputFd) noexcept
{
    int pipeFds[2];
    int r = ::pipe2(pipeFds, O_CLOEXEC);
    if (r)
    {
        r = errno;
        SILK_FAIL("could not create the output pipe: r=%d", r);
    }

    // dup2 clears close-on-exec on the child's stdout and stderr; the pipe's own descriptors close at exec.
    posix_spawn_file_actions_t actions;
    r = ::posix_spawn_file_actions_init(&actions);
    SILK_ASSERT(!r, "could not initialize the spawn file actions: r=%d", r);
    r = ::posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
    SILK_ASSERT(!r, "could not redirect the child stdout: r=%d", r);
    r = ::posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDERR_FILENO);
    SILK_ASSERT(!r, "could not redirect the child stderr: r=%d", r);

    static char exePath[] = "/proc/self/exe";
    char parent[16];
    std::snprintf(parent, sizeof(parent), "%d", ::getpid());
    char * argv[] = {exePath, CHILD_FLAG, mode, parent, nullptr};

    pid_t pid;
    r = ::posix_spawn(&pid, exePath, &actions, nullptr, argv, environ);
    SILK_ASSERT(!r, "could not spawn the child: r=%d", r);

    ::posix_spawn_file_actions_destroy(&actions);

    // The parent keeps only the read end, so the pipe ends once the child and its dumper exit.
    ::close(pipeFds[1]);
    *outputFd = pipeFds[0];
    return pid;
}

void CrashDumperTest::readOutput(int outputFd, pid_t pid, bool hang, char * output) noexcept
{
    size_t length = 0;
    bool signalled = false;

    for (;;)
    {
        SILK_ASSERT(length < OUTPUT_CAPACITY - 1, "the child output does not fit in %zu bytes", OUTPUT_CAPACITY);
        ssize_t count = ::read(outputFd, output + length, OUTPUT_CAPACITY - 1 - length);
        if (count < 0)
        {
            int r = errno;
            SILK_ASSERT(r == EINTR, "could not read the child output: r=%d", r);
            continue;
        }

        if (!count)
        {
            return;
        }

        ssize_t echoed = ::write(STDERR_FILENO, output + length, count);
        SILK_UNUSED(echoed);
        length += count;
        output[length] = '\0';

        if (hang && !signalled && std::strstr(output, READY_LINE))
        {
            int r = ::kill(pid, SIGQUIT);
            if (r)
            {
                r = errno;
                SILK_FAIL("could not send the hang signal: r=%d", r);
            }

            signalled = true;
        }
    }
}

int CrashDumperTest::waitChild(pid_t pid) noexcept
{
    int status;
    pid_t reaped = ::waitpid(pid, &status, 0);
    if (reaped < 0)
    {
        int r = errno;
        SILK_FAIL("could not wait for the child: r=%d", r);
    }

    return status;
}

int CrashDumperTest::getDumpedFiberCount(const char * output) noexcept
{
    const char * header = std::strstr(output, FIBER_LIST_HEADER);
    if (!header)
    {
        return -1;
    }

    return std::atoi(header + sizeof(FIBER_LIST_HEADER) - 1);
}

// The hang signal makes the dumper print every thread and the fiber list, then the child exits with the hang code.
TEST_F(CrashDumperTest, HangDumpsThreadsAndFibers)
{
    int outputFd;
    pid_t pid = spawnChild(HANG_MODE, &outputFd);
    readOutput(outputFd, pid, true, childOutput);
    ::close(outputFd);
    int status = waitChild(pid);

    ASSERT_TRUE(WIFEXITED(status)) << "the child died on signal " << WTERMSIG(status);
    ASSERT_EQ(WEXITSTATUS(status), HANG_EXIT_CODE);
    ASSERT_GE(getDumpedFiberCount(childOutput), FIBER_COUNT);
}

// A segfault makes the dumper print every thread and the fiber list, then the child dies of the original signal.
TEST_F(CrashDumperTest, SegfaultDumpsThreadsAndFibers)
{
    int outputFd;
    pid_t pid = spawnChild(CRASH_MODE, &outputFd);
    readOutput(outputFd, pid, false, childOutput);
    ::close(outputFd);
    int status = waitChild(pid);

    ASSERT_TRUE(WIFSIGNALED(status)) << "the child exited with " << WEXITSTATUS(status);
    ASSERT_EQ(WTERMSIG(status), SIGSEGV);
    ASSERT_GE(getDumpedFiberCount(childOutput), FIBER_COUNT);
}

} // namespace silk

int main(int argc, char ** argv)
{
    if (argc > 3 && std::strcmp(argv[1], silk::CrashDumperTest::CHILD_FLAG) == 0)
    {
        return silk::CrashDumperTest::runChild(argv[2], std::atoi(argv[3]));
    }

    ::testing::InitGoogleTest(&argc, argv);
    if (::testing::GTEST_FLAG(list_tests))
    {
        return RUN_ALL_TESTS();
    }

    silk::installCrashDumper();
    return RUN_ALL_TESTS();
}
