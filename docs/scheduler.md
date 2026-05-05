# Fiber Scheduler Design

## Overview

This is a cooperative fiber scheduler with per-CPU threads and io_uring
integration. Each CPU runs one scheduler thread that owns its own io_uring ring,
ready queue, sleep tree, and wakeup signaling infrastructure.

Source lives under `src/fibers/`. Data structures and utilities are in
`src/util/` (see `docs/util.md`). Synchronization primitives are in
`docs/sync.md`.

---

## Scheduler Loop

`FiberScheduler` runs one scheduler thread per CPU, each owning:

- An io_uring ring (128 entries) for async IO
- An eventfd for cross-CPU wakeup signaling
- A per-CPU ready queue (`BoundedQueue`) of runnable fibers
- A sleep tree (`SleepTree`) ordered by TSC deadline
- A sleep inbox and cancel inbox (`LockFreeStack`) for lock-free handoff

Each iteration always runs both `handleReadyQueue` and `runServiceLoop`,
regardless of whether the first found work. Any work resets the idle counters
and immediately continues the loop.

If the own CPU has nothing to do, `runStealLoop` is attempted next. If all
three produce nothing, the thread enters the spin/sleep phase.

When idle, `parkThread` calls `io_uring_enter2` with a `waitNs` timeout; cross-CPU wakeup is delivered by writing to an eventfd that is polled persistently in the ring (`IORING_POLL_ADD_MULTI`), producing a CQE that wakes `io_uring_enter2`.

---

## Context Switching

Context switching uses Boost.Context (`fcontext_t`). Each fiber has a 64 KB
mmap'd stack with a guard page at each end (bottom and top). ASan stack-swap
annotations are included.

Fiber lifecycle: `SUSPENDED -> READY -> RUNNING -> STOPPED`.
`SUSPEND_REQUESTED` is a transient state entered at the start of `suspend`;
the suspend callback runs while the fiber is in this state, then the fiber
transitions to either `SUSPENDED` (genuine suspension) or `READY` (if
`schedule` was called during the callback, cancelling the suspension).

`FiberState` is defined in `fiber.cpp` (not `fiber.h`) as an implementation detail.
`SleepFuture`, `SleepStack`, and `SleepTree` are declared/aliased in the private
section of `FiberScheduler` (in `fiber.h`) so they are accessible from `fiber.cpp`.

`processorNumber` tracks which CPU's ready queue the fiber targets. It is set by
`schedule()` on first dispatch (assigned to the current CPU when still
`UINT32_MAX`) and updated by `runStealLoop` when a fiber is stolen (reassigned
to the stealing CPU). When a fiber runs on a worker thread via thread mode,
`processorNumber` is not updated - the fiber returns to its last assigned CPU's
queue on `exitThreadMode()`.

---

## Suspension Pattern

`FiberScheduler::suspend(callback, ctx)` suspends the current fiber and invokes
`callback` while the fiber is parked. The callback is responsible for arranging
the wakeup (e.g. enqueuing as a waiter). The callback must handle the race where
the event already arrived before parking.

The callback and its context are stored on the `Fiber` itself
(`suspendCallback`/`suspendContext`) and cleared by `runFiber` after invocation.
This allows `runFiber` to be called from both scheduler threads and thread pool
workers without per-CPU state.

---

## Thread Mode

A fiber that needs to make blocking syscalls or perform heavy CPU work can
escape the cooperative scheduler without stalling its scheduler thread by
entering thread mode.

```cpp
{
    FiberScheduler::ThreadModeScope scope; // enterThreadMode()
    // blocking work here -- runs on a thread pool worker
}                                          // exitThreadMode()
```

There are two ready queues:

- **Per-CPU ready queue** (`ProcessorState::readyQueue`) - bounded MPMC queue
  drained by the CPU's scheduler thread. Normal cooperative fibers live here.
