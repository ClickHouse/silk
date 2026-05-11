#include "common.h"

#include <silk/fibers/fiber.h>
#include <silk/util/assert.h>
#include <silk/util/perf.h>
#include <silk/util/platform.h>
#include <silk/util/tsc.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <pthread.h>
#include <signal.h>

sigset_t blockSignals() noexcept
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGPIPE);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    return mask;
}

bool sigwaitFor(const sigset_t & mask, uint64_t ns) noexcept
{
    uint64_t endNs = silk::getTimeNanoseconds() + ns;

    for (;;)
    {
        uint64_t nowNs = silk::getTimeNanoseconds();
        if (nowNs >= endNs)
        {
            return false;
        }

        uint64_t remainingNs = endNs - nowNs;
        struct timespec timeout = {
            .tv_sec = static_cast<time_t>(remainingNs / 1'000'000'000ULL),
            .tv_nsec = static_cast<long>(remainingNs % 1'000'000'000ULL),
        };

        int r = sigtimedwait(&mask, nullptr, &timeout);
        if (r > 0)
        {
            return true;
        }

        r = errno;
        if (r == EAGAIN)
        {
            return false;
        }
        SILK_ASSERT(r == EINTR);
    }
}

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

static const char * profileEventKindName(silk::ProfileEventKind kind) noexcept
{
    switch (kind)
    {
        case silk::ProfileEventKind::READY_WAIT:
            return "ready_wait";
        case silk::ProfileEventKind::FIBER_RUN:
            return "fiber_run";
        case silk::ProfileEventKind::SUSPEND_WAIT:
            return "suspend_wait";
        case silk::ProfileEventKind::IO_WAIT:
            return "io_wait";
        case silk::ProfileEventKind::CQ_WAIT:
            return "cq_wait";
        case silk::ProfileEventKind::SQ_WAIT:
            return "sq_wait";
        case silk::ProfileEventKind::SUBMIT_IO:
            return "submit_io";
        default:
            return "unknown";
    }
}

void printCounters() noexcept
{
    uint32_t count = silk::Perf::getSimpleCounterCount();
    std::vector<silk::Perf::SimpleCounter> out(count);
    count = silk::Perf::getSimpleCounters(0, out.data(), count);

    printf("  \"counters\": {\n");
    for (uint32_t i = 0; i < count; ++i)
    {
        uint64_t value = out[i].value.load(std::memory_order_relaxed);
        printf("    \"%s\": %lu%s\n", silk::Perf::getSimpleCounterInfo(i).name, value, i + 1 < count ? "," : "");
    }
    printf("  }\n");
}

void printSchedulerLatency() noexcept
{
    printf("  \"scheduler_latency\": {\n");

    bool firstKind = true;
    for (uint32_t k = 0; k < static_cast<uint32_t>(silk::ProfileEventKind::MAX); ++k)
    {
        auto kind = static_cast<silk::ProfileEventKind>(k);

        // Collect non-zero categories.
        bool firstCat = true;
        for (uint32_t cat = 0; cat < 256; ++cat)
        {
            silk::LatencyReport report = silk::FiberScheduler::reportLatency(kind, static_cast<uint8_t>(cat));
            if (report.count == 0)
            {
                continue;
            }
            if (firstCat)
            {
                if (!firstKind)
                {
                    printf(",\n");
                }
                printf("    \"%s\": {\n", profileEventKindName(kind));
                firstKind = false;
            }
            printf(
                "      %s\"%u#\": { \"count\": %lu, \"p50_ns\": %lu, \"p90_ns\": %lu, \"p99_ns\": %lu, \"p999_ns\": %lu }",
                firstCat ? "" : ",\n      ",
                cat,
                report.count,
                report.p50,
                report.p90,
                report.p99,
                report.p999);
            firstCat = false;
        }
        if (!firstCat)
        {
            printf("\n    }");
        }
    }
    if (!firstKind)
    {
        printf("\n");
    }
    printf("  }\n");
}

uint64_t parseSize(const std::string & str)
{
    uint64_t n = std::stoull(str);
    char suffix = str.empty() ? '\0' : str.back();
    if (suffix == 'k' || suffix == 'K')
    {
        return n * 1024ULL;
    }
    if (suffix == 'm' || suffix == 'M')
    {
        return n * 1024ULL * 1024;
    }
    if (suffix == 'g' || suffix == 'G')
    {
        return n * 1024ULL * 1024 * 1024;
    }
    return n;
}

uint64_t parseDuration(const std::string & str)
{
    size_t pos;
    uint64_t n = std::stoull(str, &pos);
    std::string suffix = str.substr(pos);
    if (suffix == "ns")
    {
        return n;
    }
    if (suffix.empty())
    {
        return n * 1'000'000'000ULL;
    }
    if (suffix == "us")
    {
        return n * 1'000ULL;
    }
    if (suffix == "ms")
    {
        return n * 1'000'000ULL;
    }
    if (suffix == "s")
    {
        return n * 1'000'000'000ULL;
    }
    if (suffix == "m")
    {
        return n * 60'000'000'000ULL;
    }
    throw std::invalid_argument("unknown duration suffix: " + suffix);
}

std::string formatDuration(uint64_t ns)
{
    if (ns % 1'000'000'000ULL == 0)
    {
        return std::to_string(ns / 1'000'000'000ULL) + "s";
    }
    if (ns % 1'000'000ULL == 0)
    {
        return std::to_string(ns / 1'000'000ULL) + "ms";
    }
    if (ns % 1'000ULL == 0)
    {
        return std::to_string(ns / 1'000ULL) + "us";
    }
    return std::to_string(ns) + "ns";
}

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
