#include <silk/util/perf.h>

#include <silk/util/platform.h>

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>

// clang-format off
#define TEST_COUNTERS(x) \
    x(COUNTER_A, "CounterA") \
    x(COUNTER_B, "CounterB") \
    x(COUNTER_C, "CounterC")

#define TEST_MEM_COUNTERS(x) \
    x(MEM_HEAP,  "Heap") \
    x(MEM_STACK, "Stack")
// clang-format on

namespace silk
{

DECLARE_SIMPLE_COUNTERS(TEST_COUNTERS);
DECLARE_MEM_COUNTERS(TEST_MEM_COUNTERS);

class PerfTest : public ::testing::Test
{
protected:
    void SetUp() override { Perf::initialize(); }
    void TearDown() override { Perf::destroy(); }
};

TEST_F(PerfTest, RegisterSetsGroupFields)
{
    const char * names[] = {"A", "B"};
    Perf::CounterGroup group;
    int r = Perf::registerSimpleCounters(&group, names, 2);
    ASSERT_EQ(r, 0);
    EXPECT_EQ(group.startIndex, 0u);
    EXPECT_EQ(group.count, 2u);
}

TEST_F(PerfTest, RegisterSetsNames)
{
    const char * names[] = {"Alpha", "Beta"};
    Perf::CounterGroup group;
    ASSERT_EQ(Perf::registerSimpleCounters(&group, names, 2), 0);
    EXPECT_STREQ(Perf::getSimpleCounterInfo(0).name, "Alpha");
    EXPECT_STREQ(Perf::getSimpleCounterInfo(1).name, "Beta");
}

TEST_F(PerfTest, RegisterUpdatesCount)
{
    const char * names[] = {"X", "Y", "Z"};
    Perf::CounterGroup group;
    ASSERT_EQ(Perf::registerSimpleCounters(&group, names, 3), 0);
    EXPECT_EQ(Perf::getSimpleCounterCount(), 3u);
}

TEST_F(PerfTest, MultipleGroupsGetNonOverlappingSlots)
{
    const char * names1[] = {"A", "B"};
    const char * names2[] = {"C", "D", "E"};
    Perf::CounterGroup group1, group2;
    ASSERT_EQ(Perf::registerSimpleCounters(&group1, names1, 2), 0);
    ASSERT_EQ(Perf::registerSimpleCounters(&group2, names2, 3), 0);

    EXPECT_EQ(group1.startIndex, 0u);
    EXPECT_EQ(group1.count, 2u);
    EXPECT_EQ(group2.startIndex, 2u);
    EXPECT_EQ(group2.count, 3u);
    EXPECT_EQ(Perf::getSimpleCounterCount(), 5u);
}

TEST_F(PerfTest, CounterGroupIndexReturnsGlobalSlot)
{
    const char * names[] = {"X"};
    Perf::CounterGroup group1, group2;
    ASSERT_EQ(Perf::registerSimpleCounters(&group1, names, 1), 0);
    ASSERT_EQ(Perf::registerSimpleCounters(&group2, names, 1), 0);
    EXPECT_EQ(group1[0], 0u);
    EXPECT_EQ(group2[0], 1u);
}

TEST_F(PerfTest, IncrementAccumulatesOnSingleCpu)
{
    const char * names[] = {"Counter"};
    Perf::CounterGroup group;
    ASSERT_EQ(Perf::registerSimpleCounters(&group, names, 1), 0);

    Perf::getSimpleCounter(group[0], 0).increment(5);
    Perf::getSimpleCounter(group[0], 0).increment(3);

    Perf::SimpleCounter out[1];
    ASSERT_EQ(Perf::getSimpleCounters(0, out, 1), 1u);
    EXPECT_EQ(out[0].value.load(std::memory_order_relaxed), 8u);
}

TEST_F(PerfTest, GetSimpleCountersSumsAcrossCpus)
{
    if (getProcessorCount() < 2)
    {
        GTEST_SKIP() << "need at least 2 CPUs";
    }

    const char * names[] = {"Counter"};
    Perf::CounterGroup group;
    ASSERT_EQ(Perf::registerSimpleCounters(&group, names, 1), 0);

    Perf::getSimpleCounter(group[0], 0).increment(10);
    Perf::getSimpleCounter(group[0], 1).increment(7);

    Perf::SimpleCounter out[1];
    ASSERT_EQ(Perf::getSimpleCounters(0, out, 1), 1u);
    EXPECT_EQ(out[0].value.load(std::memory_order_relaxed), 17u);
}

TEST_F(PerfTest, GetSimpleCountersResetsOutput)
{
    const char * names[] = {"A", "B"};
    Perf::CounterGroup group;
    ASSERT_EQ(Perf::registerSimpleCounters(&group, names, 2), 0);

    Perf::getSimpleCounter(group[0], 0).increment(4);

    Perf::SimpleCounter out[2];
    out[1].value.store(99, std::memory_order_relaxed);
    ASSERT_EQ(Perf::getSimpleCounters(0, out, 2), 2u);
    EXPECT_EQ(out[0].value.load(std::memory_order_relaxed), 4u);
    EXPECT_EQ(out[1].value.load(std::memory_order_relaxed), 0u);
}

TEST_F(PerfTest, RegisterSimpleCounterSetsGroupFieldsAndName)
{
    Perf::CounterGroup group;
    ASSERT_EQ(Perf::registerSimpleCounter(&group, "Requests"), 0);
    EXPECT_EQ(group.startIndex, 0u);
    EXPECT_EQ(group.count, 1u);
    EXPECT_STREQ(Perf::getSimpleCounterInfo(group[0]).name, "Requests");
}

TEST_F(PerfTest, OverflowReturnsENOMEM)
{
    for (uint32_t i = 0; i < Perf::NUM_SIMPLE_COUNTERS; ++i)
    {
        Perf::CounterGroup group;
        ASSERT_EQ(Perf::registerSimpleCounter(&group, "x"), 0);
    }

    Perf::CounterGroup overflow;
    EXPECT_EQ(Perf::registerSimpleCounter(&overflow, "overflow"), ENOMEM);
}

TEST_F(PerfTest, DestroyResetsCounterCount)
{
    const char * names[] = {"A"};
    Perf::CounterGroup group;
    ASSERT_EQ(Perf::registerSimpleCounters(&group, names, 1), 0);
    EXPECT_EQ(Perf::getSimpleCounterCount(), 1u);

    Perf::destroy();
    Perf::initialize();

    EXPECT_EQ(Perf::getSimpleCounterCount(), 0u);
}

TEST_F(PerfTest, MacroDeclaresIds)
{
    EXPECT_EQ(COUNTER_A, 0);
    EXPECT_EQ(COUNTER_B, 1);
    EXPECT_EQ(COUNTER_C, 2);
}

TEST_F(PerfTest, MacroRegistersGroupWithNames)
{
    Perf::CounterGroup group;
    REGISTER_SIMPLE_COUNTERS(&group, TEST_COUNTERS);
    EXPECT_EQ(group.count, 3u);
    EXPECT_STREQ(Perf::getSimpleCounterInfo(group[COUNTER_A]).name, "CounterA");
    EXPECT_STREQ(Perf::getSimpleCounterInfo(group[COUNTER_B]).name, "CounterB");
    EXPECT_STREQ(Perf::getSimpleCounterInfo(group[COUNTER_C]).name, "CounterC");
}

TEST_F(PerfTest, MemMacroDeclaresIds)
{
    EXPECT_EQ(MEM_HEAP, 0);
    EXPECT_EQ(MEM_STACK, 1);
}

TEST_F(PerfTest, MemMacroRegistersGroupWithNames)
{
    Perf::CounterGroup group;
    REGISTER_MEM_COUNTERS(&group, TEST_MEM_COUNTERS);
    EXPECT_EQ(group.count, 2u);
    EXPECT_STREQ(Perf::getMemCounterInfo(group[MEM_HEAP]).name, "Heap");
    EXPECT_STREQ(Perf::getMemCounterInfo(group[MEM_STACK]).name, "Stack");
}

TEST_F(PerfTest, RegisterMemCounterSetsGroupFieldsAndName)
{
    Perf::CounterGroup group;
    ASSERT_EQ(Perf::registerMemCounter(&group, "Heap"), 0);
    EXPECT_EQ(group.startIndex, 0u);
    EXPECT_EQ(group.count, 1u);
    EXPECT_STREQ(Perf::getMemCounterInfo(group[0]).name, "Heap");
}

TEST_F(PerfTest, RegisterMemCountersSetsGroupFields)
{
    const char * names[] = {"A", "B"};
    Perf::CounterGroup group;
    ASSERT_EQ(Perf::registerMemCounters(&group, names, 2), 0);
    EXPECT_EQ(group.startIndex, 0u);
    EXPECT_EQ(group.count, 2u);
}

TEST_F(PerfTest, RegisterMemCountersSetsNames)
{
    const char * names[] = {"Allocs", "Frees"};
    Perf::CounterGroup group;
    ASSERT_EQ(Perf::registerMemCounters(&group, names, 2), 0);
    EXPECT_STREQ(Perf::getMemCounterInfo(0).name, "Allocs");
    EXPECT_STREQ(Perf::getMemCounterInfo(1).name, "Frees");
}

TEST_F(PerfTest, RegisterMemCountersUpdatesCount)
{
    const char * names[] = {"X", "Y", "Z"};
    Perf::CounterGroup group;
    ASSERT_EQ(Perf::registerMemCounters(&group, names, 3), 0);
    EXPECT_EQ(Perf::getMemCounterCount(), 3u);
}

TEST_F(PerfTest, MultipleMemCounterGroupsGetNonOverlappingSlots)
{
    const char * names1[] = {"A", "B"};
    const char * names2[] = {"C", "D", "E"};
    Perf::CounterGroup group1, group2;
    ASSERT_EQ(Perf::registerMemCounters(&group1, names1, 2), 0);
    ASSERT_EQ(Perf::registerMemCounters(&group2, names2, 3), 0);

    EXPECT_EQ(group1.startIndex, 0u);
    EXPECT_EQ(group1.count, 2u);
    EXPECT_EQ(group2.startIndex, 2u);
    EXPECT_EQ(group2.count, 3u);
    EXPECT_EQ(Perf::getMemCounterCount(), 5u);
}

TEST_F(PerfTest, MemCounterGroupIndexReturnsGlobalSlot)
{
    const char * names[] = {"X"};
    Perf::CounterGroup group1, group2;
    ASSERT_EQ(Perf::registerMemCounters(&group1, names, 1), 0);
    ASSERT_EQ(Perf::registerMemCounters(&group2, names, 1), 0);
    EXPECT_EQ(group1[0], 0u);
    EXPECT_EQ(group2[0], 1u);
}

TEST_F(PerfTest, AllocRecordsCountAndSize)
{
    const char * names[] = {"Counter"};
    Perf::CounterGroup group;
    ASSERT_EQ(Perf::registerMemCounters(&group, names, 1), 0);

    Perf::getMemCounter(group[0], 0).alloc(64);
    Perf::getMemCounter(group[0], 0).alloc(128);

    Perf::MemCounter out[1];
    ASSERT_EQ(Perf::getMemCounters(0, out, 1), 1u);
    EXPECT_EQ(out[0].allocCount.load(std::memory_order_relaxed), 2u);
    EXPECT_EQ(out[0].allocSize.load(std::memory_order_relaxed), 192u);
    EXPECT_EQ(out[0].deallocCount.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(out[0].deallocSize.load(std::memory_order_relaxed), 0u);
}

TEST_F(PerfTest, DeallocRecordsCountAndSize)
{
    const char * names[] = {"Counter"};
    Perf::CounterGroup group;
    ASSERT_EQ(Perf::registerMemCounters(&group, names, 1), 0);

    Perf::getMemCounter(group[0], 0).dealloc(256);
    Perf::getMemCounter(group[0], 0).dealloc(32);

    Perf::MemCounter out[1];
    ASSERT_EQ(Perf::getMemCounters(0, out, 1), 1u);
    EXPECT_EQ(out[0].allocCount.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(out[0].allocSize.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(out[0].deallocCount.load(std::memory_order_relaxed), 2u);
    EXPECT_EQ(out[0].deallocSize.load(std::memory_order_relaxed), 288u);
}

TEST_F(PerfTest, GetMemCountersSumsAcrossCpus)
{
    if (getProcessorCount() < 2)
    {
        GTEST_SKIP() << "need at least 2 CPUs";
    }

    const char * names[] = {"Counter"};
    Perf::CounterGroup group;
    ASSERT_EQ(Perf::registerMemCounters(&group, names, 1), 0);

    Perf::getMemCounter(group[0], 0).alloc(100);
    Perf::getMemCounter(group[0], 1).alloc(200);
    Perf::getMemCounter(group[0], 1).dealloc(50);

    Perf::MemCounter out[1];
    ASSERT_EQ(Perf::getMemCounters(0, out, 1), 1u);
    EXPECT_EQ(out[0].allocCount.load(std::memory_order_relaxed), 2u);
    EXPECT_EQ(out[0].allocSize.load(std::memory_order_relaxed), 300u);
    EXPECT_EQ(out[0].deallocCount.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(out[0].deallocSize.load(std::memory_order_relaxed), 50u);
}

TEST_F(PerfTest, GetMemCountersResetsOutput)
{
    const char * names[] = {"A", "B"};
    Perf::CounterGroup group;
    ASSERT_EQ(Perf::registerMemCounters(&group, names, 2), 0);

    Perf::getMemCounter(group[0], 0).alloc(10);

    Perf::MemCounter out[2];
    out[1].allocCount.store(99, std::memory_order_relaxed);
    out[1].allocSize.store(99, std::memory_order_relaxed);
    ASSERT_EQ(Perf::getMemCounters(0, out, 2), 2u);
    EXPECT_EQ(out[0].allocCount.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(out[1].allocCount.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(out[1].allocSize.load(std::memory_order_relaxed), 0u);
}

TEST_F(PerfTest, GetMemCountersWithIndex)
{
    const char * names1[] = {"Group1"};
    const char * names2[] = {"Group2"};
    Perf::CounterGroup group1, group2;
    ASSERT_EQ(Perf::registerMemCounters(&group1, names1, 1), 0);
    ASSERT_EQ(Perf::registerMemCounters(&group2, names2, 1), 0);

    Perf::getMemCounter(group1[0], 0).alloc(10);
    Perf::getMemCounter(group2[0], 0).alloc(20);

    Perf::MemCounter out[1];
    ASSERT_EQ(Perf::getMemCounters(group2.startIndex, out, 1), 1u);
    EXPECT_EQ(out[0].allocSize.load(std::memory_order_relaxed), 20u);
}

TEST_F(PerfTest, MemCounterOverflowReturnsENOMEM)
{
    for (uint32_t i = 0; i < Perf::NUM_MEM_COUNTERS; ++i)
    {
        Perf::CounterGroup group;
        ASSERT_EQ(Perf::registerMemCounter(&group, "x"), 0);
    }

    Perf::CounterGroup overflow;
    EXPECT_EQ(Perf::registerMemCounter(&overflow, "overflow"), ENOMEM);
}

TEST_F(PerfTest, DestroyResetsMemCounterCount)
{
    const char * names[] = {"A"};
    Perf::CounterGroup group;
    ASSERT_EQ(Perf::registerMemCounters(&group, names, 1), 0);
    EXPECT_EQ(Perf::getMemCounterCount(), 1u);

    Perf::destroy();
    Perf::initialize();

    EXPECT_EQ(Perf::getMemCounterCount(), 0u);
}

} // namespace silk
