#pragma once

#include <perf/util/latency.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace silk
{

class ConfigReader;
class ExecutionContext;

/**
 * One node of the pipeline: a leaf workload (cpu, wait, yield) or a composite owning
 * child steps (sequence, loop, parallel, concurrent, chain, maybe). A step is shared
 * by every fiber executing it - per-fiber state lives in the ExecutionContext, and
 * the totals below are merged once per completed fiber under the executor's lock.
 */
class Step
{
public:
    /** Create a step with its dotted config path and its config type key. */
    Step(std::string name, std::string type)
        : name(std::move(name))
        , type(std::move(type))
    {
    }

    virtual ~Step() = default;

    /** Parse the step's section: its keys and, for composites, its child steps. */
    virtual void parseConfig(ConfigReader * config) = 0;

    /** Link cross-step references after the whole pipeline parsed - a submit finds its pool. */
    virtual void resolve(const std::vector<Step *> & steps);

    /** Run one execution on the calling fiber, counting and measuring it when configured. */
    void execute(ExecutionContext * context);

    /** Append this step and every descendant in config order. */
    void collect(std::vector<Step *> * steps);

    /** The step's dotted config path - unique within the pipeline. */
    const std::string & getName() const { return name; }

    /** The step's config type key. */
    const std::string & getType() const { return type; }

    /** Merged post-warmup executions. */
    uint64_t getExecutions() const { return totalExecutions; }

    /** Merged latency histogram; counts samples only for measured steps. */
    const LatencyHistogram * getLatencies() const { return &latencies; }

    /** Fold a completed fiber's executions into the total; runs under the executor's lock. */
    void mergeExecutions(uint64_t count) { totalExecutions += count; }

    /** Fold a completed fiber's latency histogram into the total; runs under the executor's lock. */
    void mergeLatencies(const LatencyHistogram & samples) { latencies.merge(samples); }

protected:
    /** One execution's workload - implemented by each step type. */
    virtual void runStep(ExecutionContext * context) = 0;

    /** Parse the keys every step accepts - measure. */
    void parseCommon(ConfigReader * config);

    /** Parse every child section as a step, in file order. */
    void parseChildren(ConfigReader * config);

    /** Abort when the section has child sections - leaf steps take none. */
    void requireLeaf(ConfigReader * config);

    /** Abort when the section has no child steps. */
    void requireChildren();

    /** Execute the child steps in order on the calling fiber. */
    void executeChildren(ExecutionContext * context);

    /** Dotted config path. */
    std::string name;

    /** Config type key. */
    std::string type;

    /** Record per-execution wall time; loop and chain measure per iteration / round instead. */
    bool measure = false;

    /** Set by steps that count executions themselves (loop iterations, chain rounds). */
    bool selfCounting = false;

    /** Child steps in config order; empty for leaf steps. */
    std::vector<std::unique_ptr<Step>> children;

    /** Post-warmup executions merged from completed fibers. */
    uint64_t totalExecutions = 0;

    /** Latency histogram merged from completed fibers. */
    LatencyHistogram latencies;
};

/** Create the step for a config section by its type key; an unknown type aborts naming the step. */
std::unique_ptr<Step> createStep(std::string name, std::string type);

/** Create the pipeline root - a sequence holding the pipeline section's child steps. */
std::unique_ptr<Step> makePipeline();

} // namespace silk
