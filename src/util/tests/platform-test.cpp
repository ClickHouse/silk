#include <silk/util/platform.h>

#include <gtest/gtest.h>

#include <sched.h>

namespace silk
{

// Pin the thread to the highest allowed CPU before the first getProcessorCount
// call: a count derived from the affinity mask (as musl's sysconf is) would
// report one processor and leave raw CPU ids out of bounds.
TEST(Platform, ProcessorCountCoversPossibleCpuIds)
{
    cpu_set_t savedSet;
    CPU_ZERO(&savedSet);
    int r = sched_getaffinity(0, sizeof(savedSet), &savedSet);
    ASSERT_EQ(r, 0);

    int highestCpu = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    {
        if (CPU_ISSET(cpu, &savedSet))
        {
            highestCpu = cpu;
        }
    }
    ASSERT_GE(highestCpu, 0);

    cpu_set_t pinnedSet;
    CPU_ZERO(&pinnedSet);
    CPU_SET(highestCpu, &pinnedSet);
    r = sched_setaffinity(0, sizeof(pinnedSet), &pinnedSet);
    ASSERT_EQ(r, 0);

    uint16_t count = getProcessorCount();

    r = sched_setaffinity(0, sizeof(savedSet), &savedSet);
    ASSERT_EQ(r, 0);

    ASSERT_GT(count, highestCpu);
}

} // namespace silk
