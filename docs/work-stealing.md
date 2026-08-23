# Work-stealing under stall load

## Goal

Show empirically what work-stealing buys you: when a fraction of requests are compute-bound (server-side stall), can silk's per-CPU scheduler keep throughput up by redistributing the *other* connections sharing a stalled CPU, where epoll's per-thread reactor cannot.

## Mechanism

A `--stall-rate <Hz>` + `--stall-duration <us|ms>` knob is provided on all three engines (`net-perf`, `net-perf-asio`, `net-perf-epoll`). Wire format: each message's first 4 bytes are a `uint32_t stall_ns`; the server reads the prefix and busy-loops via RDTSC for that duration before echoing. The client schedules stalls per connection as independent Poisson processes (`std::exponential_distribution` inter-arrivals, seeded by connection address). All three implementations share the same wire format and `busyLoopForStall` helper in `src/perf/common.h` so the comparison is apples-to-apples.

Stall is a busy-loop, not a sleep, so it models compute-bound work (a slow query, JSON parse, regex). On the server side it pins the executor for its duration: silk's scheduler thread for one CPU holds; asio's thread executor blocks; epoll's thread spins.

## Per-CPU stall load

Server pinned to CPUs 0-15 (16 distinct physical cores); client uses 16-31 (HT siblings of the server cores). Per-CPU stall load:

```
load% = (connections × stall_rate × stall_duration) / 16_cores
```

| config | load |
|---|---|
| 256 conns × 10Hz × 1ms | 16% |
| 256 conns × 100Hz × 1ms | 160% (over capacity) |
| 1024 conns × 10Hz × 100us | 6.4% |
| 1024 conns × 10Hz × 1ms | 64% |
| 1024 conns × 10Hz × 10ms | 640% (way over) |

## Canonical results (10-minute runs, 30s warmup)

The silk rows are measured with the processor prefix active: the shrink signal counts a wait rewarded by arriving work as demand, so stall-heavy load holds the width and the stall rows measure the same full prefix as the no-stall row (`scheduler.md`). Each silk stall row is still one draw from a warmup-locked mode distribution - the 64% row has been observed from 845k to 1576k across runs - while the epoll and asio rows repeat tightly.

| config | engine | RPS | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|
| 1024 baseline | silk | 2054k | 452 | 750 | 1818 | **2768** |
| 1024 baseline | epoll | 2387k | 425 | 542 | 567 | **650** |
| 1024 baseline | asio | 408k | 2569 | 2668 | 2702 | **2748** |
| 1024 × 10Hz × 1ms (64%) | silk | 1333k | 166 | 1354 | 5667 | **22790** |
| 1024 × 10Hz × 1ms (64%) | epoll | 938k | 434 | 3412 | 5404 | **7501** |
| 1024 × 10Hz × 1ms (64%) | asio | 411k | 2439 | 2978 | 3501 | **3933** |
| 256 × 100Hz × 1ms (160%) | silk | 862k | 32 | 61 | 1072 | **147290** |
| 256 × 100Hz × 1ms (160%) | epoll | 85k | 59 | 15118 | 20125 | **21147** |
| 256 × 100Hz × 1ms (160%) | asio | 24k | 10581 | 11654 | 12033 | **12397** |

Per-stage breakdown at 64% load (silk client, 60s with --print-counters):

| span | p99 | p99.9 | meaning |
|---|---|---|---|
| `io_wait` | 8.2 ms | 25.7 ms | full silk-side IO wait |
| `sq_wait` | 8.2 ms | 27.6 ms | SQE pending in silk's SQ ring before flush to kernel |
| `cq_wait` | 61 µs | 115 µs | CQE-in-ring sitting upper bound |
| `submit_io` | 29 µs | 62 µs | io_uring_submit syscall cost |
| `ready_wait` | 22 µs | 258 µs | enqueueReady -> dispatch |

## What silk wins

- **Throughput under stalls.** ~1.4x at 64% (1333k vs 938k req/s) and ~10x at 160% (862k vs 85k). Per-CPU stall load on silk is averaged across all CPUs via work-stealing plus CQ-draining-by-stealer; epoll's per-thread isolation leaves each reactor to eat its own stalls, and it silently piles up.
- **The latency body at saturation.** At 160% silk holds p50 32 µs and p95 61 µs against epoll's 59 µs and 15 ms -- the shared deficit shows up only past p99, where epoll's whole distribution has already collapsed.

## Where silk still trails

- **p99.9 under stalls.** silk 23 ms vs epoll 7.5 ms at 64% (~3x), 147 ms vs 21 ms at 160% (~7x) -- stall-driven dispatch jitter accumulates across many fibers in a way per-thread reactors avoid, and the throughput win under stalls comes with the worse worst case.
- **Baseline.** silk is -14% RPS (2054k vs 2387k) with p99.9 2.8 ms vs epoll's 0.65 ms at 1024 connections -- the fiber dispatch loop has structurally more tail than a per-thread epoll reactor.
- **p99 at light stall load.** At 6.4% load (1024 × 10Hz × 100us) silk is worse than epoll on p99 (6.0 ms vs 0.7 ms in this draw) -- the stealing path doesn't fire often enough to offset silk's higher per-fiber overhead.

