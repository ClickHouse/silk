/**
 * GDB integration test driver for fiber.py.
 *
 * Creates fibers in known states, then calls sleep() so GDB can break in and
 * inspect them with fiber-list, fiber-verify-layout, and the context commands.
 *
 * silk::Fiber states at the sleep() call:
 *   spinner       RUNNING    cpu{N}/current  (busy-loops on g_stop)
 *   holder        SUSPENDED  waiterTable     (waiting on g_release silk::FiberFuture)
 *   waiter[0..2]  SUSPENDED  waiterTable     (blocked on g_mutex held by holder)
 *   sleeper[0..2] SUSPENDED  sleepTree       (parked in FiberScheduler::sleep)
 *
 * Interactive use:
 *   gdb ./build/debug/bin/gdb-test
 *   (gdb) source gdb/fiber.py
 *   (gdb) b gdb_ready
 *   (gdb) run
 *   -- stopped inside sleep() --
 *   (gdb) fiber-list --waiters
 *   (gdb) fiber-verify-layout 0x<waiter ptr>
 *   (gdb) fiber-savecontext
 *   (gdb) fiber-switchcontext 0x<waiter ptr>
 *   (gdb) bt
 *   (gdb) fiber-restorecontext
 *   (gdb) continue
 *
 * Automated use (via CTest, run under taskset to limit scheduler threads):
 *   taskset -c 0-3 gdb --batch -q \
 *       -ex "source gdb/fiber.py" -ex "source gdb/autotest.py" \
 *       ./build/debug/bin/gdb-test
 */

#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>
#include <silk/fibers/mutex.h>
#include <silk/util/assert.h>
#include <silk/util/init.h>
#include <silk/util/perf.h>
#include <silk/util/platform.h>

#include <atomic>
#include <cstdio>

#include <semaphore.h>
#include <unistd.h>

static silk::FiberMutex g_mutex;
static silk::FiberFuture g_release;
static std::atomic<bool> g_stop{false};

// Acquires g_mutex, signals main via semaphore, then suspends on g_release.
// At the sleep() call: SUSPENDED in waiterTable (bucket keyed by &g_release).
struct HolderParams
{
    sem_t * ready;

    static int fiberMain(HolderParams * p) noexcept
    {
        g_mutex.lock();
        sem_post(p->ready); // main may now start waiters
        g_release.wait(); // SUSPENDED in waiterTable
        g_mutex.unlock();
        return 0;
    }
};

// Blocks on g_mutex (held by holder).
// At the sleep() call: SUSPENDED in waiterTable (bucket keyed by &g_mutex).
struct WaiterParams
{
    static int fiberMain(WaiterParams * p) noexcept
    {
        SILK_UNUSED(p);
        g_mutex.lock();
        g_mutex.unlock();
        return 0;
    }
};

// Busy-loops until g_stop.
// At the sleep() call: RUNNING on a CPU (visible as cpu{N}/current).
struct SpinnerParams
{
    static int fiberMain(SpinnerParams * p) noexcept
    {
        SILK_UNUSED(p);
        for (;;)
        {
            if (g_stop.load(std::memory_order_relaxed))
            {
                break;
            }
            silk::cpuPause();
        }
        return 0;
    }
};

// Parks in FiberScheduler::sleep for an hour; main cancels it at cleanup.
// SUSPENDED with its SleepFuture IN_TABLE in a per-CPU sleepTree.
struct SleeperParams
{
    silk::FiberScheduler::SleepFuture * sleepFuture;
    sem_t * ready;

    static int fiberMain(SleeperParams * p) noexcept
    {
        silk::FiberScheduler::sleep(3'600ULL * 1'000'000'000ULL, p->sleepFuture);
        sem_post(p->ready); // main may now expect this sleeper in a sleepTree
        p->sleepFuture->wait();
        return 0;
    }
};

static constexpr int N_WAITERS = 3;
static constexpr int N_SLEEPERS = 3;

// Named breakpoint target: set "b gdb_ready" in GDB.
// noinline + asm barrier prevent the compiler from optimizing the call away.
__attribute__((noinline, used)) static void gdb_ready()
{
    asm volatile("" ::: "memory");
}

int main()
{
    silk::initialize();
    silk::FiberScheduler::initialize();

    // Spinner stays RUNNING on a CPU.
    silk::FiberFuture spinner;
    int r = silk::FiberScheduler::run(SpinnerParams::fiberMain, SpinnerParams{}, &spinner);
    SILK_ASSERT(!r);

    // Holder acquires the mutex and suspends on g_release.
    sem_t ready;
    sem_init(&ready, 0, 0);
    silk::FiberFuture holder;
    r = silk::FiberScheduler::run(HolderParams::fiberMain, HolderParams{&ready}, &holder);
    SILK_ASSERT(!r);
    sem_wait(&ready); // wait until holder holds the mutex

    // Waiters block on the held mutex -> SUSPENDED in waiterTable.
    silk::FiberFuture waiters[N_WAITERS];
    for (int i = 0; i < N_WAITERS; ++i)
    {
        r = silk::FiberScheduler::run(WaiterParams::fiberMain, WaiterParams{}, &waiters[i]);
        SILK_ASSERT(!r);
    }

    // Sleepers park in FiberScheduler::sleep -> SUSPENDED, SleepFuture IN_TABLE
    // in a per-CPU sleepTree.  main cancels them at cleanup.
    silk::FiberScheduler::SleepFuture sleepFutures[N_SLEEPERS];
    silk::FiberFuture sleepers[N_SLEEPERS];
    sem_t sleepReady;
    sem_init(&sleepReady, 0, 0);
    for (int i = 0; i < N_SLEEPERS; ++i)
    {
        r = silk::FiberScheduler::run(SleeperParams::fiberMain, SleeperParams{&sleepFutures[i], &sleepReady}, &sleepers[i]);
        SILK_ASSERT(!r);
    }

    for (int i = 0; i < N_SLEEPERS; ++i)
    {
        sem_wait(&sleepReady); // wait until each sleeper has armed its sleep
    }

    // Give waiters time to enter the silk::FiberMutex slow path and suspend, and the
    // service loop time to move the armed sleeps from their sleepQueue into the sleepTree.
    usleep(100'000);

    std::puts("gdb-test: ready -- set breakpoint on gdb_ready() and inspect fibers");

    // GDB breakpoint: b gdb_ready
    // Automated test (autotest.py) breaks here, runs commands, then quits.
    // Interactive use: 'continue' resumes into sleep(), giving time before cleanup.
    gdb_ready();
    sleep(1);

    // Cleanup.
    g_stop.store(true, std::memory_order_relaxed);
    g_release.set(0); // wake holder so it releases the mutex
    for (int i = 0; i < N_SLEEPERS; ++i)
    {
        sleepFutures[i].cancel(); // wake sleepers parked in their sleepTrees
    }

    for (int i = 0; i < N_WAITERS; ++i)
    {
        waiters[i].wait();
    }
    holder.wait();
    spinner.wait();
    for (int i = 0; i < N_SLEEPERS; ++i)
    {
        sleepers[i].wait();
    }

    sem_destroy(&ready);
    sem_destroy(&sleepReady);

    silk::FiberScheduler::destroy();
    silk::destroy();

    return 0;
}
