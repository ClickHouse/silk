#include "config.h"
#include "executor.h"
#include "pipeline.h"

#include <silk/fibers/blocking-queue.h>
#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>
#include <silk/util/assert.h>
#include <silk/util/platform.h>
#include <silk/util/tsc.h>

#include <atomic>
#include <random>

namespace silk
{

//
// Step base.
//

void Step::execute(ExecutionContext * context)
{
    if (selfCounting)
    {
        runStep(context);
        return;
    }

    if (!measure)
    {
        // Counted by the start stamp, like the measured branch - an execution spanning
        // the warmup boundary lands on one side under both rules.
        uint64_t startCycles = Tsc::getCycles();
        runStep(context);

        if (startCycles >= context->warmupEndCycles)
        {
            context->countExecutions(this, 1);
        }

        return;
    }

    uint64_t startCycles = Tsc::getCycles();
    runStep(context);
    uint64_t endCycles = Tsc::getCycles();

    if (startCycles >= context->warmupEndCycles)
    {
        context->countExecutions(this, 1);
        context->recordLatency(this, Tsc::cyclesToNanoseconds(endCycles - startCycles));
    }
}

void Step::resolve(const std::vector<Step *> & steps)
{
    SILK_UNUSED(steps);
}

void Step::collect(std::vector<Step *> * steps)
{
    steps->push_back(this);

    for (const std::unique_ptr<Step> & child : children)
    {
        child->collect(steps);
    }
}

void Step::parseCommon(ConfigReader * config)
{
    measure = config->readUint64Opt("measure").value_or(0) != 0;
}

void Step::parseChildren(ConfigReader * config)
{
    std::vector<std::string> childNames;
    config->list(&childNames);

    for (const std::string & childName : childNames)
    {
        ConfigReader * childConfig = config->get(childName.c_str());
        std::string childType = childConfig->readString("type");
        std::unique_ptr<Step> child = createStep(name.empty() ? childName : name + "." + childName, childType);
        child->parseConfig(childConfig);
        children.push_back(std::move(child));
    }
}

void Step::requireLeaf(ConfigReader * config)
{
    std::vector<std::string> childNames;
    config->list(&childNames);

    bool leaf = childNames.empty();
    SILK_ASSERT(leaf, "step %s takes no child steps", name.c_str());
}

void Step::requireChildren()
{
    bool hasChildren = !children.empty();
    SILK_ASSERT(hasChildren, "step %s has no child steps", name.c_str());
}

void Step::executeChildren(ExecutionContext * context)
{
    for (const std::unique_ptr<Step> & child : children)
    {
        child->execute(context);
    }
}

//
// Leaf steps.
//

/**
 * Burns CPU for a fixed time - the compute phase of a request, or an injected stall.
 * Keys: duration, or cycles for a raw TSC count.
 */
class CpuStep final : public Step
{
public:
    using Step::Step;

    void parseConfig(ConfigReader * config) override;

private:
    void runStep(ExecutionContext * context) override;

    /** Busy-loop length in TSC cycles. */
    uint64_t cpuCycles = 0;
};

void CpuStep::parseConfig(ConfigReader * config)
{
    parseCommon(config);
    requireLeaf(config);

    std::optional<uint64_t> cycles = config->readUint64Opt("cycles");

    if (cycles)
    {
        cpuCycles = *cycles;
    }
    else
    {
        cpuCycles = Tsc::nanosecondsToCycles(config->readDurationNs("duration"));
    }

    SILK_ASSERT(cpuCycles, "step %s: the cpu burn length is zero", name.c_str());
}

void CpuStep::runStep(ExecutionContext * context)
{
    SILK_UNUSED(context);
    uint64_t endCycles = Tsc::getCycles() + cpuCycles;

    while (Tsc::getCycles() < endCycles)
    {
        cpuPause();
    }
}

/**
 * Suspends the calling fiber for a sleep - the IO or think-time phase of a request.
 * Keys: duration; exponential = 1 draws each sleep from Exp with that mean, turning
 * a loop over this step into a Poisson process.
 */
class WaitStep final : public Step
{
public:
    using Step::Step;

