#include <perf/util/stall.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>

TEST(StallTest, ParsesDurationsWithinWireRange)
{
    ASSERT_EQ(parseStallDuration("1ms"), 1'000'000U);
    ASSERT_EQ(parseStallDuration("4294967295ns"), UINT32_MAX);
}

TEST(StallTest, ParsesZeroDuration)
{
    ASSERT_EQ(parseStallDuration("0"), 0U);
    ASSERT_EQ(parseStallDuration("0ns"), 0U);
}

TEST(StallTest, ChecksDurationsWithoutSuffix)
{
    ASSERT_EQ(parseStallDuration("4"), 4'000'000'000U);
    ASSERT_THROW(parseStallDuration("5"), std::out_of_range);
}

TEST(StallTest, RejectsDurationsOutsideWireRange)
{
    ASSERT_THROW(parseStallDuration("4294967296ns"), std::out_of_range);
    ASSERT_THROW(parseStallDuration("5s"), std::out_of_range);
}
