# Fiber Workload Simulator

## Purpose

`fibers-simulator` (`src/perf/simulator/`) runs a synthetic fiber workload described by a pipeline config file, so scheduler behavior can be studied against controlled load shapes without a network stack: closed loops of dependent wakes, micro-bursts, fan-out/fan-in, injected compute stalls, and stepped load levels. It reproduces the scheduling patterns behind the `net-perf` scenarios (`work-stealing.md`) and the amber commit pipeline as pure fiber workloads, and pairs each run with the scheduler's latency histograms and counters (`--print-counters`) so a scheduling change can be evaluated scenario by scenario.

**Runs go through `bb`.** `./bb -b release simulator <config> [--param name=value] [--duration D] [--warmup D] [--cpus LIST] [--print-counters] [--flamegraph]`, where `<config>` is a file path or the name of a bundled config under `src/perf/simulator/configs/`. The full option table is in `README.md`. Throughput and latency numbers are only meaningful from a `release` build, and `--cpus 0-15` pins the run to the 16 physical cores the bundled configs' load math assumes.

## Config format

**A config is a brace-nested text file** - one statement per line, `#` starts a comment - with two top-level sections: `params` and `pipeline`.

```
params {
    duration = 10s
    warmup = 2s
    fibers = 200
}

pipeline {
    clients {
        type = parallel
        count = $fibers

        request {
            type = loop
            measure = 1

            think {
                type = wait
                duration = 100us
                exponential = 1
            }

            work {
                type = cpu
                duration = 2us
            }
        }
    }
}
```

**The `params` section declares every knob of the config with its default.** The run settings `duration`, `warmup`, and `seed` are read from it directly; every other entry is a placeholder referenced as `$name` inside pipeline values. `--param name=value` (repeatable, comma-separated pairs allowed) overrides a declared default, so one config file covers a whole family of measured cases. Durations take a `ns` / `us` / `ms` / `s` / `m` suffix; a bare number is seconds.

**The `pipeline` section is a tree of named steps.** Every step section carries a `type` key; the section name becomes the step's dotted path in the report (`clients.request.work`). Composite steps execute their child sections in file order.

**Config typos abort the run.** A key or section nothing ever read, a `$name` without a declaration, an override naming no declared param, and a declared param no value references are all reported as errors, so a misspelled knob cannot silently fall back to a default.

## Step types

| Type | Keys | Behavior |
|---|---|---|
| `cpu` | `duration` (or `cycles`) | Busy-loops the TSC for the given time - the compute phase of a request, or an injected stall |
| `wait` | `duration`, `exponential` | Suspends the fiber for a sleep; `exponential = 1` draws each sleep from Exp with that mean, turning a loop into a Poisson process |
| `yield` | `count` | Reschedules the fiber through the ready queue |
| `sequence` | - | Executes the children in order on the calling fiber; the implicit pipeline root |
| `loop` | `count`, `duration`, `period` | Repeats the children: `count` iterations (absent - until the run deadline), `duration` caps the loop's own run time, `period` paces iteration starts for a constant arrival rate |
| `parallel` | `count` | Fans out `count` fibers, each executing the children once, and joins them all |
| `concurrent` | - | Runs each child on its own fiber and joins them - heterogeneous workloads side by side |
| `chain` | `length`, `rounds` | A ring of `length` fibers passing one token: each hop waits, executes the children, and wakes the next hop; `rounds` bounds the run (absent - until the deadline), and an empty body is a pure wake ring |
| `maybe` | `probability` | Executes the children with the given probability per execution - rare events like stalls |
| `pool` | `workers`, `queue`, `batch`, `affinity` | A pool of worker fibers serving submitted requests: dequeue, execute the children, complete the request; a `batch` limit past one makes the worker a combiner - it takes every queued request up to the limit, runs the children once, and completes them together; `affinity = 1` gives every worker its own queue and each submitting fiber a sticky worker, the handler-per-connection server structure; drains and joins at the deadline |
| `submit` | `pool`, `stall`, `stall_rate` | Submits one request to the named pool and waits its completion - the client half of a real cross-fiber handoff; `stall` rides the request and the worker burns it before the body (per request even in a combined batch), and `stall_rate` marks requests from a per-fiber Poisson clock at that rate instead of every one |

**`parallel` reproduces connection clumping.** The spawned fibers home on the spawning fiber's processor, the way accepted connections home on the acceptor, so a burst lands as a clump and spreads only by stealing.

**`chain` is the closed dependent-wake loop.** Its queueing shows up as suspend wait, never as backlog age - the load shape a demand signal based on queue depth cannot see.

**`pool` plus `submit` is the real request path.** A submit enqueues onto the pool's queue, a worker's doorbell fires, the worker executes the pool's children, and the completion wakes the submitter - every hop a genuine fiber switch. The client loop is closed - a submitter waits its completion - so saturation collapses the rate the way a real pipeline does. Size `queue` at or above the submitting fiber count: a parked enqueue on a full queue is admission-order unfair and shows as an extreme latency tail. Under `affinity` a slow request delays only its own submitter's stream - a shared queue would convoy every submitter behind it - and cpu-level interference between workers is left to the scheduler, whose clump-spreading is then the thing being measured.