    void parseConfig(ConfigReader * config) override;

private:
    void runStep(ExecutionContext * context) override;

    /** Sleep length (or mean length) in nanoseconds. */
    uint64_t waitNs = 0;

    /** Draw each sleep from an exponential distribution with mean waitNs. */
    bool exponential = false;
};

void WaitStep::parseConfig(ConfigReader * config)
{
    parseCommon(config);
    requireLeaf(config);

    waitNs = config->readDurationNs("duration");
    exponential = config->readUint64Opt("exponential").value_or(0) != 0;
    SILK_ASSERT(waitNs, "step %s: the wait length is zero", name.c_str());
}

void WaitStep::runStep(ExecutionContext * context)
{
    uint64_t sleepNs = waitNs;

    if (exponential)
    {
        std::exponential_distribution<double> distribution(1.0 / static_cast<double>(waitNs));
        sleepNs = static_cast<uint64_t>(distribution(context->rng));
    }

    FiberScheduler::sleep(sleepNs);
}

/**
 * Reschedules the calling fiber through the ready queue. Keys: count.
 */
class YieldStep final : public Step
{
public:
    using Step::Step;

    void parseConfig(ConfigReader * config) override;

private:
    void runStep(ExecutionContext * context) override;

    /** Yields per execution. */
    uint64_t yieldCount = 1;
};

void YieldStep::parseConfig(ConfigReader * config)
{
    parseCommon(config);
    requireLeaf(config);

    yieldCount = config->readUint64Opt("count").value_or(1);
}

void YieldStep::runStep(ExecutionContext * context)
{
    SILK_UNUSED(context);

    for (uint64_t i = 0; i < yieldCount; ++i)
    {
        FiberScheduler::yield();
    }
}

//
// Composite steps.
//

/**
 * Executes its child steps in order on the calling fiber; also the pipeline root.
 */
class SequenceStep final : public Step
{
public:
    using Step::Step;

    void parseConfig(ConfigReader * config) override;

private:
    void runStep(ExecutionContext * context) override;
};

void SequenceStep::parseConfig(ConfigReader * config)
{
    parseCommon(config);
    parseChildren(config);
    requireChildren();
}

void SequenceStep::runStep(ExecutionContext * context)
{
    executeChildren(context);
}

/**
 * Repeats its child steps. Keys: count (0 or absent - until the deadline), duration
 * capping the loop's own run time, period pacing iteration starts for a constant
 * arrival rate. Counts executions per iteration; measure records per-iteration wall
 * time - the request latency of a client loop.
 */
class LoopStep final : public Step
{
public:
    using Step::Step;

    void parseConfig(ConfigReader * config) override;

private:
    void runStep(ExecutionContext * context) override;

    /** Iterations to run; zero runs until the deadline. */
    uint64_t count = 0;

    /** Loop run-time cap in TSC cycles; zero caps at the run deadline only. */
    uint64_t durationCycles = 0;

    /** Iteration start pacing in TSC cycles; zero starts the next iteration immediately. */
    uint64_t periodCycles = 0;

    /** Record per-iteration wall time. */
    bool measureIterations = false;
};

void LoopStep::parseConfig(ConfigReader * config)
{
    parseCommon(config);
    parseChildren(config);
    requireChildren();

    count = config->readUint64Opt("count").value_or(0);
    durationCycles = Tsc::nanosecondsToCycles(config->readDurationNsOpt("duration").value_or(0));
    periodCycles = Tsc::nanosecondsToCycles(config->readDurationNsOpt("period").value_or(0));

    // The loop counts iterations and measures each one rather than the whole run.
    measureIterations = measure;
    measure = false;
    selfCounting = true;
}

void LoopStep::runStep(ExecutionContext * context)
{
    uint64_t startCycles = Tsc::getCycles();
    uint64_t capCycles = context->deadlineCycles;

    if (durationCycles && startCycles + durationCycles < capCycles)
    {
        capCycles = startCycles + durationCycles;
    }

    for (uint64_t iteration = 0; !count || iteration < count; ++iteration)
    {
        uint64_t iterationStartCycles = Tsc::getCycles();

        if (iterationStartCycles >= capCycles)
        {
            break;
        }

        executeChildren(context);

        uint64_t iterationEndCycles = Tsc::getCycles();

        if (iterationStartCycles >= context->warmupEndCycles)
        {
            context->countExecutions(this, 1);

            if (measureIterations)
            {
                context->recordLatency(this, Tsc::cyclesToNanoseconds(iterationEndCycles - iterationStartCycles));
            }
        }

        if (periodCycles)
        {
            uint64_t targetCycles = startCycles + (iteration + 1) * periodCycles;

            if (iterationEndCycles < targetCycles)
            {
                FiberScheduler::sleep(Tsc::cyclesToNanoseconds(targetCycles - iterationEndCycles));
            }
        }
    }
}

/**
 * Fans out count fibers, each executing the child steps once, and joins them all.
 * The fibers home on the spawning fiber's processor - a burst lands as a clump, the
 * way accepted connections do, and spreads only by stealing. Keys: count; measure
 * records the fan-out-to-join wall time.
 */
class ParallelStep final : public Step
{
public:
    using Step::Step;

