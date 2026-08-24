# Fiber Scheduler Design

## Overview

This is a cooperative fiber scheduler with per-CPU threads and io_uring integration. Each CPU in the active set runs one scheduler thread that owns its own io_uring ring, ready queue, sleep tree, and wakeup signaling infrastructure.

Source lives under `src/fibers/`. Data structures and utilities are in `src/util/` (see `docs/util.md`). Synchronization primitives are in `docs/sync.md`.

---

## Active CPU Set

The active set is the affinity mask of the thread calling `initialize` (`sched_getaffinity`) intersected with `Options::cpuMask`, a `cpu_set_t` of allowed CPUs with every CPU set by default, admitting the whole affinity mask. A scheduler thread is started and pinned only on each active CPU, and the thread-mode worker pool is pinned to the set of active CPUs, so silk schedules no fiber on a CPU outside the active set (a proxy fiber is the caller's own thread and runs wherever that thread runs). This lets a user reserve CPUs for other activity - busy-loop pollers, for example - by clearing their bits in `cpuMask`.

Within the active set, load concentrates on a processor prefix sized to demand - see the Processor Prefix section.

`processorState` is sized to every configured CPU and indexed by raw CPU id, so a CPU left out of the active set keeps `number == kInvalidProcessorNumber` and owns no ring or ready queue. Work injected from a thread running on such a CPU (a reserved-core poller calling `run`, `schedule`, `sleep`, or IO) is redirected to a home active CPU: each configured CPU maps to itself when active, or to an active CPU chosen round-robin when not, so injection lands on a real ring and spreads across the active set instead of piling onto one. The redirect keys off the current CPU read fresh on every call, so a fiber that migrates mid-flight is always routed by the CPU it currently runs on.

---

## Scheduler Loop

`FiberScheduler` runs one scheduler thread per active CPU, each owning:

- An io_uring ring (256 entries by default, `Options::ioUringQueueSize`) for async IO
- An eventfd for cross-CPU wakeup signaling
- A per-CPU ready queue (`BoundedQueue`) of runnable fibers
- A sleep tree (`SleepTree`) ordered by TSC deadline
- A sleep inbox and cancel inbox (`LockFreeStack`) for lock-free handoff

Each iteration always runs both `handleReadyQueue` and `runServiceLoop`, regardless of whether the first found work. Any work resets the idle counters and immediately continues the loop.

If the own CPU has nothing to do, `runStealLoop` is attempted next. If all three produce nothing, the thread enters the spin/sleep phase.

When idle, `parkThread` calls `io_uring_enter2` - with a `waitNs` timeout inside the processor prefix, with no timeout outside it; cross-CPU wakeup is delivered by writing to an eventfd that is polled persistently in the ring (`IORING_POLL_ADD_MULTI`), producing a CQE that wakes `io_uring_enter2`.

---

## Context Switching

Context switching uses Boost.Context (`fcontext_t`). Each fiber has a 64 KB mmap'd stack with a guard page at each end (bottom and top). ASan stack-swap annotations are included.

Fiber lifecycle: `SUSPENDED -> READY -> RUNNING -> STOPPED`. `SUSPEND_REQUESTED` is a transient state entered at the start of `suspend`; the suspend callback runs while the fiber is in this state, then the fiber transitions to either `SUSPENDED` (genuine suspension) or `READY` (if `schedule` was called during the callback, cancelling the suspension).

`FiberState` is defined in `fiber.cpp` (not `fiber.h`) as an implementation detail. `SleepFuture`, `SleepStack`, and `SleepTree` are declared/aliased in the private section of `FiberScheduler` (in `fiber.h`) so they are accessible from `fiber.cpp`.

`processorNumber` tracks which CPU's ready queue the fiber targets. It is set by `schedule()` on first dispatch (assigned to the scheduling caller's home processor when still `kInvalidProcessorNumber`) and updated by `runStealLoop` when a fiber is stolen (reassigned to the stealing CPU). When a fiber runs on a worker thread via thread mode, `processorNumber` is not updated - the fiber returns to its last assigned CPU's queue on `exitThreadMode()`.

---

## Suspension Pattern

`FiberScheduler::suspend(callback, ctx)` suspends the current fiber and invokes `callback` while the fiber is parked. The callback is responsible for arranging the wakeup (e.g. enqueuing as a waiter). The callback must handle the race where the event already arrived before parking.