**`measure = 1` on any step records wall-time percentiles - the request latency.** A measured step's sample is the full chain from its first child to its last: the work, the think-time waits, and (through `submit`) the fiber handoffs to a pool and back. A plain step measures each execution; `loop` measures per iteration (the request latency of a client loop) and `chain` per round (the full ring traversal), and both count their executions at that same granularity. Executions and samples are counted only after the warmup ends.

## Execution model

**The executor runs the pipeline root on one fiber and lets composites fan out from there** (`src/perf/simulator/executor.cpp`). The run deadline and warmup end are TSC stamps fixed at start; loops and chains poll the deadline and drain naturally, so a run ends by joining every spawned fiber - nothing is force-killed, and a run overshoots the deadline by at most one in-flight iteration.

**Every fiber executes against its own context and merges once.** An `ExecutionContext` carries a per-fiber deterministic random stream - child seeds derive from the parent's stream, so a run is reproducible for a given `seed` param - plus local sinks for execution counts and latency samples. A completed fiber folds its context into the shared per-step totals under a single `FiberMutex`, so the hot path stays free of shared counters and the merge suspends the fiber, not the scheduler thread.

## Report

**The binary prints one JSON object**: the config path, the applied `--param` overrides, `duration_s`, `measured_s` (the post-warmup window), and per-step entries with type, executions, rate, and `latency_us` percentiles (count, avg, p50, p90, p99, p999, max) for measured steps. With `--print-counters` it appends the scheduler latency histograms and the named scheduler counters, and `bb` renders the steps as a table before the counter dump.

**Rates divide by the whole measured window.** A step that runs for only part of the run - a phase loop inside a sequence - reads correctly by its execution count, but its printed rate is averaged over the full window, not over the phase.

## Bundled scenarios

| Config | Exercises |
|---|---|
| `net-baseline` | 1024 echo-like connection loops at ~2M msg/s - steady moderate load with clumped homes |
| `net-stall` | The `work-stealing.md` stall matrix, closed-loop over an affinity `pool` with one worker per connection - the real server's handler structure: one echo loop per connection, stalls ride the requests from a per-fiber Poisson clock (`stall_rate`); on the real 16-cpu budget the baseline reads within 2% of real, the 64% row lands near the top of the real draw band, and the over-capacity 160% row reads inside the real draw range with the real signature - a fast body under rare long stall episodes |
| `chain` | 8 rings of 64 fibers, 500ns per hop - closed dependent-wake loops |
| `micro-burst` | 256 fibers of 10us fanned out every 2ms - clump spreading and park/unpark churn |
| `fan-out` | 16 request loops each scattering 32 subtasks and joining - join-latency tails |
| `phased` | 5s heavy / 5s near-idle / 5s heavy - width shed on the drop and regrow on the return |
| `mixed` | Rings, bursts, and a stalling background concurrently |
| `pool` | 256 client loops calling a 16-worker pool - measured end-to-end request latency across real fiber handoffs, closed-loop |
| `amber` | The amber master commit write path at the inproc tier, closed-loop over combiner pools |

**The amber config closes the commit loop over combiner pools.** Transfer fibers run the engine work split around two commit handoffs: a prepare submit into the 3-worker log-shard pool and a durable submit into the single-worker strict-LSN collector, whose completion wake is the durable release. Both pools combine batches, so the per-batch stage costs amortize with in-flight depth and the commit park lengths emerge from queue depth - one parameter set covers every fiber count, no per-case calibration. The collector's 4us carried per record is the serial cost that sets the plateau, fitted to the real 245K ceiling and the 36.4us single-fiber chain; the delivery / apply stages and the periodic ticks stay timer-paced off the commit path. Against the five measured counts (1, 32, 200, 400, 800 fibers - 27.5K to 245K tx/s, ready-made `--param` lines in the config header) the model reads 34.6K / 178K / 211K / 210K / 210K: within 15% from 32 fibers up, fast by 26% at one fiber, and flat where the real plateau still creeps from 219K to 245K.

## Modeling limits

- **A `wait` wake comes from a timer, not another fiber.** The wakeup is a sleep expiry handled on the fiber's home processor - it exercises park/unpark and timer dispatch, not cross-processor doorbells or wake chains. `chain` and `pool` / `submit` are the exceptions and model dependent wakes exactly.
- **Collective releases come only from combiner pools.** A `batch` pool completes a whole batch at once - the wake clustering of a sequencer watermark or a batched future `setAll` - but the release of timer-modeled waiters is still approximated by independent per-fiber waits, so wake clustering stays under-represented in the fan-in shapes.
- **Timer-modeled configs are open-loop.** Where offered load is set by `wait` rates rather than `submit` completions, saturation shows as latency growth rather than the closed-loop rate collapse a real pipeline exhibits; calibrating waits per case is the workaround, and a `pool`-based shape closes the loop outright.
- **Latency floors include the sleep machinery.** A measured step's wall time includes sleep-expiry and wake latency, so sub-10us wait modeling is dominated by scheduler overhead rather than the configured duration.