- **Shared ready queue** (`SchedulerState::readyQueue`) - unbounded MPMC queue
  drained by the worker thread pool. Thread-mode fibers live here, and normal
  fibers overflow here when a CPU ready queue is full.

`enterThreadMode()` sets `fiber->inThreadMode` and calls `schedule()`, which
routes the fiber to the shared ready queue. A worker thread dequeues it and runs
it via `runFiber(nullptr, fiber)`, where it may block freely. When the fiber
suspends inside thread mode (e.g. waiting on a future), `schedule()` re-enqueues
it to the shared ready queue when it is woken - any free worker picks it up.

`exitThreadMode()` clears `fiber->inThreadMode` and calls `schedule()`, which
routes the fiber back to its CPU's per-CPU ready queue. After this point the
fiber runs cooperatively again.

`schedule()` routing summary:
- `inThreadMode = true` => shared ready queue
- `inThreadMode = false`, CPU ready queue not full => CPU ready queue
- `inThreadMode = false`, CPU ready queue full => shared ready queue (overflow)

---

## Synchronization Primitives

`FiberMutex`, `FiberFutex`, `FiberSequencer`, `FiberEvent`, and
`FairFiberMutex` are documented in `docs/sync.md`.

`FiberMutex` and `FiberFutex` use the scheduler's waiter table directly
(`enqueueWaiter` / `releaseWaiters`, keyed by `this`). The higher-level
primitives build on `FiberSequencer`, which manages its own ordered waiter tree
via a combiner lock.

---

## Data Structures

The scheduler uses `LockFreeStack`, `BoundedQueue`, `Queue`, `Tree`, and
`MemoryPool` from `src/util/`. See `docs/util.md` for full descriptions.

Key usages:
- `MemoryPool` - fiber allocation (rseq fast path, zero atomics)
- `BoundedQueue` - per-CPU ready queue (MPMC, fixed capacity)
- `LockFreeStack` - sleep inbox, cancel inbox
- `Tree` - sleep deadline ordering (keyed by TSC deadline)

---

## Timing

All sleep deadlines are in TSC cycles. `Tsc::getCycles()` is `rdtsc` /
`cntvct_el0`. Conversion uses fixed-point multiply-shift
(`cycles * nsPerCycleFp >> 20`) - no division on the hot path. Frequency is
detected once at startup via CPUID / hypervisor leaves / `cntfrq_el0`. See
`docs/util.md` for details.

---

## Async IO

Each of `read`, `write`, and `poll` has two overloads: a blocking form that
submits an io_uring SQE and suspends the fiber until completion, and an async
form that submits the SQE and returns immediately with an `IoFuture*` the caller
waits on separately. `handleCompletionQueue` processes CQEs, extracts the
`IoFuture*` from the CQE user data, and calls `future->set()` to wake the
waiting fiber.

---

## Sleep Cancellation

`SleepFuture` has an atomic `state` with two bits: `IN_TABLE` (entry is in the
sleep tree) and `CANCELLED`. `cancelSleep` does a `fetch_or(CANCELLED)` -- if
`IN_TABLE` was set, it also pushes the entry onto `cancelQueue`.
`handleCancelQueue` drains `cancelQueue` and calls `sleepTree.remove()` directly
(O(log n), no scan).

The `StackEntry` inside `SleepFuture` is shared between `sleepQueue` and
`cancelQueue` - only one can hold it at a time, enforced by the `IN_TABLE` flag.

---

## Work-Stealing

An idle CPU can steal fibers from a neighbor's ready queue and claim a
neighbor's frozen service loop to process its pending completions and
expirations. Without stealing, load imbalance is permanent and service loop
starvation occurs while a scheduler thread is inside a fiber.

### Service Loop Claiming

`runServiceLoop` processes io_uring CQEs (`handleCompletionQueue`), the sleep
inbox (`handleSleepQueue`), cancellation requests (`handleCancelQueue`), and
expired waiters (`handleExpiredWaiters`), then calls `enqueueWakeup` to arm the
next sleep deadline. It is protected by `serviceLoopLock` so at most one thread
runs the service loop for a given processor at a time.

