#include "fibers/histogram.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace silk
{

// Empty histogram has zero count and percentile returns 0.
TEST(Histogram, empty)
{
    Histogram histogram;
    EXPECT_EQ(histogram.count(), 0u);
    EXPECT_EQ(histogram.percentile(0.50), 0u);
    EXPECT_EQ(histogram.percentile(0.99), 0u);
}

// record(0) and record(1) both fall into bucket 0 ([0, 2)).
TEST(Histogram, recordZeroAndOne)
{
    Histogram histogram;
    histogram.record(0);
    histogram.record(1);
    EXPECT_EQ(histogram.count(), 2u);
}

// record beyond MAX_NS saturates into the last bucket; count is still tracked.
TEST(Histogram, recordSaturates)
{
    Histogram histogram;
    histogram.record(uint64_t{1} << 30);
    histogram.record(uint64_t{1} << 40);
    histogram.record(UINT64_MAX);
    EXPECT_EQ(histogram.count(), 3u);
    // p99 should land in the saturation bucket [2^30, 2^31).
    uint64_t value = histogram.percentile(0.99);
    EXPECT_GE(value, uint64_t{1} << 30);
}

// All samples in one bucket: percentile lies within that bucket's range.
TEST(Histogram, percentileSingleBucket)
{
    Histogram histogram;
    // Bucket for 1024 ns is log2Floor(1024)=10, covering [1024, 2048).
    for (int i = 0; i < 100; ++i)
    {
        histogram.record(1024 + i);
    }
    uint64_t p50 = histogram.percentile(0.50);
    uint64_t p99 = histogram.percentile(0.99);
    EXPECT_GE(p50, 1024u);
    EXPECT_LT(p50, 2048u);
    EXPECT_GE(p99, 1024u);
    EXPECT_LT(p99, 2048u);
}

// Flat distribution across two buckets: percentiles in different halves
// land in different buckets.
TEST(Histogram, percentileTwoBuckets)
{
    Histogram histogram;
    // 100 samples in [1024, 2048), 100 samples in [2048, 4096).
    for (int i = 0; i < 100; ++i)
    {
        histogram.record(1500);
        histogram.record(3000);
    }
    EXPECT_EQ(histogram.count(), 200u);
    // p25 lands in the first bucket [1024, 2048).
    uint64_t p25 = histogram.percentile(0.25);
    EXPECT_GE(p25, 1024u);
    EXPECT_LT(p25, 2048u);
    // p75 lands in the second bucket [2048, 4096).
    uint64_t p75 = histogram.percentile(0.75);
    EXPECT_GE(p75, 2048u);
    EXPECT_LT(p75, 4096u);
}

// merge sums bucket-wise; counts compose.
TEST(Histogram, merge)
{
    Histogram a;
    Histogram b;
    a.record(100);
    a.record(200);
    b.record(300);
    b.record(400);
    b.record(500);

    a.merge(b);
    EXPECT_EQ(a.count(), 5u);
    EXPECT_EQ(b.count(), 3u); // b unchanged
}

// merge with an empty histogram is a no-op.
TEST(Histogram, mergeEmpty)
{
    Histogram a;
    Histogram empty;
    a.record(100);
    a.record(200);

    a.merge(empty);
    EXPECT_EQ(a.count(), 2u);
}

} // namespace silk