    void parseConfig(ConfigReader * config) override;

private:
    /** Worker fiber parameters. */
    struct WorkerTask
    {
        /** The parallel step whose children the worker executes. */
        ParallelStep * step;

        /** The executor merging the worker's context. */
        Executor * executor;

        /** Seed of the worker's random stream. */
        uint64_t seed;
    };

    void runStep(ExecutionContext * context) override;
    static int workerFiberMain(WorkerTask * task) noexcept;

    /** Fibers per execution. */
    uint64_t count = 0;
};

void ParallelStep::parseConfig(ConfigReader * config)
{
    parseCommon(config);
    parseChildren(config);
    requireChildren();

    count = config->readUint64("count");
    SILK_ASSERT(count, "step %s: the fiber count is zero", name.c_str());
}

void ParallelStep::runStep(ExecutionContext * context)
{
    std::unique_ptr<FiberFuture[]> done = std::make_unique<FiberFuture[]>(count);

    for (uint64_t i = 0; i < count; ++i)
    {
        WorkerTask task{this, context->executor, context->rng()};
        int r = FiberScheduler::run(workerFiberMain, std::move(task), &done[i]);
        SILK_ASSERT(!r, "step %s: could not start a worker fiber: r=%d", name.c_str(), r);
    }

    for (uint64_t i = 0; i < count; ++i)
    {
        int r = done[i].wait();
        SILK_ASSERT(!r, "step %s: a worker fiber failed: r=%d", name.c_str(), r);
    }
}

int ParallelStep::workerFiberMain(WorkerTask * task) noexcept
{
    ExecutionContext context(task->executor, task->seed);
    task->step->executeChildren(&context);
    task->executor->mergeContext(&context);
    return 0;
}

/**
 * Runs each child step on its own fiber and joins them all - heterogeneous workloads
 * side by side. Measure records the fan-out-to-join wall time.
 */
class ConcurrentStep final : public Step
{
public:
    using Step::Step;

    void parseConfig(ConfigReader * config) override;

private:
    /** Branch fiber parameters. */
    struct BranchTask
    {
        /** The child step the branch executes. */
        Step * child;

        /** The executor merging the branch's context. */
        Executor * executor;

        /** Seed of the branch's random stream. */
        uint64_t seed;
    };

    void runStep(ExecutionContext * context) override;
    static int branchFiberMain(BranchTask * task) noexcept;
};

void ConcurrentStep::parseConfig(ConfigReader * config)
{
    parseCommon(config);
    parseChildren(config);
    requireChildren();
}

void ConcurrentStep::runStep(ExecutionContext * context)
{
    uint64_t count = children.size();
    std::unique_ptr<FiberFuture[]> done = std::make_unique<FiberFuture[]>(count);

    for (uint64_t i = 0; i < count; ++i)
    {
        BranchTask task{children[i].get(), context->executor, context->rng()};
        int r = FiberScheduler::run(branchFiberMain, std::move(task), &done[i]);
        SILK_ASSERT(!r, "step %s: could not start a branch fiber: r=%d", name.c_str(), r);
    }

    for (uint64_t i = 0; i < count; ++i)
    {
        int r = done[i].wait();
        SILK_ASSERT(!r, "step %s: a branch fiber failed: r=%d", name.c_str(), r);
    }
}

int ConcurrentStep::branchFiberMain(BranchTask * task) noexcept
{
    ExecutionContext context(task->executor, task->seed);
    task->child->execute(&context);
    task->executor->mergeContext(&context);
    return 0;
}

/**
 * Executes its child steps with the configured probability per execution - rare
 * events like injected stalls. Keys: probability in [0, 1].
 */
class MaybeStep final : public Step
{
public:
    using Step::Step;

