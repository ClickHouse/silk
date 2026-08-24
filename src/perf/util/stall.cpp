#include "stall.h"

#include <silk/util/platform.h>
#include <silk/util/tsc.h>

#include <cstring>
#include <random>

uint32_t StallScheduler::next() noexcept
{
    if (rateHz <= 0.0)
    {
        return 0;
    }
    uint64_t now = silk::Tsc::getCycles();
    if (now < nextStallCycles)
    {
        return 0;
    }
    std::exponential_distribution<double> dist(rateHz);
    double gapNs = dist(rng) * 1'000'000'000.0;
    nextStallCycles = now + silk::Tsc::nanosecondsToCycles(static_cast<uint64_t>(gapNs));
    return static_cast<uint32_t>(stallNs);
}

void busyLoopForStall(const char * buf) noexcept
{
    uint32_t stallNs = 0;
    std::memcpy(&stallNs, buf, sizeof(stallNs));
    if (stallNs == 0)
    {
        return;
    }
    uint64_t target = silk::Tsc::getCycles() + silk::Tsc::nanosecondsToCycles(stallNs);
    while (silk::Tsc::getCycles() < target)
    {
        silk::cpuPause();
    }
}
