#include "latency.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>

static double percentileUs(const std::vector<uint64_t> & latNs, double pct)
{
    if (latNs.empty())
    {
        return 0.0;
    }
    uint64_t idx = static_cast<uint64_t>(static_cast<double>(latNs.size() - 1) * pct / 100.0);
    return static_cast<double>(latNs[idx]) / 1000.0;
}

void printLatencyUs(std::vector<uint64_t> & latNs) noexcept
{
    std::sort(latNs.begin(), latNs.end());

    uint64_t total = latNs.size();

    double sumNs = 0.0;
    for (uint64_t v : latNs)
    {
        sumNs += static_cast<double>(v);
    }
    double meanNs = total > 0 ? sumNs / static_cast<double>(total) : 0.0;

    double sumSq = 0.0;
    for (uint64_t v : latNs)
    {
        double d = static_cast<double>(v) - meanNs;
        sumSq += d * d;
    }
    double stdevNs = total > 0 ? std::sqrt(sumSq / static_cast<double>(total)) : 0.0;

    printf("  \"latency_us\": {\n");
    printf("    \"min\": %.2f,\n", total > 0 ? latNs.front() / 1000.0 : 0.0);
    printf("    \"max\": %.2f,\n", total > 0 ? latNs.back() / 1000.0 : 0.0);
    printf("    \"avg\": %.2f,\n", meanNs / 1000.0);
    printf("    \"stdev\": %.2f,\n", stdevNs / 1000.0);
    printf("    \"p1\":    %.2f,\n", percentileUs(latNs, 1.0));
    printf("    \"p50\":   %.2f,\n", percentileUs(latNs, 50.0));
    printf("    \"p90\":   %.2f,\n", percentileUs(latNs, 90.0));
    printf("    \"p95\":   %.2f,\n", percentileUs(latNs, 95.0));
    printf("    \"p99\":   %.2f,\n", percentileUs(latNs, 99.0));
    printf("    \"p99_9\": %.2f,\n", percentileUs(latNs, 99.9));
    printf("    \"p99_99\":%.2f\n", percentileUs(latNs, 99.99));
    printf("  }\n");
}

void LatencyHistogram::record(uint64_t ns) noexcept
{
    buckets[bucketOf(ns)]++;
    total++;

    if (ns < minNs)
    {
        minNs = ns;
    }

    if (ns > maxNs)
    {
        maxNs = ns;
    }

    double valueNs = static_cast<double>(ns);
    sumNs += valueNs;
    sumSquaredNs += valueNs * valueNs;
}

void LatencyHistogram::merge(const LatencyHistogram & other) noexcept
{
    for (uint32_t i = 0; i < BUCKET_COUNT; ++i)
    {
        buckets[i] += other.buckets[i];
    }

    total += other.total;
    minNs = std::min(minNs, other.minNs);
    maxNs = std::max(maxNs, other.maxNs);
    sumNs += other.sumNs;
    sumSquaredNs += other.sumSquaredNs;
}

double LatencyHistogram::getPercentileUs(double pct) const noexcept
{
    if (!total)
    {
        return 0.0;
    }

    uint64_t rank = static_cast<uint64_t>(static_cast<double>(total - 1) * pct / 100.0);
    uint64_t seen = 0;
    for (uint32_t i = 0; i < BUCKET_COUNT; ++i)
    {
        seen += buckets[i];
        if (seen > rank)
        {
            return static_cast<double>(bucketValueNs(i)) / 1000.0;
        }
    }

    return static_cast<double>(maxNs) / 1000.0;
}

uint32_t LatencyHistogram::bucketOf(uint64_t ns) noexcept
{
    if (ns < SUB_COUNT)
    {
        return static_cast<uint32_t>(ns);
    }

    uint32_t exponent = static_cast<uint32_t>(std::bit_width(ns)) - 1;
    uint32_t mantissa = static_cast<uint32_t>(ns >> (exponent - SUB_BITS)) & (SUB_COUNT - 1);
    return (exponent - SUB_BITS + 1) * SUB_COUNT + mantissa;
}

uint64_t LatencyHistogram::bucketValueNs(uint32_t bucket) noexcept
{
    if (bucket < SUB_COUNT)
    {
        return bucket;
    }

    uint32_t exponent = bucket / SUB_COUNT + SUB_BITS - 1;
    uint64_t mantissa = bucket % SUB_COUNT;
    uint64_t width = uint64_t{1} << (exponent - SUB_BITS);
    return (uint64_t{1} << exponent) + mantissa * width + width / 2;
}

void printLatencyUs(const LatencyHistogram & latencies) noexcept
{
    uint64_t total = latencies.getCount();
    double meanNs = total ? latencies.getSumNs() / static_cast<double>(total) : 0.0;
    double varianceNs = total ? latencies.getSumSquaredNs() / static_cast<double>(total) - meanNs * meanNs : 0.0;
    double stdevNs = varianceNs > 0.0 ? std::sqrt(varianceNs) : 0.0;

    printf("  \"latency_us\": {\n");
    printf("    \"min\": %.2f,\n", static_cast<double>(latencies.getMinNs()) / 1000.0);
    printf("    \"max\": %.2f,\n", static_cast<double>(latencies.getMaxNs()) / 1000.0);
    printf("    \"avg\": %.2f,\n", meanNs / 1000.0);
    printf("    \"stdev\": %.2f,\n", stdevNs / 1000.0);
    printf("    \"p1\":    %.2f,\n", latencies.getPercentileUs(1.0));
    printf("    \"p50\":   %.2f,\n", latencies.getPercentileUs(50.0));
    printf("    \"p90\":   %.2f,\n", latencies.getPercentileUs(90.0));
    printf("    \"p95\":   %.2f,\n", latencies.getPercentileUs(95.0));
    printf("    \"p99\":   %.2f,\n", latencies.getPercentileUs(99.0));
    printf("    \"p99_9\": %.2f,\n", latencies.getPercentileUs(99.9));
    printf("    \"p99_99\":%.2f\n", latencies.getPercentileUs(99.99));
    printf("  }\n");
}