The callback and its context are stored on the `Fiber` itself (`suspendCallback`/`suspendContext`) and cleared by `runFiber` after invocation. This allows `runFiber` to be called from both scheduler threads and thread pool workers without per-CPU state.

---

## Proxy Fibers

A non-fiber OS thread (the application's `main`, a thread-pool worker, any external thread) can still participate in every fiber-aware API. The first time such a thread calls `getCurrentFiber()`, the scheduler lazily allocates a `thread_local` **proxy fiber** (`isProxyFiber = true`) that represents the thread itself. The proxy is created on demand and lives for the thread's lifetime; it owns no stack.

Proxy fibers do not context-switch — they **block and wake on a POSIX semaphore** instead of `jump_fcontext`:

- `suspend()` detects `isProxyFiber`, runs the suspend callback inline (the callback arranges the wakeup exactly as for a real fiber), then, if the fiber is still `SUSPENDED`, calls `parkThread()` (`sem_wait`). On return it transitions `READY -> RUNNING`.
- `enqueueReady()` detects `isProxyFiber` and calls `wakeThread()` (`sem_post`) instead of pushing onto a ready queue. There is no scheduler thread to dispatch a proxy; the parked OS thread resumes itself.

The consequence is **uniform thread/fiber interop**: every primitive that keys off `getCurrentFiber()` — `FiberFuture::wait`, `FiberMutex`, `FiberFutex`, the rest of `docs/sync.md` — works identically whether the caller is a real fiber or a plain thread, because each side just suspends "the current fiber" and the other side wakes it. A fiber can hand a result to a waiting thread (and vice versa) through the same `FiberFuture`. It is also what lets the blocking `FiberScheduler::run(fiberMain, params)` overload work from a non-fiber thread: the caller becomes a proxy that parks on its semaphore until the spawned fiber sets the result future. Thread mode (below) is the inverse direction — a real fiber temporarily borrowing a worker thread — so the two mechanisms bracket the fiber/thread boundary from both sides.

---

## Thread Mode

A fiber that needs to make blocking syscalls or perform heavy CPU work can escape the cooperative scheduler without stalling its scheduler thread by entering thread mode.

```cpp
{
    FiberScheduler::ThreadModeScope scope; // enterThreadMode()
    // blocking work here -- runs on a thread pool worker
}                                          // exitThreadMode()
```

There are two ready queues:

- **Per-CPU ready queue** (`ProcessorState::readyQueue`) - bounded MPMC queue drained by the CPU's scheduler thread. Normal cooperative fibers live here.
- **Shared ready queue** (`SchedulerState::readyQueue`) - unbounded MPMC queue drained by the worker thread pool (one worker thread per active CPU, sized `workerThreadCount == schedulerThreadCount`, pinned to the active set, running `runThreadWorker`). Thread-mode fibers live here, and normal fibers overflow here when a CPU ready queue is full.

`enterThreadMode()` sets `fiber->inThreadMode` and calls `schedule()`, which routes the fiber to the shared ready queue. A worker thread dequeues it and runs it via `runFiber(nullptr, fiber)`, where it may block freely. When the fiber suspends inside thread mode (e.g. waiting on a future), `schedule()` re-enqueues it to the shared ready queue when it is woken - any free worker picks it up.

`exitThreadMode()` clears `fiber->inThreadMode` and calls `schedule()`, which routes the fiber back to its CPU's per-CPU ready queue. After this point the fiber runs cooperatively again.

`schedule()` routing summary:
- `inThreadMode = true` => shared ready queue
- `inThreadMode = false`, CPU ready queue not full => CPU ready queue
- `inThreadMode = false`, CPU ready queue full => shared ready queue (overflow)

---

## Synchronization Primitives

`FiberMutex`, `FiberFutex`, `FiberSequencer`, `FiberEvent`, and `FairFiberMutex` are documented in `docs/sync.md`.

`FiberMutex` and `FiberFutex` use the scheduler's waiter table directly (`enqueueWaiter` / `releaseWaiters`, keyed by `this`). The higher-level primitives build on `FiberSequencer`, which manages its own ordered waiter tree via a combiner lock.

---

## Data Structures

The scheduler uses `LockFreeStack`, `BoundedQueue`, `IntrusiveQueue`, `Tree`, and `MemoryPool` from `src/util/`. See `docs/util.md` for full descriptions.

Key usages:
- `MemoryPool` - `Fiber` object allocation only (rseq fast path, zero atomics). The pool recycles fibers rather than freeing, so each fiber keeps the 64 KB stack it `mmap`s once on first init.
- `BoundedQueue` - per-CPU ready queue (MPMC, fixed capacity; holds `Fiber *`)
- `IntrusiveQueue<Fiber, &Fiber::reservedNode>` - shared overflow / thread-mode ready queue; the node is embedded in each fiber, so enqueuing never allocates
- `LockFreeStack` - sleep inbox, cancel inbox
- `Tree` - sleep deadline ordering (keyed by TSC deadline)

---

## Timing

All sleep deadlines are in TSC cycles. `Tsc::getCycles()` is `rdtsc` / `cntvct_el0`. Conversion uses fixed-point multiply-shift (`cycles * nsPerCycleFp >> 20`) - no division on the hot path. Frequency is detected once at startup via CPUID / hypervisor leaves / `cntfrq_el0`. See `docs/util.md` for details.

---

## Async IO

Each of `read`, `write`, and `poll` has two overloads: a blocking form that submits an io_uring SQE and suspends the fiber until completion, and an async form that submits the SQE and returns immediately with an `IoFuture*` the caller waits on separately. `handleCompletionQueue` processes CQEs, extracts the `IoFuture*` from the CQE user data, and calls `future->set()` to wake the waiting fiber.

### Registered (fixed) buffers

`readFixed` / `writeFixed` are async-only counterparts to `read`/`write` that submit `IORING_OP_READ_FIXED` / `IORING_OP_WRITE_FIXED` against a buffer that was pre-registered with the kernel via `registerBuffers`. Instead of passing an iovec the kernel must pin per IO, the caller passes a `(buf, len)` plus the `bufIndex` of a previously registered buffer; the kernel reuses the pre-pinned page mapping and skips the per-IO page-pin and iovec import. `buf` must lie inside the registered buffer at `bufIndex`.

`registerBuffers(iovecs, count)` registers one buffer set on **every active** CPU's io_uring ring, so a fiber that is work-stolen to another CPU can still submit fixed IO referencing the same index. Buffers are addressable as `bufIndex` `0..count-1`. Constraints:

- Call once, after `initialize()` and before issuing any fixed-buffer IO. io_uring allows a single buffer set per ring with no way to undo or change it, so a second call fails (`-EBUSY`) and trips an assert.
- Each ring pins its own copy, so total locked memory is `(number of active CPUs) * (size of all buffers)`. With many CPUs or large buffers this can exceed `RLIMIT_MEMLOCK`; registration then fails and trips an assert.

The underlying liburing helpers are `io_uring_register_buffers`, `io_uring_prep_read_fixed`, and `io_uring_prep_write_fixed`. `file-perf --fixed-buffers` exercises this path (see `docs/perf.md`).

### Splice

`splice` submits `IORING_OP_SPLICE` in the same blocking and async forms as `read` and `write`, and moves bytes between two descriptors entirely inside the kernel. As with `splice(2)` at least one of the two descriptors must be a pipe, and the offsets apply only to seekable files, so a caller relaying a stream between two sockets moves the bytes through an intermediate pipe: one splice from the source socket into the pipe, another from the pipe into the destination socket. Nothing is copied into user space, so the throughput of such a relay does not depend on any user-space buffer size; `bytesSpliced` of zero means the source reached end of input.

---

## Sleep Cancellation

`SleepFuture` has an atomic `state` with two bits: `IN_TABLE` (entry is in the sleep tree) and `CANCELLED`. `cancelSleep` does a `fetch_or(CANCELLED)` -- if `IN_TABLE` was set, it also pushes the entry onto `cancelQueue`. `handleCancelQueue` drains `cancelQueue` and calls `sleepTree.remove()` directly (O(log n), no scan).

The `StackEntry` inside `SleepFuture` is shared between `sleepQueue` and `cancelQueue` - only one can hold it at a time, enforced by the `IN_TABLE` flag.

---

## Work-Stealing

An idle CPU can steal fibers from a neighbor's ready queue and claim a neighbor's frozen service loop to process its pending completions and expirations. Without stealing, load imbalance is permanent and service loop starvation occurs while a scheduler thread is inside a fiber.

### Service Loop Claiming

`runServiceLoop` processes io_uring CQEs (`handleCompletionQueue`), the sleep inbox (`handleSleepQueue`), cancellation requests (`handleCancelQueue`), and expired waiters (`handleExpiredWaiters`), then stamps the attendance heartbeat. It is protected by `serviceLoopLock` so at most one thread runs the service loop for a given processor at a time.

- **Owner thread**: try-locks before running its own service loop. If a helper holds the lock, the owner skips it entirely - the helper is already doing the work.
- **Helper thread**: try-locks the victim's lock; if it fails, moves to the next candidate.

There is no armed deadline to race: the owner caps its next park at the earliest `sleepTree` deadline, and every tree mutation republishes that deadline under the lock for the pre-park sweeps to read.

`enqueueWakeup` is excluded from the `didWork` return value - it is preparation for sleeping, not productive work. Counting it would cause a spin loop.

Fibers woken by a claimed service loop are enqueued via `schedule()` using `fiber->processorNumber`, routing them back to the victim's ready queue.

### Fiber Stealing

An idle CPU dequeues fibers from a neighbor's `readyQueue` and runs them locally. `BoundedQueue` is MPMC, so `victim->readyQueue.dequeue()` is safe from any thread.

### Victim Selection

`buildStealCandidates()` runs once at `initialize()`. For each active CPU it builds a `stealCandidates[]` array sorted by estimated steal cost (cheapest first). Topology is read from sysfs:

- `/sys/devices/system/cpu/cpuN/topology/core_id`
- `/sys/devices/system/cpu/cpuN/topology/physical_package_id`
- `/sys/devices/system/node/nodeN/cpulist` (NUMA)

Cost tiers:

| Distance | Cost threshold |
|---|---|
| HT sibling | ~1 us |
| Same socket | ~50 us |
| Cross-socket | ~500 us |

Thresholds reflect cache warming cost: fiber stealing moves potentially hundreds of KB of stack and heap data, while service loop claiming touches a bounded ~2 KB io_uring CQ. Within each tier, candidates are shuffled randomly (seeded by CPU number) to spread load. Inactive CPUs get `UINT64_MAX` cost and sort to the end. If sysfs is unavailable, all active CPUs get the same fallback cost.

### Steal Loop

`runStealLoop` computes `idleCycles = now - idleSinceCycles` and a budget deadline of `now + idleCycles`. It walks `stealCandidates[]` cheapest first, breaking immediately if `idleCycles < candidate->costCycles`. For each candidate it:

1. Calls `runServiceLoop(victim)` - the `try_lock` handles concurrency.
2. Drains `victim->readyQueue` via repeated `dequeue()`, calling `runFiber` for each fiber, until the queue is empty or the budget deadline passes.

The deadline is shared across all candidates and all fiber steals, bounding total steal time to `idleCycles`.

### Idle Progression

- `waitNs` starts at `Options::initialWaitNs` (1 us) and doubles each idle iteration up to `Options::maxWaitNs` (10 ms).
- While `waitNs < Options::spinThresholdNs` (20 us): `spinWait` runs 64 x `cpuPause` iterations (~2 us) checking `hasWork`.
- Once `waitNs >= Options::spinThresholdNs`: `parkThread` calls `io_uring_enter2` with a `waitNs` timeout. The timed wait ensures sleeping prefix processors wake periodically to attempt stealing even without an explicit wakeup.
- A processor outside the prefix that has ramped to `Options::maxWaitNs` with no pending SQEs parks with no timeout - wakeups are doorbell-driven; the standby keeps the timed cadence instead (see Processor Prefix).

Any productive iteration resets `waitNs` back to `Options::initialWaitNs`.

---

## Processor Prefix

**The scheduler runs on a processor prefix sized to load.** Processors are ordered whole cores first, HT siblings after; the prefix of that order is the awake set. Processors inside the prefix park timed (capped at `Options::maxWaitNs`) and steal from each other; processors outside park with no timeout and hold no work. The scheduler boots at full width and decays; a fully idle scheduler decays to one processor plus the standby poller - the width floors at one. The timed park is the only way parked neighbors learn about stealable backlog, so scoping it to the prefix removes the idle fleet's polling (each park pays kernel newidle balancing) and concentrates small loads on few cores, where every extra awake core inserts park/doorbell round trips into the commit path.

**One time constant governs the width.** `Options::maxWaitNs` is the park backoff cap, the backlog age that grows the prefix, and the wait-outcome window that shrinks it. `Options::disableCpuAdjust` pins the width at full for static-width baselines.

**The policy lives in `CpuController`.** Every grow and shrink decision routes through one gate; the scheduler embeds the controller (a per-processor `Window` of signals on each processor, the shared state on the scheduler) and executes the decisions it returns.

**Growth: two doors, one gate.** A queue continuously non-empty for one window is the stall-regime door: a backlog observation stamps the queue, the owner clears the stamp whenever it drains empty - backlog consumed in time never grows anything - and a stamp that survives a full window asks to grow, paced to one processor per queue per window. A window whose dispatches pile up behind two others is the closed-loop door: a chain of dependent wakes touches empty between bursts and never ages the stamp, so its queueing shows as excess dispatch depth and votes to grow. Both doors pass the same gate - full width refuses, and growth paces to one processor per window fleet-wide, so steal traffic re-homes each new member's share before the next one can prove demand - and a granted grow starts the next processor and rings its ordinary doorbell. No work is assigned to it - its own steal loop finds the backlog cheapest-first.

**Shrink: the rightmost processor extinguishes itself after three consecutive windows of wasted waits - never the first, so the width floors at one.** Only the rightmost meters its windows, and a recent growth vetoes the shed for four windows, so a freshly started member is not judged before steal traffic re-homes its share. Wait outcomes are the signal: a wait rewarded by arriving work - a spin or park cut short, or a park expiry whose drain finds due work - is demand; an expired spin or an empty park expiry is waste; a window whose waste outnumbers rewards eightfold reads as shrink-able. Loaded processors measure 3-4x waste whatever their CPU duty cycle, so IO-heavy load holds the width - utilization cannot make that distinction, which is why time-based signals breathe at the load boundary - and a busy processor never waits, produces no outcomes, and never shrinks. A ripening sleeper rewards each bounded park, so a processor hosting a periodic timer keeps its width. A pure-idle window - not one dispatch - sheds without the streak: there is no load to misread, only decay to finish. A shrunk processor keeps serving its sleep tree; its wakers migrate left, and the drained tree lets it park indefinitely.

**The standby probes.** The first processor right of the prefix keeps timed parks - the only observer of backlog aging behind a prefix too busy to signal. Each growth appoints the new standby by doorbell (a sleeping processor never re-evaluates its role on its own); a standby that finds aged backlog wakes the next standby first and then activates itself into the prefix.

**A pre-park sweep makes silence lossless.** Ready-queue work has a producer running scheduler code at arrival; ring work arrives from the kernel and sleep expiries from the clock, with no agent to signal. Each processor therefore publishes its attendance - a heartbeat stamped on every completed service pass, and its earliest sleep deadline - and before parking, a prefix processor scans its neighbors for aged ready backlog and for unattended work: a heartbeat stale for a full window while the ring holds completions or the published deadline is due, the signature of a thread held inside a long-running fiber or blocked outside the scheduler loop. A hit aborts the park and the steal loop takes the work; the aged-backlog rescue passes the same grow gate as every door, while the unattended rescue is unconditional. The sweep and the growth check are fence-ordered against each other, so either the sweeper sees the enqueue or the producer sees the shrunk prefix and grows it - no interleaving loses both. The park role is classified behind the same fence, so a grow either meets a fresh classification or rings the parked target, and an activating standby appoints the next poller before the width moves - the timed observer never lapses. A callback that blocks a scheduler thread fails loudly, so no park waits on a held thread.

**Placement: a fiber migrates when its home cannot run it without a doorbell** - no home assigned yet, deactivated out of the prefix, or sleeping below full width while an awake prefix producer with an empty ready queue could run it with its data still warm; a producer holding queued work stops attracting, else wakes recentralize on the loaded member and undo every steal. The fiber lands on the producer when it is a prefix member, on the first processor otherwise. At full width placement stays sticky, and stealing is unchanged at every width. The `SchedulerThreadGrow` / `SchedulerThreadShrink` counters expose the controller.

**Known limit.** At partial stall-heavy load the width still moves at the load boundary during warmup, and the run-to-run throughput spread across warmup-locked pipeline modes remains; a pinned width is unimodal, so the residual cost is the transitions, not the resting width.

---

## Performance

Measurements on a 32-CPU x86 machine (Intel Xeon Platinum 8488C, 3.6 GHz, shared L3).

### Context switching

| Benchmark | Round-trip | Per switch |
|---|---|---|
| Raw `fcontext` (no scheduler) | ~6.5 ns | ~3.3 ns |
| Scheduler yield (`yield()` -> re-schedule -> resume) | ~165 ns | ~83 ns |

The raw `fcontext` round-trip is two `jump_fcontext` calls with no other work. The scheduler yield round-trip goes fiber -> scheduler loop -> ready queue enqueue -> dequeue -> fiber, adding ready queue and state transition overhead.

### Thread producer (semaphore join)

The main thread schedules fibers via `run()`; completion is delivered through a POSIX semaphore. All fibers land on the main thread's CPU and are stolen by the remaining 31 scheduler threads.

| N | Wall / iter | Throughput |
|---|---|---|
| 1 | ~4.2 µs | ~490k fibers/s |
| 4 | ~970 ns | ~1.5M fibers/s |
| 16 | ~430 ns | ~2.7M fibers/s |
| 64 | ~330 ns | ~3.0M fibers/s |
| 256 | ~330 ns | ~3.0M fibers/s |

Throughput saturates at N=64. At saturation, wall time ~= CPU time (~330 ns): the main thread is never blocking. The bottleneck is its own join+schedule loop.

**Bottleneck breakdown (~330 ns at saturation):** pool and queue ops account for ~55 ns. The remaining ~275 ns is cache-line transfer overhead - each join+schedule cycle forces the main thread to reclaim ownership of lines last written by the stealing CPU (~65 ns per transfer, 4-5 transfers).

### Fiber producer (FiberFuture)

The benchmark loop runs inside a driver fiber; child completion is delivered via `FiberFuture`. Fibers spin for a configurable number of `cpuPause()` iterations to simulate work (~35 ns per pause).

| N | Spin | Work | Wall / iter | Speedup vs N=1 |
|---|---|---|---|---|
| 1 | 0 | 0 | ~420 ns | -- |
| 16 | 0 | 0 | ~310 ns | ~1.4x |
| 1 | 100 | ~3.5 us | ~1640 ns | -- |
| 16 | 100 | ~3.5 us | ~690 ns | ~2.4x |
| 1 | 1000 | ~35 us | ~13200 ns | -- |
| 16 | 1000 | ~35 us | ~2320 ns | ~5.7x |
| 1 | 10000 | ~350 us | ~128000 ns | -- |
| 16 | 10000 | ~350 us | ~11600 ns | ~11x |

For no-op fibers, N=16 yields 1.4x — steal and scheduling overhead limits gains at small work. As fiber work grows, parallelism dominates: at ~350 us of work per fiber, 16 in-flight fibers run 11x faster than serial, close to linear scaling. The wide points need a warmed-up scheduler: the first parallel burst after an idle period runs on a narrow prefix and pays the growth pacing of one processor per window, so short cold bursts measure the ramp, not the steady state.

**Fiber producer advantage:** replacing `join` with `FiberFuture` eliminates the POSIX semaphore from the hot path. For no-op fibers at N=1, this gives ~10x lower latency (~420 ns vs ~4.2 us).

### io_uring fiber ping-pong

Two fibers exchange bytes through a pipe, both using io_uring for IO. Each iteration is one full round-trip: ping writes, pong reads (suspends on CQE), pong writes back, ping reads (suspends on CQE). Writes complete inline. Isolates the cost of one async io_uring cycle through the scheduler: SQE submit -> fiber suspend -> CQE -> fiber resume.

| Benchmark | Round-trip | Per io_uring op |
|---|---|---|
| `IoUringFiberPingPong` | ~2.6 µs | ~1.3 µs |

The ~1.3 µs per operation is the scheduler-side floor with both fibers concentrated on one processor by the prefix. The single-connection TCP echo p50 of ~9 µs follows: 4 io_uring operations (server read, server write, client read, client write) x ~1.3 µs plus kernel TCP processing overhead.

### Component costs

| Operation | Cost |
|---|---|
| `MemoryPool` alloc + free (single-thread) | ~5.5 ns |
| `BoundedQueue` enqueue + dequeue (single-thread) | ~12 ns |
| `sem_post` + `sem_wait` (same thread, fast path) | ~18 ns |
| `sem_post` + `sem_wait` (cross-thread, blocking) | ~5.2 us |
| `eventfd_write` + `eventfd_read` (same thread) | ~315 ns |
| `eventfd_write` + `eventfd_read` (cross-thread, blocking) | ~5.3 us |
| Cache-line ownership round trip (2 transfers) | ~130 ns (~65 ns each) |
