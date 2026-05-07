#include <fibers/cpu.h>

#include <silk/util/platform.h>
#include <silk/util/tsc.h>

#include <gtest/gtest.h>

#include <memory>

// topologyCostCycles: unknown topology returns cross-NUMA cost.

namespace silk
{

TEST(CpuTopology, unknownBothReturnsMaxCost)
{
    CpuTopology a, b; // packageId = UINT32_MAX by default
    uint64_t cost = topologyCostCycles(a, b);
    EXPECT_EQ(cost, Tsc::nanosecondsToCycles(500'000));
}

TEST(CpuTopology, unknownFirstReturnsMaxCost)
{
    CpuTopology unknown;
    CpuTopology known{0, 0, 0};
    EXPECT_EQ(topologyCostCycles(unknown, known), Tsc::nanosecondsToCycles(500'000));
}

TEST(CpuTopology, unknownSecondReturnsMaxCost)
{
    CpuTopology known{0, 0, 0};
    CpuTopology unknown;
    EXPECT_EQ(topologyCostCycles(known, unknown), Tsc::nanosecondsToCycles(500'000));
}

// HT sibling: same package, same core, any NUMA node.
TEST(CpuTopology, htSiblingCost)
{
    CpuTopology a{0, 0, 0};
    CpuTopology b{0, 0, 0};
    EXPECT_EQ(topologyCostCycles(a, b), Tsc::nanosecondsToCycles(1'000));
}

// Same NUMA node, different core.
TEST(CpuTopology, sameNumaCost)
{
    CpuTopology a{0, 0, 0};
    CpuTopology b{0, 1, 0}; // same package, different core, same NUMA
    EXPECT_EQ(topologyCostCycles(a, b), Tsc::nanosecondsToCycles(50'000));
}

// Different package, same NUMA node.
TEST(CpuTopology, sameNumaDifferentPackageCost)
{
    CpuTopology a{0, 0, 0};
    CpuTopology b{1, 0, 0}; // different package, same NUMA node
    EXPECT_EQ(topologyCostCycles(a, b), Tsc::nanosecondsToCycles(50'000));
}

// Cross-NUMA node.
TEST(CpuTopology, crossNumaCost)
{
    CpuTopology a{0, 0, 0};
    CpuTopology b{1, 0, 1}; // different NUMA node
    EXPECT_EQ(topologyCostCycles(a, b), Tsc::nanosecondsToCycles(500'000));
}

// Container-style: package and core known but no NUMA sysfs, so numaNodeId
// stays at the default UINT32_MAX. Both ends are "unknown NUMA"; the cost
// must fall through to cross-NUMA, not classify them as same-node by virtue
// of UINT32_MAX == UINT32_MAX.
TEST(CpuTopology, unknownNumaReturnsCrossNumaCost)
{
    CpuTopology a{0, 0, UINT32_MAX};
    CpuTopology b{1, 0, UINT32_MAX};
    EXPECT_EQ(topologyCostCycles(a, b), Tsc::nanosecondsToCycles(500'000));
}

// cpuInCpulist: single CPU.
TEST(CpuTopology, cpuInCpulistSingle)
{
    EXPECT_TRUE(cpuInCpulist(5, "5"));
    EXPECT_FALSE(cpuInCpulist(4, "5"));
    EXPECT_FALSE(cpuInCpulist(6, "5"));
}

// cpuInCpulist: contiguous range.
TEST(CpuTopology, cpuInCpulistRange)
{
    EXPECT_TRUE(cpuInCpulist(0, "0-7"));
    EXPECT_TRUE(cpuInCpulist(7, "0-7"));
    EXPECT_TRUE(cpuInCpulist(3, "0-7"));
    EXPECT_FALSE(cpuInCpulist(8, "0-7"));
}

// cpuInCpulist: comma-separated entries.
TEST(CpuTopology, cpuInCpulistMultiple)
{
    EXPECT_TRUE(cpuInCpulist(1, "0-3,5,7-9"));
    EXPECT_TRUE(cpuInCpulist(5, "0-3,5,7-9"));
    EXPECT_TRUE(cpuInCpulist(9, "0-3,5,7-9"));
    EXPECT_FALSE(cpuInCpulist(4, "0-3,5,7-9"));
    EXPECT_FALSE(cpuInCpulist(6, "0-3,5,7-9"));
    EXPECT_FALSE(cpuInCpulist(10, "0-3,5,7-9"));
}

// cpuInCpulist: stride syntax. "0-7:2/4" means positions in [0,7] for which
// (pos - 0) % 4 < 2 -- i.e. {0,1,4,5}.
TEST(CpuTopology, cpuInCpulistStride)
{
    EXPECT_TRUE(cpuInCpulist(0, "0-7:2/4"));
    EXPECT_TRUE(cpuInCpulist(1, "0-7:2/4"));
    EXPECT_FALSE(cpuInCpulist(2, "0-7:2/4"));
    EXPECT_FALSE(cpuInCpulist(3, "0-7:2/4"));
    EXPECT_TRUE(cpuInCpulist(4, "0-7:2/4"));
    EXPECT_TRUE(cpuInCpulist(5, "0-7:2/4"));
    EXPECT_FALSE(cpuInCpulist(6, "0-7:2/4"));
    EXPECT_FALSE(cpuInCpulist(7, "0-7:2/4"));
    EXPECT_FALSE(cpuInCpulist(8, "0-7:2/4"));
}

// cpuInCpulist: ":c" without "/d" defaults d=c, so all positions in the range
// are selected (kernel shorthand for the no-stride case).
TEST(CpuTopology, cpuInCpulistStrideDefaultsGroupToUsed)
{
    EXPECT_TRUE(cpuInCpulist(0, "0-7:2"));
    EXPECT_TRUE(cpuInCpulist(3, "0-7:2"));
    EXPECT_TRUE(cpuInCpulist(7, "0-7:2"));
    EXPECT_FALSE(cpuInCpulist(8, "0-7:2"));
}

// cpuInCpulist: stride combined with comma list.
TEST(CpuTopology, cpuInCpulistStrideMixed)
{
    // "0-3,8-15:1/2": {0,1,2,3} ∪ {8,10,12,14}
    EXPECT_TRUE(cpuInCpulist(2, "0-3,8-15:1/2"));
    EXPECT_FALSE(cpuInCpulist(4, "0-3,8-15:1/2"));
    EXPECT_TRUE(cpuInCpulist(8, "0-3,8-15:1/2"));
    EXPECT_FALSE(cpuInCpulist(9, "0-3,8-15:1/2"));
    EXPECT_TRUE(cpuInCpulist(14, "0-3,8-15:1/2"));
    EXPECT_FALSE(cpuInCpulist(15, "0-3,8-15:1/2"));
}

// cpuInCpulist: trailing newline must be tolerated (sysfs reads include it).
TEST(CpuTopology, cpuInCpulistTrailingNewline)
{
    EXPECT_TRUE(cpuInCpulist(3, "0-7\n"));
    EXPECT_FALSE(cpuInCpulist(8, "0-7\n"));
}

// readCpuTopologies: smoke test -- must not crash and should populate at least
// one CPU with a valid package ID on a real Linux system.
TEST(CpuTopology, readDoesNotCrash)
{
    uint32_t count = getProcessorCount();
    ASSERT_GT(count, 0u);

    auto topologies = std::make_unique<CpuTopology[]>(count);
    readCpuTopologies(topologies.get(), count);

    // Only assert the call completed without crashing; sysfs entries may be
    // absent in containers so we make no assertion about the topology values.
}

} // namespace silk
