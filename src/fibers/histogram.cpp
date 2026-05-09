#include "histogram.h"

namespace silk
{

uint64_t Histogram::percentile(double p) const noexcept
{
    uint64_t total = count();
    if (total == 0)
    {
        return 0;
    }

    // Linear interpolation within the bucket containing the p-th sample.
    // Bucket i covers [lo, hi) where lo = (i == 0 ? 0 : 1 << i), hi = 1 << (i + 1).
    double target = p * static_cast<double>(total);
    uint64_t accumulated = 0;
    for (uint32_t i = 0; i < NUM_BUCKETS; ++i)
    {
        uint64_t prev = accumulated;
        accumulated += buckets[i];
        if (buckets[i] > 0 && static_cast<double>(accumulated) >= target)
        {
            uint64_t lo = i > 0 ? (uint64_t(1) << i) : 0;
            uint64_t hi = uint64_t(1) << (i + 1);
            double fraction = (target - static_cast<double>(prev)) / static_cast<double>(buckets[i]);
            return lo + static_cast<uint64_t>(fraction * static_cast<double>(hi - lo));
        }
    }
    return uint64_t(1) << NUM_BUCKETS;
}

} // namespace silk