    void parseConfig(ConfigReader * config) override;

private:
    void runStep(ExecutionContext * context) override;

    /** Chance of executing the child steps, per execution. */
    double probability = 0.0;
};

void MaybeStep::parseConfig(ConfigReader * config)
{
    parseCommon(config);
    parseChildren(config);
    requireChildren();

    probability = config->readDouble("probability");
    bool valid = probability >= 0.0 && probability <= 1.0;
    SILK_ASSERT(valid, "step %s: the probability is outside [0, 1]", name.c_str());
}

void MaybeStep::runStep(ExecutionContext * context)
{
    std::uniform_real_distribution<double> distribution(0.0, 1.0);

    if (distribution(context->rng) < probability)
    {
        executeChildren(context);
    }
}

/**
 * A ring of length fibers passing one token: each hop waits for the token, executes
 * the child steps, and wakes the next hop - a closed loop of dependent wakes whose
 * queueing shows as wait time, never as backlog age. Keys: length, rounds (0 or
 * absent - until the deadline). Counts executions per completed round; measure
 * records per-round wall time. An empty body is a pure wake ring.
 */
class ChainStep final : public Step
{
public:
    using Step::Step;

    void parseConfig(ConfigReader * config) override;

private:
    /** Shared ring state, alive for one chain execution. */
    struct RingState
    {
        /** The chain being executed. */
        ChainStep * step = nullptr;

        /** The executor merging the hop contexts. */
        Executor * executor = nullptr;

        /** Hop wake tokens - hop i waits on tokens[i] and sets the next one. */
        std::unique_ptr<FiberFuture[]> tokens;

        /** Raised by hop zero at the deadline or the round limit; every hop propagates the token once more and exits. */
        std::atomic<bool> stop{false};
    };

    /** Hop fiber parameters. */
    struct HopTask
    {
        /** The shared ring. */
        RingState * ring;

        /** Seed of the hop's random stream. */
        uint64_t seed;

        /** Position in the ring. */
        uint64_t index;
    };

    void runStep(ExecutionContext * context) override;
    static int hopFiberMain(HopTask * task) noexcept;

    /** Fibers in the ring. */
    uint64_t length = 0;

    /** Full rounds to run; zero runs until the deadline. */
    uint64_t rounds = 0;