## The load-imbalance tail

**Anatomy of the p99.9 episodes.** Connection setup homes every fiber on the spawning processor, and stealing diffuses the clump only partially, leaving persistent homes of hundreds of connections - about one full CPU of message work with no headroom. Roughly once per run a server stall bunches that processor's responses, it falls behind its wave, its sends go silent for up to a second, and the whole window then releases at once - a relaxation oscillation that takes hundreds of milliseconds to damp. During an episode every CPU is busy, so no wake mechanism applies: nothing is asleep to wake.

**Timed parks everywhere mask the mechanics.** The convoys form regardless of park policy; a full-width fleet of capped parks bulk-drains them within milliseconds, clipping episodes at ~15 ms, while a narrowed prefix lets them run ~300 ms and bounds the extreme tail lower.

**The causal bound.** Homing new fibers round-robin instead of spawner-local kills the clumps at birth - p99.9 collapses and the run-to-run variance disappears - but throughput halves, because uniform static partitioning loses the pooling that spawner-local homes plus stealing provide. Spawner-local homing with stealing is the throughput-correct choice; the tail is its price.

## Counter evidence at three load points

`silk + --print-counters`, illustrating where the throughput win comes from:

| load | RPS | FiberStolen | SchedulerThreadParked | SchedulerIdleTime |
|---|---|---|---|---|
| baseline | 1947k | 16,758 | 392k | 33s (3%) |
| 64% (1024 × 10Hz × 1ms) | 804k | 15,709 | 12k | 33s (3%) |
| 160% (256 × 100Hz × 1ms) | 901k | 1,384 | 881k | 22s (2%) |

Idle time sits near 3% at every load - the width tracks demand. The parked column is the stall profile, not idleness: the 64% fleet is busy and barely parks, while the 160% fleet parks deadline-capped between stall expiries. `FiberStolen` collapses at saturation because the steal loop skips victims attended within the spin horizon; the saturation throughput win is structural spreading of stall load across the fleet (an idle scheduler thread drains a stalled core's CQ by claiming its service loop), where epoll's per-thread reactor leaves a stalled thread's 64 connections starved until the stall ends - moving fibers is the minor term.

## Lower-rate stall sweep

Sweep of stall patterns at 1024 connections, holding per-CPU offered load roughly constant. Each row is a single 60s run, warmup 10s:

| pattern | per-CPU load | engine | RPS | p50 | p99 | p99.9 |
|---|---|---|---:|---:|---:|---:|
| 1 Hz × 10 ms | 64% | silk | 883k | 308 µs | 10687 µs | 90674 µs |
| 1 Hz × 10 ms | 64% | epoll | 936k | 380 µs | 20353 µs | 40433 µs |
| 5 Hz × 2 ms | 64% | silk | 826k | 416 µs | 10669 µs | 28764 µs |
| 5 Hz × 2 ms | 64% | epoll | 935k | 411 µs | 6450 µs | 10470 µs |
| 10 Hz × 100 µs | 6.4% | silk | 1986k | 298 µs | 5969 µs | 6701 µs |
| 10 Hz × 100 µs | 6.4% | epoll | 2243k | 438 µs | **695 µs** | **894 µs** |
| 100 Hz × 100 µs | 64% | silk | 1525k | 52 µs | 29610 µs | 64681 µs |
| 100 Hz × 100 µs | 64% | epoll | 996k | 989 µs | 2089 µs | 2522 µs |

Two findings:

- **At 6.4% offered load (10 Hz × 100 µs), epoll wins p99** (695 µs vs 6.0 ms in this draw). silk's per-fiber overhead dominates at light load and the stealer fires too rarely to amortize it. There is no regime where silk pulls ahead before saturation -- below saturation, epoll's lower per-request cost wins.
- **A single silk draw at a stall row is a mode sample, not a stable mean.** Warmup seeds the pipeline into one of several locked modes (the known limit in `scheduler.md`), so silk's 64% rows range from -12% (5 Hz × 2 ms) to +53% (100 Hz × 100 µs) against epoll across draws; epoll's rows repeat tightly.
- **The 100 Hz × 100 µs row exposes silk's tail weakness under high stall frequency**: p99.9 64 ms vs epoll 4.1 ms. Each individual stall is short, but the dispatch jitter from constantly resuming fibers is substantial.

## Reproducing

```
# Headline cases:
./bb -b release net-perf --connections 1024 --duration 60s --warmup 10s \
    --stall-rate 10 --stall-duration 1ms                 # silk @ 64% load

./bb -b release net-perf-epoll --connections 1024 --duration 60s --warmup 10s \
    --stall-rate 10 --stall-duration 1ms                 # epoll @ 64% load

./bb -b release net-perf --connections 256 --duration 60s --warmup 10s \
    --stall-rate 100 --stall-duration 1ms                # silk @ 160% load

# Counters (FiberStolen jump is the proof of mechanism):
./bb -b release net-perf --connections 1024 --duration 60s --warmup 10s \
    --print-counters --stall-rate 100 --stall-duration 1ms
```

Canonical numbers in this document are 10-minute runs (`--duration 600s --warmup 30s`).
