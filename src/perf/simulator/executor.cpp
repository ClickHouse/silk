#include "config.h"
#include "executor.h"
#include "pipeline.h"

#include <silk/fibers/fiber.h>
#include <silk/util/assert.h>
#include <silk/util/tsc.h>

#include <cstdio>
#include <mutex>

namespace silk
{

ExecutionContext::ExecutionContext(Executor * executor, uint64_t seed)
    : executor(executor)
    , rng(seed)
    , deadlineCycles(executor->getDeadlineCycles())
    , warmupEndCycles(executor->getWarmupEndCycles())
{
}

void ExecutionContext::countExecutions(Step * step, uint64_t count)
{
    for (auto & [entryStep, entryCount] : executionCounts)
    {
        if (entryStep == step)
        {
            entryCount += count;
            return;
        }
    }

    executionCounts.emplace_back(step, count);
}

void ExecutionContext::recordLatency(Step * step, uint64_t sampleNs)
{
    for (auto & [entryStep, histogram] : latencySamples)
    {
        if (entryStep == step)
        {
            histogram.record(sampleNs);
            return;
        }
    }

    latencySamples.emplace_back(step, LatencyHistogram());
    latencySamples.back().second.record(sampleNs);
}

bool ExecutionContext::stallDue(Step * step, uint64_t rateHz, uint64_t nowCycles)
{
    // Exponential inter-arrivals at rateHz - the net-perf StallScheduler analog; the
    // first draw arms the clock, so a fiber's first request is never marked.
    std::exponential_distribution<double> distribution(static_cast<double>(rateHz));

    for (auto & [entryStep, entryDueCycles] : stallDueCycles)
    {
        if (entryStep == step)
        {
            if (nowCycles < entryDueCycles)
            {
                return false;
            }

            entryDueCycles = nowCycles + Tsc::nanosecondsToCycles(static_cast<uint64_t>(distribution(rng) * 1e9));
            return true;
        }
    }

    uint64_t dueCycles = nowCycles + Tsc::nanosecondsToCycles(static_cast<uint64_t>(distribution(rng) * 1e9));
    stallDueCycles.emplace_back(step, dueCycles);
    return false;
}

uint64_t ExecutionContext::getWorkerBinding(Step * step, std::atomic<uint64_t> * cursor, uint64_t workerCount)
{
    for (const auto & [entryStep, entryWorkerIndex] : workerBindings)
    {
        if (entryStep == step)
        {
            return entryWorkerIndex;
        }
    }

    uint64_t workerIndex = cursor->fetch_add(1, std::memory_order_relaxed) % workerCount;
    workerBindings.emplace_back(step, workerIndex);
    return workerIndex;
}

void Executor::parseConfig(ConfigReader * config)
{
    if (config)
    {
        durationNs = config->readDurationNsOpt("duration").value_or(durationNs);
        warmupNs = config->readDurationNsOpt("warmup").value_or(warmupNs);
        seed = config->readUint64Opt("seed").value_or(seed);
    }
}

void Executor::execute(Step * root)
{
    SILK_ASSERT(warmupNs < durationNs, "the warmup must end before the run deadline");

    // Link cross-step references now that every step exists - a submit finds its pool.
    std::vector<Step *> steps;
    root->collect(&steps);

    for (Step * step : steps)
    {
        step->resolve(steps);
    }

    rootStep = root;
    startCycles = Tsc::getCycles();
    warmupEndCycles = startCycles + Tsc::nanosecondsToCycles(warmupNs);
    deadlineCycles = startCycles + Tsc::nanosecondsToCycles(durationNs);

    RootTask task{this, root, seed};
    int r = FiberScheduler::run(rootFiberMain, std::move(task));
    SILK_ASSERT(!r, "the root pipeline fiber failed: r=%d", r);

    endCycles = Tsc::getCycles();
}

int Executor::rootFiberMain(RootTask * task) noexcept
{
    ExecutionContext context(task->executor, task->seed);
    task->root->execute(&context);
    task->executor->mergeContext(&context);
    return 0;
}

void Executor::mergeContext(ExecutionContext * context)
{
    std::lock_guard guard(mergeLock);

    for (const auto & [step, count] : context->executionCounts)
    {
        step->mergeExecutions(count);
    }

    for (const auto & [step, histogram] : context->latencySamples)
    {
        step->mergeLatencies(histogram);
    }
}

void Executor::printReport()
{
    double durationS = static_cast<double>(Tsc::cyclesToNanoseconds(endCycles - startCycles)) / 1e9;
    uint64_t measuredCycles = endCycles > warmupEndCycles ? endCycles - warmupEndCycles : 0;
    double measuredS = static_cast<double>(Tsc::cyclesToNanoseconds(measuredCycles)) / 1e9;

    printf("  \"duration_s\": %.3f,\n", durationS);
    printf("  \"measured_s\": %.3f,\n", measuredS);
    printf("  \"steps\": {\n");

    std::vector<Step *> steps;
    rootStep->collect(&steps);

    bool first = true;

    for (Step * step : steps)
    {
        if (step->getName().empty())
        {
            continue;
        }

        if (!first)
        {
            printf(",\n");
        }
        first = false;

        double rate = measuredS > 0.0 ? static_cast<double>(step->getExecutions()) / measuredS : 0.0;
        printf(
            "    \"%s\": { \"type\": \"%s\", \"executions\": %lu, \"rate\": %.1f",
            step->getName().c_str(),
            step->getType().c_str(),
            step->getExecutions(),
            rate);

        const LatencyHistogram * latencies = step->getLatencies();

        if (latencies->getCount())
        {
            printf(", ");
            printStepLatency(*latencies);
        }

        printf(" }");
    }

    printf("\n  }\n");
}

void Executor::printStepLatency(const LatencyHistogram & latencies)
{
    uint64_t count = latencies.getCount();

    printf(
        "\"latency_us\": { \"count\": %lu, \"avg\": %.2f, \"p50\": %.2f, \"p90\": %.2f, \"p99\": %.2f, \"p999\": %.2f, \"max\": %.2f }",
        count,
        latencies.getSumNs() / static_cast<double>(count) / 1000.0,
        latencies.getPercentileUs(50.0),
        latencies.getPercentileUs(90.0),
        latencies.getPercentileUs(99.0),
        latencies.getPercentileUs(99.9),
        static_cast<double>(latencies.getMaxNs()) / 1000.0);
}

} // namespace silk