    /** Record per-round wall time. */
    bool measureRounds = false;
};

void ChainStep::parseConfig(ConfigReader * config)
{
    parseCommon(config);
    parseChildren(config);

    length = config->readUint64("length");
    SILK_ASSERT(length >= 2, "step %s: a chain needs at least two fibers", name.c_str());
    rounds = config->readUint64Opt("rounds").value_or(0);

    // The chain counts completed rounds and measures each one rather than the whole run.
    measureRounds = measure;
    measure = false;
    selfCounting = true;
}

void ChainStep::runStep(ExecutionContext * context)
{
    RingState ring;
    ring.step = this;
    ring.executor = context->executor;
    ring.tokens = std::make_unique<FiberFuture[]>(length);

    std::unique_ptr<FiberFuture[]> done = std::make_unique<FiberFuture[]>(length);

    for (uint64_t i = 0; i < length; ++i)
    {
        HopTask task{&ring, context->rng(), i};
        int r = FiberScheduler::run(hopFiberMain, std::move(task), &done[i]);
        SILK_ASSERT(!r, "step %s: could not start a hop fiber: r=%d", name.c_str(), r);
    }

    ring.tokens[0].set(0);

    for (uint64_t i = 0; i < length; ++i)
    {
        int r = done[i].wait();
        SILK_ASSERT(!r, "step %s: a hop fiber failed: r=%d", name.c_str(), r);
    }
}

int ChainStep::hopFiberMain(HopTask * task) noexcept
{
    RingState * ring = task->ring;
    ChainStep * step = ring->step;
    ExecutionContext context(ring->executor, task->seed);
    FiberFuture * token = &ring->tokens[task->index];
    FiberFuture * nextToken = &ring->tokens[(task->index + 1) % step->length];

    uint64_t roundsDone = 0;
    uint64_t roundStartCycles = 0;

    for (;;)
    {
        int r = token->wait();
        SILK_ASSERT(!r, "step %s: a token wait failed: r=%d", step->name.c_str(), r);
        token->reset();

        if (ring->stop.load(std::memory_order_relaxed))
        {
            nextToken->set(0);
            break;
        }

        // Hop zero owns the round accounting and the stop decision - a token receipt
        // after the first one completes a round.
        if (task->index == 0)
        {
            uint64_t nowCycles = Tsc::getCycles();

            if (roundStartCycles)
            {
                ++roundsDone;

                if (roundStartCycles >= context.warmupEndCycles)
                {
                    context.countExecutions(step, 1);

                    if (step->measureRounds)
                    {
                        context.recordLatency(step, Tsc::cyclesToNanoseconds(nowCycles - roundStartCycles));
                    }
                }
            }

            if (nowCycles >= context.deadlineCycles || (step->rounds && roundsDone >= step->rounds))
            {
                ring->stop.store(true, std::memory_order_relaxed);
                nextToken->set(0);
                break;
            }

            roundStartCycles = nowCycles;
        }

        step->executeChildren(&context);
        nextToken->set(0);
    }

    ring->executor->mergeContext(&context);
    return 0;
}

/** One submitted request, on the submitting fiber's stack; the worker's completion set is its last touch. */
struct PoolRequest
{
    /** Request-carried stall the worker burns before the pool's child steps - the net-perf stall_ns analog. */
    uint64_t stallCycles = 0;

    /** Completed by the serving worker after the pool's child steps ran. */
    FiberFuture done;
};

/**
 * A pool of worker fibers serving submitted requests: each worker dequeues a request,
 * executes the child steps, and completes the request - a real dependent wake chain of
 * enqueue, worker doorbell, execution, and completion wake. A batch limit past one makes
 * the worker a combiner: it takes every queued request up to the limit without waiting,
 * runs the child steps once, and completes them together, so the per-batch cost amortizes
 * with queue depth while each request's carried stall is still burned individually.
 * Affinity gives every worker its own queue and each submitting fiber a sticky worker -
 * the handler-per-connection server structure, where a slow request delays only its own
 * submitter. Runs to the deadline, then drains the queues and joins the workers. Keys:
 * workers, queue (capacity, default 1024), batch (combine limit, default 1), affinity.
 */
class PoolStep final : public Step
{
public:
    using Step::Step;

    void parseConfig(ConfigReader * config) override;

    /** True when submitters bind to one worker's queue. */
    bool hasAffinity() const { return affinity; }

    /** The calling fiber's sticky worker index, assigned round-robin on first use. */
    uint64_t bindWorker(ExecutionContext * context);

    /** Enqueue a request on a worker queue, parking when full; ECANCELED once the deadline drain began. */
    int submitRequest(PoolRequest * request, uint64_t workerIndex);

private:
    /** Worker fiber parameters. */
    struct WorkerTask
    {
        /** The pool the worker serves. */
        PoolStep * step;

        /** The queue the worker serves - shared, or its own under affinity. */
        FiberBlockingQueue<PoolRequest *> * queue;

        /** The executor merging the worker's context. */
        Executor * executor;

        /** Seed of the worker's random stream. */
        uint64_t seed;
    };

    void runStep(ExecutionContext * context) override;
    static int workerFiberMain(WorkerTask * task) noexcept;

    /** Worker fibers serving the queues. */
    uint64_t workers = 0;

    /** Requests a worker combines into one child-steps execution. */
    uint64_t batch = 1;