- **Owner thread**: try-locks before running its own service loop. If a helper
  holds the lock, the owner skips it entirely - the helper is already doing the
  work.
- **Helper thread**: try-locks the victim's lock; if it fails, moves to the next
  candidate.

`enqueueWakeup` runs inside the locked section to eliminate a race where a helper could concurrently modify `sleepTree` while the owner armed the next deadline.

`enqueueWakeup` is excluded from the `didWork` return value - it is preparation
for sleeping, not productive work. Counting it would cause a spin loop.

Fibers woken by a claimed service loop are enqueued via `schedule()` using
`fiber->processorNumber`, routing them back to the victim's ready queue.

### Fiber Stealing

An idle CPU dequeues fibers from a neighbor's `readyQueue` and runs them
locally. `BoundedQueue` is MPMC, so `victim->readyQueue.dequeue()` is safe from
any thread.

### Victim Selection

`buildStealCandidates()` runs once at `initialize()`. For each active CPU it
builds a `stealCandidates[]` array sorted by estimated steal cost (cheapest
first). Topology is read from sysfs:

- `/sys/devices/system/cpu/cpuN/topology/core_id`
- `/sys/devices/system/cpu/cpuN/topology/physical_package_id`
- `/sys/devices/system/node/nodeN/cpulist` (NUMA)

Cost tiers:

| Distance | Cost threshold |
|---|---|
| HT sibling | ~1 us |
| Same socket | ~50 us |
| Cross-socket | ~500 us |

Thresholds reflect cache warming cost: fiber stealing moves potentially hundreds
of KB of stack and heap data, while service loop claiming touches a bounded ~2 KB
io_uring CQ. Within each tier, candidates are shuffled randomly (seeded by CPU
number) to spread load. Inactive CPUs get `UINT64_MAX` cost and sort to the end.
If sysfs is unavailable, all active CPUs get the same fallback cost.

### Steal Loop

`runStealLoop` computes `idleCycles = now - idleSinceCycles` and a budget
deadline of `now + idleCycles`. It walks `stealCandidates[]` cheapest first,
breaking immediately if `idleCycles < candidate->costCycles`. For each
candidate it:

1. Calls `runServiceLoop(victim)` - the `try_lock` handles concurrency.
2. Drains `victim->readyQueue` via repeated `dequeue()`, calling `runFiber` for
   each fiber, until the queue is empty or the budget deadline passes.

The deadline is shared across all candidates and all fiber steals, bounding
total steal time to `idleCycles`.

### Idle Progression

- `waitNs` starts at `INITIAL_WAIT_NS` (1 us) and doubles each idle iteration
  up to `MAX_WAIT_NS` (10 ms).
- While `waitNs < SPIN_THRESHOLD_NS` (20 us): `spinWait()` runs 64 x
  `cpuPause()` iterations (~2 us) checking `hasWork()`.
- Once `waitNs >= SPIN_THRESHOLD_NS`: `parkThread` calls `io_uring_enter2` with a `waitNs` timeout. The timed wait ensures sleeping CPUs wake periodically to attempt stealing even without an explicit wakeup.

Any productive iteration resets `waitNs` back to `INITIAL_WAIT_NS`.

---

## Performance

Measurements on a 32-CPU x86 machine (Intel Xeon Platinum 8488C, 3.6 GHz, shared L3).

### Context switching

| Benchmark | Round-trip | Per switch |
|---|---|---|
| Raw `fcontext` (no scheduler) | ~6.5 ns | ~3.3 ns |
| Scheduler yield (`yield()` -> re-schedule -> resume) | ~81 ns | ~40 ns |

The raw `fcontext` round-trip is two `jump_fcontext` calls with no other work.
The scheduler yield round-trip goes fiber -> scheduler loop -> ready queue
enqueue -> dequeue -> fiber, adding ready queue and state transition overhead.

