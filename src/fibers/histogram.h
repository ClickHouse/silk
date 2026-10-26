#pragma once

#include <atomic>
#include <cstdint>

namespace silk
{

// Returns floor(log2(n)). n must be non-zero.
static constexpr uint32_t log2Floor(uint64_t n) noexcept
{
    return 63 - __builtin_clzll(n);
}

/**
 * Log2 histogram with clamped range.
 * The caller serializes writers; a reader on another thread sees a relaxed snapshot.
 */
class Histogram
{
public:
    /** Record a duration in nanoseconds. */
    void record(uint64_t ns) noexcept
    {
        uint32_t bucket = ns ? log2Floor(ns) : 0;
        if (bucket >= NUM_BUCKETS)
        {
            bucket = NUM_BUCKETS - 1;
        }
        uint64_t bucketCount = buckets[bucket].load(std::memory_order_relaxed);
        buckets[bucket].store(bucketCount + 1, std::memory_order_relaxed);
    }

    /** Add another histogram's counts into this one. */
    void merge(const Histogram & other) noexcept
    {
        for (uint32_t i = 0; i < NUM_BUCKETS; ++i)
        {
            uint64_t otherCount = other.buckets[i].load(std::memory_order_relaxed);
            uint64_t bucketCount = buckets[i].load(std::memory_order_relaxed);
            buckets[i].store(bucketCount + otherCount, std::memory_order_relaxed);
        }
    }

    /** Return the total number of recorded samples. */
    uint64_t count() const noexcept
    {
        uint64_t total = 0;
        for (const std::atomic<uint64_t> & bucket : buckets)
        {
            total += bucket.load(std::memory_order_relaxed);
        }
        return total;
    }

    /**
     * Return the linearly-interpolated value at the p-th percentile (ns).
     * @param p  Quantile in [0, 1]; e.g. 0.99 for p99.
     */
    uint64_t percentile(double p) const noexcept;

private:
    static constexpr uint64_t MAX_NS = 1'000'000'000; // 1 s
    static constexpr uint32_t NUM_BUCKETS = log2Floor(MAX_NS) + 2;

    std::atomic<uint64_t> buckets[NUM_BUCKETS] = {};
};

} // namespace silk