    /** Submitters bind to one worker's queue when set - sticky connection affinity. */
    bool affinity = false;

    /** Round-robin cursor of the next sticky worker binding. */
    std::atomic<uint64_t> bindingCursor = 0;

    /** Submitted requests awaiting a worker - one queue when shared, one per worker under affinity. */
    std::vector<std::unique_ptr<FiberBlockingQueue<PoolRequest *>>> queues;
};

void PoolStep::parseConfig(ConfigReader * config)
{
    parseCommon(config);
    parseChildren(config);
    requireChildren();

    workers = config->readUint64("workers");
    SILK_ASSERT(workers, "step %s: the worker count is zero", name.c_str());

    batch = config->readUint64Opt("batch").value_or(1);
    SILK_ASSERT(batch, "step %s: the batch limit is zero", name.c_str());

    affinity = config->readUint64Opt("affinity").value_or(0) != 0;

    uint64_t capacity = config->readUint64Opt("queue").value_or(1024);
    SILK_ASSERT(capacity, "step %s: the queue capacity is zero", name.c_str());

    uint64_t queueCount = affinity ? workers : 1;

    for (uint64_t i = 0; i < queueCount; ++i)
    {
        queues.push_back(std::make_unique<FiberBlockingQueue<PoolRequest *>>(capacity));
    }

    // The pool runs once to the deadline - it has no execution rate of its own; the
    // child steps count and measure per served request on the worker contexts.
    measure = false;
    selfCounting = true;
}

uint64_t PoolStep::bindWorker(ExecutionContext * context)
{
    return context->getWorkerBinding(this, &bindingCursor, workers);
}

int PoolStep::submitRequest(PoolRequest * request, uint64_t workerIndex)
{
    return queues[workerIndex]->enqueue(request);
}

void PoolStep::runStep(ExecutionContext * context)
{
    std::unique_ptr<FiberFuture[]> done = std::make_unique<FiberFuture[]>(workers);

    for (uint64_t i = 0; i < workers; ++i)
    {
        WorkerTask task{this, queues[affinity ? i : 0].get(), context->executor, context->rng()};
        int r = FiberScheduler::run(workerFiberMain, std::move(task), &done[i]);
        SILK_ASSERT(!r, "step %s: could not start a worker fiber: r=%d", name.c_str(), r);
    }

    // Hold to the deadline, then drain: the teardown serves the queued remainder to the
    // workers and completes their dequeues with ECANCELED once it runs dry.
    uint64_t nowCycles = Tsc::getCycles();

    if (nowCycles < context->deadlineCycles)
    {
        FiberScheduler::sleep(Tsc::cyclesToNanoseconds(context->deadlineCycles - nowCycles));
    }

    for (std::unique_ptr<FiberBlockingQueue<PoolRequest *>> & queue : queues)
    {
        queue->teardown();
    }

    for (uint64_t i = 0; i < workers; ++i)
    {
        int r = done[i].wait();
        SILK_ASSERT(!r, "step %s: a worker fiber failed: r=%d", name.c_str(), r);
    }
}

int PoolStep::workerFiberMain(WorkerTask * task) noexcept
{
    PoolStep * step = task->step;
    ExecutionContext context(task->executor, task->seed);
    std::unique_ptr<PoolRequest *[]> requests = std::make_unique<PoolRequest *[]>(step->batch);

    for (;;)
    {
        int r = task->queue->dequeue(&requests[0]);

        if (r)
        {
            break;
        }

        // Combine: take every queued request up to the batch limit without waiting.
        uint64_t count = 1;

        while (count < step->batch && task->queue->tryDequeue(&requests[count]))
        {
            ++count;
        }

        uint64_t stallCycles = 0;

        for (uint64_t i = 0; i < count; ++i)
        {
            stallCycles += requests[i]->stallCycles;
        }

        if (stallCycles)
        {
            uint64_t stallEndCycles = Tsc::getCycles() + stallCycles;

            while (Tsc::getCycles() < stallEndCycles)
            {
                cpuPause();
            }
        }

        step->executeChildren(&context);

        for (uint64_t i = 0; i < count; ++i)
        {
            requests[i]->done.set(0);
        }
    }

    task->executor->mergeContext(&context);
    return 0;
}

/**
 * Submits one request to a named pool and waits its completion - the client half of the
 * request handoff. A measured enclosing step records the full request latency: enqueue,
 * worker doorbell, execution, completion wake. Keys: pool (the pool step's dotted path).
 */
class SubmitStep final : public Step
{
public:
    using Step::Step;

