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

| config | engine | RPS | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|
| 1024 baseline | silk | 2152k | 466 | 664 | 767 | **1776** |
| 1024 baseline | epoll | 2474k | 419 | 472 | 493 | **564** |
| 1024 baseline | asio | 368k | 2786 | 2871 | 2908 | **2953** |
| 1024 × 10Hz × 1ms (64%) | silk | 1641k | 74 | 234 | 1226 | **59384** |
| 1024 × 10Hz × 1ms (64%) | epoll | 970k | 403 | 3411 | 5488 | **9483** |
| 1024 × 10Hz × 1ms (64%) | asio | 381k | 2631 | 3180 | 3724 | **4206** |
| 256 × 100Hz × 1ms (160%) | silk | 671k | 35 | 1032 | 4088 | **83816** |
| 256 × 100Hz × 1ms (160%) | epoll | 53k | 2075 | 17127 | 21139 | **22166** |
| 256 × 100Hz × 1ms (160%) | asio | 24k | 10574 | 11651 | 12028 | **12380** |

Per-stage breakdown at 64% load (silk client, 60s with --print-counters):

| span | p99 | p99.9 | meaning |
|---|---|---|---|
| `io_wait` | 1.7 ms | 4.3 ms | full silk-side IO wait |
| `sq_wait` | 1.7 ms | 4.3 ms | SQE pending in silk's SQ ring before flush to kernel |
| `cq_wait` | 58 µs | 65 µs | CQE-in-ring sitting upper bound |
| `submit_io` | 43 µs | 64 µs | io_uring_submit syscall cost |
| `ready_wait` | 7 µs | 14 µs | enqueueReady -> dispatch |

## What silk wins

- **Throughput under stalls.** ~1.7x at 64% load (1641k vs 970k req/s), ~12.7x at 160% (671k vs 53k). Per-CPU stall load on silk is averaged across all CPUs via work-stealing plus CQ-draining-by-stealer; on epoll it's per-thread isolation.
- **p99 under stall load.** silk 1.2 ms vs epoll 5.5 ms at 64% -- silk wins by ~4.5x. At 160% silk 4.1 ms vs epoll 21 ms -- silk wins by ~5x.
- **Graceful degradation.** At 160% offered load epoll silently piles up; silk completes ~12.7x more requests per second by best-effort sharing the deficit across CPUs.

## Where silk still trails

- **p99.9 at moderate stall load.** silk 59 ms vs epoll 9.5 ms at 64% load -- silk is **~6.2x** epoll's tail. Stall-driven dispatch jitter accumulates across many fibers in a way per-thread reactors avoid.
- **p99.9 at saturation.** silk 84 ms vs epoll 22 ms at 160% load -- silk's throughput win comes with a ~3.8x worse worst-case tail.
- **Baseline p99.9.** Even with no stall load, silk's p99.9 is 1.8 ms vs epoll's 0.6 ms at 1024 connections -- the fiber dispatch loop has structurally more tail than a per-thread epoll reactor.
- **p99 at light stall load.** At 6.4% load (1024 × 10Hz × 100us) silk is worse than epoll on p99 (2.1 ms vs 0.7 ms) -- the stealing path doesn't fire often enough to offset silk's higher per-fiber overhead.

## Counter evidence at three load points

`silk + --print-counters`, illustrating where the throughput win comes from:

| load | RPS | FiberStolen | SchedulerThreadParked | SchedulerIdleTime |
|---|---|---|---|---|
| baseline | 2072k | 1,748 | 855k | 202s (21%) |
| 64% (1024 × 10Hz × 1ms) | 1512k | 4,661 | 11.3M | 271s (28%) |
| 160% (256 × 100Hz × 1ms) | 970k | **196,210** | 25.9M | 424s (44%) |

`FiberStolen` rises ~112x at the saturation row but only 2.7x at 64% load. So the throughput win at 64% load is **not primarily from stealing** -- it's from silk's structural ability to spread stall load across all 16 cores (silk's scheduler thread idle on CPU N can drain CPU M's CQ via `runStealLoop`'s `runServiceLoop(victim)` call), where epoll's per-thread reactor leaves a stalled thread's 64 connections starved until the stall ends. Work-stealing is doing what it claims to do; its meaningful contribution is at saturation, not mid-load.


## Lower-rate stall sweep

Sweep of stall patterns at 1024 connections, holding per-CPU offered load roughly constant. Each row is a single 60s run, warmup 10s:

| pattern | per-CPU load | engine | RPS | p50 | p99 | p99.9 |
|---|---|---|---:|---:|---:|---:|
| 1 Hz × 10 ms | 64% | silk | 1465k | 111 µs | 10084 µs | 10422 µs |
| 1 Hz × 10 ms | 64% | epoll | 995k | 370 µs | 10486 µs | 30464 µs |
| 5 Hz × 2 ms | 64% | silk | 1591k | 71 µs | 2444 µs | 8468 µs |
| 5 Hz × 2 ms | 64% | epoll | 996k | 374 µs | 6478 µs | 12460 µs |
| 10 Hz × 100 µs | 6.4% | silk | 2117k | 302 µs | 2079 µs | 2380 µs |
| 10 Hz × 100 µs | 6.4% | epoll | 2409k | 408 µs | **678 µs** | **847 µs** |
| 100 Hz × 100 µs | 64% | silk | 1620k | 115 µs | 15364 µs | 63992 µs |
| 100 Hz × 100 µs | 64% | epoll | 1055k | 896 µs | 2184 µs | 4116 µs |

Two findings:

- **At 6.4% offered load (10 Hz × 100 µs), epoll wins p99 by 3x** (678 µs vs 2079 µs). silk's per-fiber overhead dominates at light load and the stealer fires too rarely to amortize it. There is no regime where silk pulls ahead before saturation -- below saturation, epoll's lower per-request cost wins.
- **Throughput edge to silk is consistent under any meaningful stall load** (47-65% more RPS at 64% load). The shape of the stalls (rare-long vs frequent-short) shifts silk's p99 distribution but not its throughput advantage.
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
