#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>
#include <silk/util/assert.h>
#include <silk/util/crash-dumper.h>
#include <silk/util/init.h>
#include <silk/util/platform.h>

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sched.h>
#include <spawn.h>
#include <unistd.h>

#include <fibers/cpu.h>
#include <sys/prctl.h>
#include <sys/wait.h>

namespace silk
{

/**
 * Stress test of the scheduler teardown order: destroy must join the worker threads before it destroys
 * the processors, because a worker's epilogue may still touch a fiber's home ring after the application
 * has joined every fiber. One process can initialize the scheduler once, so every cycle runs in a child
 * that this binary spawns from itself, and several children share two CPUs so the kernel preempts a
 * worker inside that window. A child dying on a signal is the failure.
 */
class DestroyTest : public ::testing::Test
{
protected:
    /** Children kept in flight at once - several per CPU, so a worker is preempted mid-epilogue. */
    static constexpr int CHILD_COUNT = 8;

    /** Child cycles per test; on the reference boxes the wrong teardown order fails within the first hundred. */
    static constexpr int CYCLE_COUNT = 1000;

    /** CPUs the children share. */
    static constexpr int SHARED_CPU_COUNT = 2;

    /** Fibers each child runs through thread mode. */
    static constexpr int FIBER_COUNT = 256;

    /** Fiber entry: enter and leave thread mode, then terminate. */
    static int threadModeFiberMain(int * params) noexcept;

    /** Restrict the calling thread to the first SHARED_CPU_COUNT CPUs of its affinity mask; spawned children inherit it. */
    static void restrictToCpus() noexcept;

    /** Start one child cycle of this binary and return its pid. */
    static pid_t spawnChild() noexcept;

    /** Wait for the child pid, or for any child when pid is -1; return the reaped pid and its raw wait status. */
    static pid_t waitChild(pid_t pid, int * status) noexcept;

    /** Return the index of pid in children; pid 0 finds a free slot. Fails when absent. */
    static int findSlot(const pid_t * children, pid_t pid) noexcept;

public:
    /** Run one initialize / thread-mode fibers / destroy cycle; the child mode body of main. parent is the spawning pid. */
    static int runChildCycle(pid_t parent) noexcept;

    /** First argument of child mode; the second is the spawning pid. */
    static inline char CHILD_FLAG[] = "--child";
};

int DestroyTest::threadModeFiberMain(int * params) noexcept
{
    SILK_UNUSED(params);

    FiberScheduler::ThreadModeScope scope;
    return 0;
}

int DestroyTest::runChildCycle(pid_t parent) noexcept
{
    // Die with the parent: a ctest timeout reaches only the parent, and this signal makes every live child
    // dump its own stack through the crash dumper instead of surviving as an orphan that holds the captured
    // output pipe open. It fires when the spawning thread dies - the parent's main thread, alive until exit -
    // and reports nothing about a death before it is armed, hence the parent check right after it.
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

    installCrashDumper();
    silk::initialize();
    FiberScheduler::initialize();

    FiberFuture futures[FIBER_COUNT];

    for (int i = 0; i < FIBER_COUNT; ++i)
    {
        r = FiberScheduler::run(threadModeFiberMain, 0, &futures[i]);
        SILK_ASSERT(!r, "could not spawn the thread-mode fiber: r=%d", r);
    }

    for (int i = 0; i < FIBER_COUNT; ++i)
    {
        r = futures[i].wait();
        SILK_ASSERT(!r, "the thread-mode fiber failed: r=%d", r);
    }

    FiberScheduler::destroy();
    silk::destroy();
    return 0;
}

void DestroyTest::restrictToCpus() noexcept
{
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    int r = ::sched_getaffinity(0, sizeof(cpu_set_t), &affinity);
    if (r)
    {
        r = errno;
        SILK_FAIL("could not read the process affinity mask: r=%d", r);
    }

    cpu_set_t cpuSet;
    CPU_ZERO(&cpuSet);
    int count = 0;

    for (int cpu = 0; cpu < CPU_SETSIZE && count < SHARED_CPU_COUNT; ++cpu)
    {
        if (CPU_ISSET(cpu, &affinity))
        {
            CPU_SET(cpu, &cpuSet);
            ++count;
        }
    }

    r = pinThreadToCpus(cpuSet);
    SILK_ASSERT(!r, "could not restrict the thread to %d cpus: r=%d", count, r);
}

pid_t DestroyTest::spawnChild() noexcept
{
    static char exePath[] = "/proc/self/exe";
    char parent[16];
    std::snprintf(parent, sizeof(parent), "%d", ::getpid());
    char * argv[] = {exePath, CHILD_FLAG, parent, nullptr};

    pid_t pid;
    int r = ::posix_spawn(&pid, exePath, nullptr, nullptr, argv, environ);
    SILK_ASSERT(!r, "could not spawn the child cycle: r=%d", r);
    return pid;
}

pid_t DestroyTest::waitChild(pid_t pid, int * status) noexcept
{
    pid_t reaped = ::waitpid(pid, status, 0);
    if (reaped < 0)
    {
        int r = errno;
        SILK_FAIL("could not wait for the child cycle: r=%d", r);
    }

    return reaped;
}

int DestroyTest::findSlot(const pid_t * children, pid_t pid) noexcept
{
    for (int slot = 0; slot < CHILD_COUNT; ++slot)
    {
        if (children[slot] == pid)
        {
            return slot;
        }
    }

    SILK_FAIL("pid %d is not a child cycle in flight", pid);
}

// Every child must exit cleanly: a child killed by a signal ran a worker into a destroyed processor.
TEST_F(DestroyTest, joinsWorkersBeforeProcessors)
{
    restrictToCpus();

    // Keep CHILD_COUNT children in flight: spawn one per step and, from step CHILD_COUNT on, reap any one per
    // step. The first bad status ends the loop and is the verdict; the drain loop reaps the rest first, so
    // the parent never exits with live children.
    pid_t children[CHILD_COUNT] = {};
    pid_t failedPid = 0;
    int failedStatus = 0;
    int failedStep = 0;

    for (int step = 0; step < CYCLE_COUNT + CHILD_COUNT; ++step)
    {
        if (step >= CHILD_COUNT)
        {
            int status;
            pid_t pid = waitChild(-1, &status);
            children[findSlot(children, pid)] = 0;

            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            {
                failedPid = pid;
                failedStatus = status;
                failedStep = step;
                break;
            }
        }

        if (step < CYCLE_COUNT)
        {
            children[findSlot(children, 0)] = spawnChild();
        }
    }

    for (int slot = 0; slot < CHILD_COUNT; ++slot)
    {
        if (children[slot])
        {
            int status;
            waitChild(children[slot], &status);
        }
    }

    ASSERT_TRUE(WIFEXITED(failedStatus)) << "child " << failedPid << " died on signal " << WTERMSIG(failedStatus) << " at step "
                                         << failedStep;
    ASSERT_EQ(WEXITSTATUS(failedStatus), 0) << "child " << failedPid << " at step " << failedStep;
}

} // namespace silk

int main(int argc, char ** argv)
{
    if (argc > 2 && std::strcmp(argv[1], silk::DestroyTest::CHILD_FLAG) == 0)
    {
        return silk::DestroyTest::runChildCycle(std::atoi(argv[2]));
    }

    ::testing::InitGoogleTest(&argc, argv);
    if (::testing::GTEST_FLAG(list_tests))
    {
        return RUN_ALL_TESTS();
    }

    silk::installCrashDumper();
    silk::initialize();

    int r = RUN_ALL_TESTS();

    silk::destroy();
    return r;
}