    void parseConfig(ConfigReader * config) override;
    void resolve(const std::vector<Step *> & steps) override;

private:
    void runStep(ExecutionContext * context) override;

    /** The target pool's dotted config path. */
    std::string poolName;

    /** Request-carried stall in TSC cycles; zero submits a plain request. */
    uint64_t stallCycles = 0;

    /** Poisson rate of stall-marked requests per fiber, in Hz; zero marks every request carrying a stall. */
    uint64_t stallRateHz = 0;

    /** The target pool, linked by resolve. */
    PoolStep * pool = nullptr;
};

void SubmitStep::parseConfig(ConfigReader * config)
{
    parseCommon(config);
    requireLeaf(config);

    poolName = config->readString("pool");
    stallCycles = Tsc::nanosecondsToCycles(config->readDurationNsOpt("stall").value_or(0));
    stallRateHz = config->readUint64Opt("stall_rate").value_or(0);
}

void SubmitStep::resolve(const std::vector<Step *> & steps)
{
    for (Step * step : steps)
    {
        if (step->getName() == poolName)
        {
            bool isPool = step->getType() == "pool";
            SILK_ASSERT(isPool, "step %s: '%s' is not a pool", name.c_str(), poolName.c_str());
            pool = static_cast<PoolStep *>(step);
            return;
        }
    }

    SILK_FAIL("step %s: no pool named '%s'", name.c_str(), poolName.c_str());
}

void SubmitStep::runStep(ExecutionContext * context)
{
    // The request lives on this fiber's stack; the worker's completion set is its last
    // touch. A refused submit means the deadline drain began - nothing to wait for.
    PoolRequest request;
    request.stallCycles = stallCycles;

    if (stallRateHz && !context->stallDue(this, stallRateHz, Tsc::getCycles()))
    {
        request.stallCycles = 0;
    }

    uint64_t workerIndex = 0;

    if (pool->hasAffinity())
    {
        workerIndex = pool->bindWorker(context);
    }

    int r = pool->submitRequest(&request, workerIndex);

    if (r)
    {
        return;
    }

    request.done.wait();
}

//
// Factory.
//


std::unique_ptr<Step> createStep(std::string name, std::string type)
{
    if (type == "cpu")
    {
        return std::make_unique<CpuStep>(std::move(name), std::move(type));
    }

    if (type == "wait")
    {
        return std::make_unique<WaitStep>(std::move(name), std::move(type));
    }

    if (type == "yield")
    {
        return std::make_unique<YieldStep>(std::move(name), std::move(type));
    }

    if (type == "sequence")
    {
        return std::make_unique<SequenceStep>(std::move(name), std::move(type));
    }

    if (type == "loop")
    {
        return std::make_unique<LoopStep>(std::move(name), std::move(type));
    }

    if (type == "parallel")
    {
        return std::make_unique<ParallelStep>(std::move(name), std::move(type));
    }

    if (type == "concurrent")
    {
        return std::make_unique<ConcurrentStep>(std::move(name), std::move(type));
    }

    if (type == "maybe")
    {
        return std::make_unique<MaybeStep>(std::move(name), std::move(type));
    }

    if (type == "chain")
    {
        return std::make_unique<ChainStep>(std::move(name), std::move(type));
    }

    if (type == "pool")
    {
        return std::make_unique<PoolStep>(std::move(name), std::move(type));
    }

    if (type == "submit")
    {
        return std::make_unique<SubmitStep>(std::move(name), std::move(type));
    }

    SILK_FAIL("unknown step type '%s' for step %s", type.c_str(), name.c_str());
}

std::unique_ptr<Step> makePipeline()
{
    return std::make_unique<SequenceStep>(std::string(), "sequence");
}

} // namespace silk
