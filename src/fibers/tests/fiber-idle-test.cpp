#include <silk/fibers/fiber.h>
#include <silk/util/assert.h>
#include <silk/util/perf.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>

#include <sched.h>
#include <unistd.h>

#include <sys/resource.h>

namespace silk
{

/**
 * Measures what a fully idle scheduler costs. The total park rate is the sharp,
 * machine-independent metric: every park pays the kernel's newidle balancing
 * once, and an idle scheduler keeps exactly one standby prober parking at the
 * maxWaitNs cadence regardless of the fleet width, so the process-wide rate is
 * bounded by that cadence on any topology and under any external load.
 */
class IdleTest : public ::testing::Test
{
protected:
    /** Length of the idle measurement window. */
    static constexpr uint64_t IDLE_WINDOW_US = 3'000'000;

    /** Upper bound on waiting for the prefix to decay to zero; generous for loaded machines. */
    static constexpr uint64_t DECAY_TIMEOUT_US = 20'000'000;

    /** Poll period while waiting for the decay. */
    static constexpr uint64_t DECAY_POLL_US = 10'000;

    /** Return the aggregate value of the named scheduler counter across all CPUs. */
    static uint64_t readSimpleCounter(const char * name) noexcept;

    /** Return the process CPU time (user plus system) in microseconds. */
    static uint64_t readProcessCpuTimeUs() noexcept;
};

uint64_t IdleTest::readSimpleCounter(const char * name) noexcept
{
    uint32_t count = Perf::getSimpleCounterCount();

    for (uint32_t index = 0; index < count; ++index)
    {
        const Perf::CounterInfo & info = Perf::getSimpleCounterInfo(index);

        if (info.name && !strcmp(info.name, name))
        {
            Perf::SimpleCounter total;
            uint32_t filled = Perf::getSimpleCounters(index, &total, 1);
            SILK_ASSERT(filled == 1, "could not read the simple counter: %s", name);
            return total.value.load(std::memory_order_relaxed);
        }
    }

    SILK_FAIL("could not find the simple counter: %s", name);
}

uint64_t IdleTest::readProcessCpuTimeUs() noexcept
{
    rusage usage;
    int r = ::getrusage(RUSAGE_SELF, &usage);
    SILK_ASSERT(!r);

    uint64_t userUs = static_cast<uint64_t>(usage.ru_utime.tv_sec) * 1'000'000 + usage.ru_utime.tv_usec;
    uint64_t systemUs = static_cast<uint64_t>(usage.ru_stime.tv_sec) * 1'000'000 + usage.ru_stime.tv_usec;
    return userUs + systemUs;
}

// An idle scheduler decays its prefix to one processor, and only that processor
// and the standby keep timed parks - the total park rate stays at two cadences
// and every other thread parks indefinitely. Parks only get rarer under external
// load (a preempted timed park oversleeps and an idle scheduler receives no
// doorbells), so the bound holds on a busy machine.
TEST_F(IdleTest, parkRateWhileIdle)
{
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    int r = ::sched_getaffinity(0, sizeof(affinity), &affinity);
    ASSERT_EQ(r, 0);

    int processorCount = CPU_COUNT(&affinity);
    ASSERT_GT(processorCount, 0);

    // The prefix width is the boot width plus grows minus shrinks, and the width
    // floors at one, so a fully decayed scheduler has shrinks leading grows by the
    // boot width minus one. Waiting for it proves the decay and keeps decay parks
    // out of the measurement window.
    uint64_t decayCount = processorCount - 1;
    uint64_t growCount = 0;
    uint64_t shrinkCount = 0;

    for (uint64_t waitedUs = 0; waitedUs < DECAY_TIMEOUT_US; waitedUs += DECAY_POLL_US)
    {
        shrinkCount = readSimpleCounter("SchedulerThreadShrink");
        growCount = readSimpleCounter("SchedulerThreadGrow");

        if (shrinkCount - growCount == decayCount)
        {
            break;
        }

        ::usleep(DECAY_POLL_US);
    }

    ASSERT_EQ(shrinkCount - growCount, decayCount);

    uint64_t parkedBefore = readSimpleCounter("SchedulerThreadParked");
    uint64_t wakedBefore = readSimpleCounter("SchedulerThreadWaked");
    uint64_t cpuTimeBefore = readProcessCpuTimeUs();

    ::usleep(IDLE_WINDOW_US);

    uint64_t parkedAfter = readSimpleCounter("SchedulerThreadParked");
    uint64_t wakedAfter = readSimpleCounter("SchedulerThreadWaked");
    uint64_t cpuTimeAfter = readProcessCpuTimeUs();

    FiberScheduler::Options options;
    double standbyParksPerSecond = 1e9 / options.maxWaitNs;

    double seconds = IDLE_WINDOW_US / 1e6;
    double parksPerSecond = (parkedAfter - parkedBefore) / seconds;
    double wakesPerSecond = (wakedAfter - wakedBefore) / seconds;
    double cpuMsPerSecond = (cpuTimeAfter - cpuTimeBefore) / 1000.0 / seconds;

    printf("idle parks/s: %.1f (standby cadence %.1f, %d threads)\n", parksPerSecond, standbyParksPerSecond, processorCount);
    printf("idle wakes/s: %.1f\n", wakesPerSecond);
    printf("idle process cpu ms/s: %.2f\n", cpuMsPerSecond);

    ASSERT_LE(parksPerSecond, 3.0 * standbyParksPerSecond);
}

} // namespace silk
