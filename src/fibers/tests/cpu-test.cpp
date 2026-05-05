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

// readCpuTopologies: smoke test — must not crash and should populate at least
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
