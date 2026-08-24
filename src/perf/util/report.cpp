#include "report.h"

#include <silk/fibers/fiber.h>
#include <silk/util/perf.h>

#include <cstdio>
#include <vector>

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