### Thread producer (semaphore join)

The main thread schedules fibers via `run()`; completion is delivered through a
POSIX semaphore. All fibers land on the main thread's CPU and are stolen by the
remaining 31 scheduler threads.

| N | Wall / iter | Throughput |
|---|---|---|
| 1 | ~11 µs | ~240k fibers/s |
| 4 | ~710 ns | ~2.0M fibers/s |
| 16 | ~360 ns | ~2.8M fibers/s |
| 64 | ~390 ns | ~2.6M fibers/s |
| 256 | ~390 ns | ~2.6M fibers/s |

Throughput saturates at N=16. At saturation, wall time ~= CPU time (~360 ns):
the main thread is never blocking. The bottleneck is its own join+schedule loop.

**Bottleneck breakdown (~360 ns at saturation):** pool and queue ops account for
~55 ns. The remaining ~305 ns is cache-line transfer overhead - each
join+schedule cycle forces the main thread to reclaim ownership of lines last
written by the stealing CPU (~65 ns per transfer, 4-6 transfers).

### Fiber producer (FiberFuture)

The benchmark loop runs inside a driver fiber; child completion is delivered via
`FiberFuture`. Fibers spin for a configurable number of `cpuPause()` iterations
to simulate work (~35 ns per pause).

| N | Spin | Work | Wall / iter | Speedup vs N=1 |
|---|---|---|---|---|
| 1 | 0 | 0 | ~350 ns | -- |
| 16 | 0 | 0 | ~210 ns | ~1.7x |
| 1 | 100 | ~3.5 us | ~1440 ns | -- |
| 16 | 100 | ~3.5 us | ~820 ns | ~1.8x |
| 1 | 1000 | ~35 us | ~12300 ns | -- |
| 16 | 1000 | ~35 us | ~2800 ns | ~4.4x |
| 1 | 10000 | ~350 us | ~121000 ns | -- |
| 16 | 10000 | ~350 us | ~10900 ns | ~11x |

For no-op fibers, N=16 yields 1.7x — steal and scheduling overhead limits gains
at small work. As fiber work grows, parallelism dominates: at ~350 us of work
per fiber, 16 in-flight fibers run 11x faster than serial, close to linear
scaling.

**Fiber producer advantage:** replacing `join` with `FiberFuture` eliminates the
POSIX semaphore from the hot path. For no-op fibers at N=1, this gives ~30x
lower latency (~350 ns vs ~11 us).

### io_uring fiber ping-pong

Two fibers exchange bytes through a pipe, both using io_uring for IO. Each
iteration is one full round-trip: ping writes, pong reads (suspends on CQE),
pong writes back, ping reads (suspends on CQE). Writes complete inline. Isolates
the cost of one async io_uring cycle through the scheduler: SQE submit -> fiber
suspend -> CQE -> fiber resume.

| Benchmark | Round-trip | Per io_uring op |
|---|---|---|
| `IoUringFiberPingPong` | ~7.6 µs | ~3.8 µs |

The ~3.8 µs per operation matches file-perf's 3-5 µs p50 at `numjobs=1
iodepth=1`. The TCP echo p50 of ~23 µs follows directly: 4 io_uring operations
(server read, server write, client read, client write) x ~3.8 µs plus kernel
TCP processing overhead.

### Component costs

| Operation | Cost |
|---|---|
| `MemoryPool` alloc + free (single-thread) | ~5.5 ns |
| `BoundedQueue` enqueue + dequeue (single-thread) | ~12 ns |
| `sem_post` + `sem_wait` (same thread, fast path) | ~18 ns |
| `sem_post` + `sem_wait` (cross-thread, blocking) | ~13 us |
| `eventfd_write` + `eventfd_read` (same thread) | ~315 ns |
| `eventfd_write` + `eventfd_read` (cross-thread, blocking) | ~13 us |
| Cache-line ownership round trip (2 transfers) | ~130 ns (~65 ns each) |
