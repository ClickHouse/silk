#pragma once

#include <perf/util/latency.h>
#include <silk/fibers/mutex.h>

#include <atomic>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace silk
{

class ConfigReader;
class Executor;
class Step;

/**
 * Per-fiber execution state: the owning executor, a deterministic random stream, the
 * run deadlines, and local sinks for execution counts and measured latency samples.
 * Every fiber executes steps against its own context and merges it into the shared
 * step totals once, when the fiber completes.
 */
class ExecutionContext
{
public:
    /** Create a context for one fiber, copying the deadlines from the executor. */
    ExecutionContext(Executor * executor, uint64_t seed);

    /** Add completed executions of step to the local sink. */
    void countExecutions(Step * step, uint64_t count);

    /** Add a measured wall-time sample of step to the local sink. */
    void recordLatency(Step * step, uint64_t sampleNs);

    /** True when step's per-fiber Poisson stall clock is due; reschedules the next due on fire. */
    bool stallDue(Step * step, uint64_t rateHz, uint64_t nowCycles);

    /** The fiber's sticky worker index for step, taking the next round-robin index from cursor on first use. */
    uint64_t getWorkerBinding(Step * step, std::atomic<uint64_t> * cursor, uint64_t workerCount);

    /** The executor running the pipeline. */
    Executor * executor;

    /** Deterministic random stream - seeded by the spawning fiber's stream. */
    std::mt19937_64 rng;

    /** Run deadline in TSC cycles - copied from the executor at construction. */
    uint64_t deadlineCycles;

    /** Warmup end in TSC cycles - executions before it are not counted. */
    uint64_t warmupEndCycles;

    /** Locally buffered post-warmup execution counts per step. */
    std::vector<std::pair<Step *, uint64_t>> executionCounts;

    /** Locally buffered latency histograms per measured step. */
    std::vector<std::pair<Step *, LatencyHistogram>> latencySamples;

    /** Per-step Poisson stall clocks - the next due stamp in TSC cycles. */
    std::vector<std::pair<Step *, uint64_t>> stallDueCycles;

    /** Sticky worker bindings per affinity pool. */
    std::vector<std::pair<Step *, uint64_t>> workerBindings;
};

/**
 * Runs a parsed pipeline to a configured deadline on the fiber scheduler and reports
 * per-step statistics: the root step executes on one fiber, composite steps fan out
 * further fibers, and every completed fiber merges its context back under a single
 * cold-path lock.
 */
class Executor
{
public:
    /** Parse the run settings from the params section: duration, warmup, seed. Null applies the defaults. */
    void parseConfig(ConfigReader * config);

    /** Override the configured run duration. */
    void setDurationNs(uint64_t durationNs_) { durationNs = durationNs_; }

    /** Override the configured warmup. */
    void setWarmupNs(uint64_t warmupNs_) { warmupNs = warmupNs_; }

    /** Execute root to the deadline; returns after every spawned fiber completed. */
    void execute(Step * root);

    /** Print the report JSON fields: run timing and per-step statistics. */
    void printReport();

    /** Run deadline in TSC cycles; valid during execute. */
    uint64_t getDeadlineCycles() const { return deadlineCycles; }

    /** Warmup end in TSC cycles; valid during execute. */
    uint64_t getWarmupEndCycles() const { return warmupEndCycles; }

    /** Fold a completed fiber's context into the step totals. */
    void mergeContext(ExecutionContext * context);

private:
    /** Root fiber parameters. */
    struct RootTask
    {
        /** The executor running the pipeline. */
        Executor * executor;

        /** The pipeline root step. */
        Step * root;

        /** Seed of the root random stream. */
        uint64_t seed;
    };

    static int rootFiberMain(RootTask * task) noexcept;
    static void printStepLatency(const LatencyHistogram & latencies);

    /** Run length; the pipeline deadline is start + duration. */
    uint64_t durationNs = 10'000'000'000;

    /** Post-start settling time excluded from counts and samples. */
    uint64_t warmupNs = 0;

    /** Root seed of the deterministic per-fiber random streams. */
    uint64_t seed = 1;

    /** The pipeline root, kept for the report. */
    Step * rootStep = nullptr;

    /** Run timing in TSC cycles, set by execute. */
    uint64_t startCycles = 0;
    uint64_t warmupEndCycles = 0;
    uint64_t deadlineCycles = 0;
    uint64_t endCycles = 0;

    /** Serializes completed-context merging into the step totals; suspends the fiber, not the thread. */
    FiberMutex mergeLock;
};

} // namespace silk
