#pragma once

#include <cstdint>
#include <vector>

/**
 * Fixed-size log-linear latency histogram: values below 64 ns record exactly, wider
 * values keep the top six bits under the leading bit - ~1.6% relative resolution -
 * alongside the exact count, sum, minimum, and maximum. Collection stays O(1) memory
 * at any run length and the percentile report needs no sort.
 */
class LatencyHistogram
{
public:
    /** Record one latency sample. */
    void record(uint64_t ns) noexcept;

    /** Fold other's samples into this histogram. */
    void merge(const LatencyHistogram & other) noexcept;

    /** Samples recorded. */
    uint64_t getCount() const noexcept { return total; }

    /** Exact bounds and moments over every recorded sample. */
    uint64_t getMinNs() const noexcept { return total ? minNs : 0; }
    uint64_t getMaxNs() const noexcept { return maxNs; }
    double getSumNs() const noexcept { return sumNs; }
    double getSumSquaredNs() const noexcept { return sumSquaredNs; }

    /** The latency at percentile pct, in microseconds. */
    double getPercentileUs(double pct) const noexcept;

private:
    /** Sub-bucket resolution: this many top mantissa bits per power of two. */
    static constexpr uint32_t SUB_BITS = 6;
    static constexpr uint32_t SUB_COUNT = 1u << SUB_BITS;
    static constexpr uint32_t BUCKET_COUNT = 64 * SUB_COUNT;

    static uint32_t bucketOf(uint64_t ns) noexcept;
    static uint64_t bucketValueNs(uint32_t bucket) noexcept;

    /** Sample counts by log-linear bucket. */
    uint64_t buckets[BUCKET_COUNT] = {};

    /** Exact aggregates over every recorded sample. */
    uint64_t total = 0;
    uint64_t minNs = UINT64_MAX;
    uint64_t maxNs = 0;
    double sumNs = 0.0;
    double sumSquaredNs = 0.0;
};

/**
 * Sort latNs, compute latency statistics, and print the "latency_us" JSON section.
 */
void printLatencyUs(std::vector<uint64_t> & latNs) noexcept;

/**
 * Compute latency statistics from the histogram and print the "latency_us" JSON section.
 */
void printLatencyUs(const LatencyHistogram & latencies) noexcept;
