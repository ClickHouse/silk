# Silk — Talk Source Document

> **Purpose of this document.** This is a self-contained briefing to upload to claude.ai (or another tool) to generate a slide deck. It is organized so each top-level section maps to one or a few slides. Numbers, claims, and structure are drawn directly from the silk codebase and its `docs/`. A suggested slide outline is at the end.

---

## 1. One-line pitch

**Silk is a cooperative fiber scheduler for Linux** — per-CPU scheduler threads, io_uring for all async I/O, and topology-aware work-stealing. It lets you write straight-line synchronous-looking code that scales to millions of concurrent operations.

Fibers are **lightweight stackful coroutines** that *suspend* instead of *blocking* their OS thread, giving high concurrency at low overhead.

---

## 2. The problem

Modern servers need to handle tens of thousands of concurrent connections / I/O operations. The classic options each have a sharp edge:

- **Thread-per-connection** — simple to write (blocking calls, normal stacks), but the OS scheduler chokes at 10k+ threads: context-switch cost, multi-millisecond tail-latency jitter, large memory per thread.
- **Callbacks / epoll reactors** — fast and lean, but the code fragments into state machines. No natural way to express sleeps, multi-step protocols, or branching control flow without building "a small interpreter."
- **C++20 stackless coroutines** — fast context switch, but **viral**: every function in a suspendable call chain must itself be a coroutine, every level is a separate heap allocation, and you cannot integrate synchronous third-party libraries without rewriting them.

**Silk's answer:** stackful fibers. You write code that *looks* blocking; the fiber suspends transparently at any call depth, the OS thread keeps working, and a third-party synchronous library (Poco, the AWS SDK) "just works" with no rewrite.

---

## 3. What a fiber is

A fiber has a **real stack** — one 64 KB mmap'd allocation (guard page at each end) that holds the entire call chain. Any function at any depth can suspend; the scheduler saves the stack pointer + registers and resumes another fiber.

```
fiber stack (one allocation):
  [ handler frame        ]
  [ parseRequest frame   ]
  [ readFromSocket frame ]  <-- suspends here; scheduler runs another fiber
```

From the programmer's view, code is **synchronous**: call a blocking-looking function, and the fiber suspends underneath you.

```cpp
int handleConnection(Conn * c) {
    char buf[4096];
    uint64_t n = 0;
    FiberScheduler::read(c->fd, buf, sizeof buf, 0, &n);  // suspends the fiber, not the thread
    FiberScheduler::write(c->fd, buf, n, 0);              // io_uring under the hood
    return 0;
}

// Launch a fiber and wait for its result:
int result = FiberScheduler::run(handleConnection, conn);  // typed params, copied into the fiber
```

`run()` is typed: it takes `int(*)(T*) noexcept` plus a parameter value (up to 64 B, moved into the fiber) and either blocks for the `int` result or delivers it through a `FiberFuture`. An optional `category` byte tags the fiber so the built-in profiler can group latency samples by role.

Contrast with stackless coroutines, where the whole chain must be `co_await`-annotated and each level is a separate heap frame:

```cpp
Task<Response> handler()            { co_return co_await sendRequest(...); }
Task<Response> sendRequest(...)     { co_return co_await receiveResponse(...); }
Task<Response> receiveResponse(...) { co_return co_await readStream(...); }
// 10 levels deep = 10 heap allocations to reach the I/O.
```

---

## 4. Architecture at a glance

One **scheduler thread pinned per CPU**. Each owns its own state — no shared mutable hot path:

- An **io_uring ring** (256 entries by default, configurable) for async I/O
- An **eventfd** for cross-CPU wakeup signaling
- A **per-CPU ready queue** (bounded MPMC) of runnable fibers
- A **sleep tree** ordered by TSC deadline
- A **sleep inbox + cancel inbox** (lock-free stacks) for lock-free handoff

The scheduler loop, each iteration:
1. `handleReadyQueue` — run ready fibers.
2. `runServiceLoop` — drain io_uring completions, sleep/cancel inboxes, expired waiters; arm next deadline.
3. If own CPU is idle → `runStealLoop` — steal from neighbors.
4. If still nothing → spin, then park in `io_uring_enter2` with a timeout. Cross-CPU wakeup arrives as an eventfd CQE.

---

## 5. Zero allocation on the hot path (the design spine)

This is the property that ties the whole design together, and the main reason silk suits memory-pressure-sensitive workloads where general-purpose async runtimes thrash. **Silk does not call `malloc` in steady state.** Three deliberate choices get it there:

**1. Every internal container is intrusive.** The object being tracked *embeds* the linkage; there is no separate node to allocate.
- `LockFreeStack<T, &T::stackEntry>` — sleep inbox, cancel inbox, waiter handoff.
- `Tree<T, &T::treeEntry, …>` (a `boost::intrusive` red-black tree) — sleep deadlines, ordered sequencer waiters.
- intrusive `List<Future>` — condition-variable waiters.
- `IntrusiveQueue<Fiber, &Fiber::reservedNode>` — the shared (overflow / thread-mode) ready queue; each fiber carries its own embedded queue node, so enqueuing allocates nothing.

**2. Futures and wait-state live on the caller's stack.** The blocking `read`/`write`/`poll`/`accept`/`connect`/`sleep` wrappers each declare their completion handle as a **plain local** (`IoFuture future;` / `SleepFuture future;`) and suspend on it — no heap. This works because `FiberFuture` carries no shared control block: it's ~16 bytes of packed atomic state, not a heap-allocated, refcounted future/promise pair like `std::future` or `folly::Future` (see §9). `waitForMultiple` builds its `MultipleWaitState` on the stack; a `FiberCondVar` waiter is a `Future` on the waiting fiber's own stack. The fiber's stack *is* the allocation, so anything scoped to a wait is free.

**3. Fibers are pooled, and each keeps its stack.** `Fiber` objects — the only thing `MemoryPool` allocates — come from a per-CPU pool (`fiberPool`) backed by a `ShardedStack` (rseq, zero atomics on the fast path); the pool recycles fibers rather than freeing them. Each fiber lazily `mmap`s its 64 KB stack (guard page each end) **once** and holds onto it, so a reused fiber reuses its existing stack — steady-state fiber churn triggers no `mmap`/`munmap` and no allocator calls.

**Why it matters — allocator invariance.** Because silk never touches the allocator on the hot path, its throughput is *invariant* to allocator quality. Run a benchmark with and without `jemalloc`: silk's numbers don't move, an allocation-per-op runtime (Asio) improves a little, an allocation-heavy one (Poco) improves a lot. Allocator quality is a hidden variable in async-runtime benchmarks — silk is the one engine that takes it off the table. That reframes the speed wins in §13: part of silk's lead over Asio is simply that *silk wasn't paying for an allocation per async op in the first place*.

> Trade-off (honest): the same pooling that gives zero steady-state allocation also **retains peak memory** — see §18.

---

## 6. Context switching

Built on **Boost.Context** (`fcontext_t`). Each fiber: 64 KB mmap'd stack, guard page at each end, ASan stack-swap annotations.

Lifecycle: `SUSPENDED → READY → RUNNING → STOPPED`.

Measured cost (32-CPU Intel Xeon Platinum 8488C, release build):

| Benchmark | Per switch |
|---|---|
| Raw `fcontext` (no scheduler) | ~3.3 ns |
| Full scheduler round-trip via `yield`, **work-stealing on** | ~3.6 ns |
| Full scheduler round-trip via `yield`, work-stealing off | ~126 ns |

**The headline:** with work-stealing, a stealer thread is *already spinning* on another CPU, so a yielding fiber is picked up immediately — near-zero scheduling overhead. (Trade-off: steal threads burn CPU spinning when work is sparse.)

> In any real I/O workload, context-switch cost is dominated by io_uring completion latency (microseconds), so these nanosecond differences are immaterial — they just mean the scheduler is never the bottleneck.

---

## 7. Work-stealing — the differentiator

Without stealing, load imbalance is permanent and a scheduler thread stuck inside a long fiber starves its own service loop. Silk steals **two things**:

1. **Fibers** — an idle CPU dequeues runnable fibers from a neighbor's ready queue and runs them locally.
2. **Service loops** — an idle CPU can claim a neighbor's *frozen* service loop and drain its io_uring completions and expired sleeps. This is the subtle, important one: even with no fibers to steal, a stalled CPU's pending I/O still gets processed.

**Topology-aware victim selection.** Built once at startup from sysfs (`core_id`, `physical_package_id`, NUMA `cpulist`). Candidates sorted cheapest-first by cache-warming cost:

| Distance | Steal cost threshold |
|---|---|
| Hyperthread sibling | ~1 µs |
| Same socket | ~50 µs |
| Cross-socket | ~500 µs |

A CPU only steals from a victim if its accumulated idle time exceeds that victim's cost — so it never pays more to steal than it has been idle.

---

## 8. Async I/O via io_uring

`read`, `write`, `poll`, `accept`, `connect`, and `sleep` each have two forms:
- **Blocking form** — submit an SQE, suspend the fiber until the CQE arrives.
- **Async form** — submit and return an `IoFuture*` immediately; wait on it later (enables per-fiber I/O queue depth, and `waitForMultiple` / `waitWithTimeout` across several in-flight ops). Both `IoFuture` and `SleepFuture` are cancellable via `cancel()`.

Submission is **bounded-batched**: `io_uring_submit` fires when either 64 SQEs accumulate *or* 100 µs elapses. This amortizes the syscall (one run averaged ~12 SQEs per syscall over 23.4M syscalls) and caps the tail. Without the bound, p50 is lower but p95–p99.9 inflate 5–10×.

A per-CPU **latency profiler** (opt-in) records seven points in the fiber/IO lifecycle (`io_wait`, `sq_wait`, `submit_io`, `cq_wait`, `ready_wait`, `suspend_wait`, `fiber_run`) as log2 histograms, queryable by fiber category — the source of the breakdown tables in the perf docs.

---

## 9. Synchronization primitives (purpose-built for fibers)

These are **not wrappers around `std::mutex`/`std::future`** — they are built from scratch for the scheduler. Waiting **suspends the fiber and frees the OS thread** to run other work; a `std::mutex` would block the whole scheduler thread and stall every other fiber on that CPU. And — consistent with §5 — every one is allocation-free: the waiter handle lives on the waiting fiber's stack and links into an intrusive structure.

```
FiberFuture     -- single-producer/single-consumer result handle (the leaf primitive)
FiberFutex      -- counter-based wakeup (Linux futex pattern)
FiberMutex      -- shared mutex, writer-priority, std::lock_guard-compatible
FiberCondVar    -- condition variable (mirrors std::condition_variable)

FiberSequencer  -- monotone counter with ordered, cancellable waiters
  FiberEvent      -- manual-reset event
  FairFiberMutex  -- fair (FIFO) ticket mutex
```

Highlights:
- **`FiberFuture` — a future with no allocation and no shared control block (a genuinely unusual property).** The entire object is one packed `std::atomic<uint64_t>` (`{waiter:61, multipleWait:1, hasCallback:1, isSet:1}`) plus an `int error` — ~16 bytes, single-producer/single-consumer, living on the caller's stack. The "shared state" *is* that one atomic word; the registered waiter is stored inline as the `Fiber*` itself, and wakeup is a direct scheduler enqueue.

  Compare the standard pattern: `std::promise`/`std::future` and `folly::Future`/`folly::Promise` split a future/promise pair over a **heap-allocated, reference-counted shared-state control block**.
    - *std* (verified): the shared state is heap-allocated — `std::promise` even has an `allocator_arg_t` constructor that exists specifically to let you control that allocation — and ownership is shared between the promise and future. The blocking `get()` is synchronized inside the shared state with a mutex + condition variable (libc++) or a futex (libstdc++), depending on the standard library.
    - *folly* (verified against `folly/futures/detail/Core.h`): a `Core<T>` is heap-allocated via the static factory `Core::make()`, carries `result_` + `callback_` + `executor_` + an atomic `state_`, and is atomically reference-counted (an atomic attach/detach so the future and promise share one Core and free it on last release).

  Either way, every such future costs a heap allocation plus an atomic refcount on creation and teardown. silk's `FiberFuture` is zero allocations, no separate control block, no refcount, no mutex/cv. `waitWithTimeout` is just `waitForMultiple` over `{future, sleepFuture}` — composition stays on the stack too.
- **`FiberMutex`** is writer-priority and conforms to the standard `Lockable` / `SharedMutex` concepts — drop-in with `std::unique_lock` / `std::shared_lock`.
- **`FiberSequencer`** uses a combiner-lock pattern: one thread drains incoming/cancel queues and a token-ordered tree, deferring wakeups so `set()` can re-enter safely.

---

## 10. Threads and fibers interoperate — the ProxyFiber bridge

**You can call the entire silk API — `FiberScheduler`, `FiberFuture`, `FiberMutex`, all of it — directly from an ordinary OS thread.** No bridge type, no special queue, no "post to the scheduler" dance. A `main()` thread, a thread-pool worker, or any external thread can wait on a `FiberFuture` set by a fiber, lock a `FiberMutex` a fiber holds, or call the blocking `FiberScheduler::run(...)` and get the result.

How it works: the first time a non-fiber thread touches a fiber-aware API, the scheduler lazily creates a `thread_local` **proxy fiber** that represents the thread. Proxy fibers don't context-switch — they **park and wake on a POSIX semaphore** (`sem_wait` / `sem_post`) instead of `jump_fcontext`:

- `suspend()` on a proxy runs the wait-arranging callback inline, then `sem_wait`s the thread to sleep.
- `enqueueReady()` on a proxy `sem_post`s to wake the parked thread, instead of pushing onto a per-CPU ready queue.

Because **every primitive keys off `getCurrentFiber()`**, neither side knows or cares whether the peer is a real fiber or a proxy. A fiber and a thread synchronize and pass results through the *same* `FiberFuture` — the thread just suspends "its" proxy fiber and the fiber wakes it (and vice versa). This is the inbound counterpart to **thread mode** (§11), which sends a fiber *out* to a worker thread; together they make the fiber/thread boundary fully two-way.

> Practical upshot: silk drops into an existing threaded application incrementally. Your `main`, your existing thread pools, and your fibers all speak the same synchronization vocabulary.

---

## 11. Thread mode — fibers that need to block

The flip side of §10. When a fiber makes a **raw blocking syscall** (e.g. deep inside a third-party library) or runs a long CPU-bound computation, it ties up its per-CPU scheduler thread. Work-stealing softens this — idle CPUs steal this CPU's *ready* fibers and even drain its io_uring completion queue (that's the §14 stall-resilience), so the others don't freeze — but the blocking fiber still **pins one scheduler thread** for the duration, costing a slice of cooperative capacity and adding migration jitter; if many fibers block at once they can starve the scheduler outright. Thread mode keeps that work off the scheduler threads entirely: the fiber runs on a **dedicated worker thread** where it can block freely.

**Switching is trivial** — a one-line RAII scope, and you can flip back and forth as often as you like:

```cpp
{
    FiberScheduler::ThreadModeScope scope;   // now running on a worker thread
    auto rows = db.runBlockingQuery(...);    // block all you want — no scheduler thread stalls
}                                            // back to cooperative mode on its CPU
// ... cooperative again here: suspend on FiberFutures, do io_uring I/O, etc.
```

**The mechanism — a dedicated worker pool draining a common queue:**
- Alongside the per-CPU scheduler threads, silk runs a **pool of worker threads (one per CPU)**. Each loops pulling fibers from a single **shared, unbounded, intrusive MPMC queue** (`runThreadWorker`) and runs them — where they may block freely — then parks when the queue drains.
- `enterThreadMode()` just flags the fiber and `schedule()`s it onto that shared queue; a free worker picks it up. If it suspends *while in thread mode* (e.g. waits on a `FiberFuture`), it goes right back to the shared queue for any worker to resume — so blocking **and** waiting compose.
- `exitThreadMode()` clears the flag and `schedule()`s it back onto its CPU's per-CPU ready queue — cooperative again. (The same shared queue also absorbs overflow when a per-CPU ready queue is full.)

Together with the ProxyFiber bridge (§10), this makes the fiber/thread boundary **fully two-way and cheap to cross**: threads call fiber APIs inbound; fibers borrow worker threads outbound; both directions are a line of code. It's also what lets silk host synchronous libraries that occasionally block hard.

---

## 12. The TLS hazard (a deep, non-obvious problem silk solves)

Because fibers **migrate between OS threads** (work-stealing, overflow, thread mode), native `thread_local` is **unsafe** in fiber code. The compiler caches the thread pointer in a callee-saved register; after a migration, that register still points at the *old* thread's storage → stale reads, segfaults at tiny fixed addresses.

There is **no compiler flag** to disable this on GCC/Clang (MSVC has `/GT`; the GNU toolchain has nothing — GCC PR 26461).

Silk's mitigations:
- `getCurrentProcessor` reads the thread pointer through **volatile asm** (can't be hoisted/CSE'd).
- `getCurrentFiber` / `getCurrentFiberId` are `__attribute__((noinline))` — the `noinline` is load-bearing, not a hint.
- `errno` is captured into a local immediately after each syscall, before any suspension.

This is a great "things you only learn building a real runtime" slide.

---

## 13. Performance — headline numbers

All on AWS 32-CPU Intel Xeon Platinum 8488C, Linux 6.17, release `-O3`, 60 s runs / 10 s warmup.

### TCP echo (net-perf), 64 B messages

| connections | silk (fibers + io_uring) | asio (C++20 coroutines + epoll) | ratio |
|---|---|---|---|
| 1 | 38k RPS, p50 27 µs | 3k RPS, p50 348 µs | **~13×** |
| 256 | 1961k RPS | 339k RPS | **~5.8×** |
| 512 | 2140k RPS | 358k RPS | **~6.0×** |
| 1024 | 2210k RPS | 427k RPS | **~5.2×** |

**Why silk wins:** Asio posts *every* completion — even immediate ones — to a mutex-protected handler queue, and allocates per async op. Silk drops a CQE straight into a per-CPU lock-free ready queue, a spinning stealer picks it up in nanoseconds via a direct register swap, and (per §5) it makes no allocation at all. (Ruled out as causes: io_uring-vs-epoll, jemalloc, thread count.)

### vs. raw epoll (the honest comparison)

| connections | silk RPS | raw epoll RPS | epoll p99 advantage |
|---|---|---|---|
| 256 | 1961k | 2478k (1.26× faster) | 0.43× (epoll tighter) |
| 1024 | 2210k | 2485k (1.12× faster) | 0.25× (epoll tighter) |

Raw multi-threaded epoll beats silk by 12–26% on throughput and on tail latency past saturation — that ~1.9 µs/request is the cost of the fiber abstraction. **What epoll gives up: composability.** Its state machine can't naturally express sleeps, multi-step protocols, or branching control flow. *net-perf-epoll is the throughput floor; silk is the structure you'd actually program against.*

### Async file I/O (file-perf, tmpfs)

- Best throughput (16 jobs × iodepth 16, randread): **5.58M IOPS, 21.3 GiB/s**
- Best latency (1 job × iodepth 1): **3–4 µs p50**
- At iodepth=1, silk beats fio **2–3×** (fio pays a full OS scheduler round-trip per I/O; silk resumes the fiber inline).

---

## 14. Performance — where work-stealing earns its keep

Stall test: a fraction of requests busy-loop on the server (modeling a slow query / JSON parse / regex), pinning the executor. Can silk redistribute the *other* connections sharing a stalled CPU where epoll's per-thread reactor cannot?

| load | engine | RPS | p99 |
|---|---|---|---|
| 64% (1024 conn × 10 Hz × 1 ms) | **silk** | **1641k** | **1.2 ms** |
| 64% | epoll | 970k | 5.5 ms |
| 160% over-capacity (256 × 100 Hz × 1 ms) | **silk** | **671k** | **4.1 ms** |
| 160% | epoll | 53k | 21 ms |

- **Throughput:** silk ~1.7× at 64% load, **~12.7× at 160%** (graceful degradation vs. epoll piling up).
- **p99:** silk wins ~4.5–5×.
- **Honest caveat:** silk's *p99.9 tail* is worse (dispatch jitter accumulates across many fibers), and below saturation epoll's lower per-request cost wins on p99.

The mechanism: a silk scheduler thread idle on CPU N can drain CPU M's io_uring completion queue. A stalled epoll thread leaves its 64 connections starved until the stall ends.

---

## 15. Why stackful, not stackless (the design debate)

| Criterion | Stackless (C++20) | Stackful (silk) |
|---|---|---|
| Context switch | ~32 ns | ~109 ns (paper) / ~3.6 ns (silk w/ stealing) |
| Task creation | ~98 ns | ~40 ns (no heap frame) |
| Deep call chain overhead | O(N) — grows with depth | **O(1) — constant** |
| Third-party sync library | Requires full rewrite | **Transparent** |
| Scheduler | Build it yourself | Already in the runtime |
| Viral annotation | Yes — every level `co_await` | **No** |

- **Stackless wins** for greenfield async code where you control the whole call tree and per-coroutine memory is the binding constraint.
- **Stackful wins** for anything integrating synchronous libraries, deep call chains, or where rewriting the whole tree isn't practical.
- **HALO** (heap-elision for coroutines) only fires when the coroutine never escapes to a scheduler — i.e. never does real async work. Any real async coroutine pays the heap allocation unconditionally.

For a runtime built around io_uring + Poco HTTP, stackful is the correct choice — and the 5–13× measured win over Asio (§13) confirms the 3.5× theoretical stackless switch advantage doesn't survive contact with a real workload.

---

## 16. Utility library (the foundations that deliver §5)

Silk is built on a set of carefully chosen lock-free / per-CPU / intrusive structures (`src/util/`) — these are the building blocks behind the zero-allocation property:

- **`ShardedStack`** — per-CPU stack via **rseq** (restartable sequences). Zero atomic instructions on the fast path. Stays <6 ns up to 16 threads where a plain lock-free stack degrades to thousands of ns (up to **2000× faster** at 32 threads). Backs the `MemoryPool`.
- **`MemoryPool`** — lock-free per-CPU allocator; used **only** for `Fiber` objects (`fiberPool`). It recycles fibers rather than freeing, so each fiber keeps its one-time-`mmap`'d stack and churn triggers no repeated mmap/munmap.
- **`BoundedQueue`** — Vyukov MPMC ring; the per-CPU ready queue (holds `Fiber *`). Slots allocated once at construction, none per-op.
- **`Tree` / `LockFreeStack` / `List` / `IntrusiveQueue`** — intrusive (object embeds the hook/node); no allocation per insert. The shared ready queue is `IntrusiveQueue<Fiber, &Fiber::reservedNode>`.
- **`Tsc`** — `rdtsc`/`cntvct_el0` timing; fixed-point multiply-shift conversion, **no division on the hot path**, no syscall.

Plus: x86-64 and aarch64 (Graviton) support, libbacktrace-symbolized asserts, a structured logger, full TSan/ASan/MSan instrumentation, and a GDB extension (`fiber-list`, `fiber-switchcontext`) for inspecting suspended fibers.

---

## 17. Tooling & developer experience

One driver script, `./bb`, wraps the whole workflow:

```
./bb                       # debug build
./bb -b release            # release build
./bb -s thread test        # run tests under ThreadSanitizer
./bb -b release bench      # microbenchmarks
./bb -b release net-perf   # TCP echo benchmark, prints a Markdown table
./bb -b release perf all   # run the whole perf suite
```

Built-in benchmark harnesses compare silk head-to-head against real alternatives: `net-perf` (fibers+io_uring) vs `net-perf-asio` (Boost.Asio coroutines) vs `net-perf-epoll` (raw epoll); `file-perf` vs `fio`; an internal Poco-based HTTP server vs nginx; an S3 client vs the AWS SDK thread executor. The benchmarks *are* the credibility.

---

## 18. Honest limitations (good for a "what's next" slide)

- **Peak memory is retained.** Steady-state is zero-alloc (§5), but a burst of 100k fibers permanently holds ~7.2 GB of mmap'd stacks until teardown — the symmetric downside of pooling. Fix options: soft cap, decay reaper, or `madvise(MADV_DONTNEED)` on idle stacks.
- **p99.9 tail under heavy stall load** is worse than a per-thread epoll reactor.
- **Below saturation**, epoll's lower per-request overhead wins on latency — silk's stealing fires too rarely to amortize its cost at light load.
- Work-stealing **burns CPU** spinning when fibers are sparse (latency-for-utilization trade).

---

## Appendix A — Suggested slide outline

1. **Title** — Silk: a cooperative fiber scheduler for Linux (§1)
2. **The problem** — threads vs callbacks vs coroutines (§2)
3. **What's a fiber** — the stack picture + synchronous-looking code (§3)
4. **Architecture** — per-CPU scheduler threads diagram (§4)
5. **Zero allocation on the hot path** — intrusive structures + stack objects + allocator invariance (§5)
6. **Context switching** — the ~3.6 ns number + work-stealing insight (§6)
7. **Work-stealing** — stealing fibers *and* service loops; topology tiers (§7)
8. **io_uring async I/O** — two forms, bounded batching (§8)
9. **Synchronization primitives** — purpose-built for fibers; the FiberFuture-vs-std/folly contrast (§9)
10. **Threads ↔ fibers interop** — the whole API works from a normal thread; ProxyFiber (§10)
11. **Thread mode** — fibers borrow a dedicated worker pool (common queue) to block; one-line switch (§11)
12. **The TLS hazard** — the war story slide (§12)
13. **Performance: TCP echo** — the 5–13× asio table (§13)
14. **Performance: the honest epoll comparison** — floor vs structure (§13)
15. **Work-stealing under stall load** — the 12.7× graceful-degradation table (§14)
16. **Stackful vs stackless** — the decision table (§15)
17. **Foundations** — rseq ShardedStack, intrusive containers, TSC (§16)
18. **Tooling** — `./bb`, the benchmark suite (§17)
19. **Limitations & what's next** (§18)
20. **Closing** — when to reach for silk

## Appendix B — Soundbites / quotable lines

- "Fibers suspend rather than block their OS thread."
- "Write synchronous-looking code; the fiber suspends underneath you at any call depth."
- "Silk does not call malloc in steady state — intrusive containers, futures on the stack, and pooled fibers that each keep their stack."
- "A FiberFuture is ~16 bytes on the stack — no heap shared state, no refcount, no mutex; std::future and folly::Future both heap-allocate a refcounted control block."
- "Allocator quality is a hidden variable in async-runtime benchmarks. Silk is the one engine that takes it off the table."
- "The whole API works from a normal thread — your main(), your existing thread pools, and your fibers all speak the same synchronization vocabulary."
- "Switching between fiber and thread mode is one line: a fiber that needs to block borrows a dedicated worker thread, then returns to cooperative mode."
- "Threads in, fibers out: ProxyFiber lets threads call the fiber API; thread mode lets fibers run on a worker pool and block. The boundary is two-way and a line of code."
- "A CQE goes straight into a per-CPU lock-free ready queue, and a spinning stealer picks it up in nanoseconds via a direct register swap."
- "net-perf-epoll is the throughput floor; silk is the structure you'd actually program against."
- "The `noinline` is load-bearing, not a hint."
- "With work-stealing, the scheduler is never the bottleneck — io_uring completion latency is."

## Appendix C — Audience-tuning notes

- **For systems engineers:** lead with §4 (architecture), §5 (zero-alloc), §7 (work-stealing), §10 (ProxyFiber interop), §12 (TLS), §16 (rseq). These are the deep, novel parts.
- **For application/server developers:** lead with §3 (synchronous-looking code), §10 (drop into an existing threaded app — call the API from any thread), §11 (thread mode + library integration), §13 (perf wins). The pitch is "concurrency without the callback mess, no rewrite of your threading model."
- **For a skeptical/perf audience:** lead with §13–14 and *emphasize the honest comparisons* (raw epoll wins on tail; silk loses below saturation). Pair the speed numbers with §5's allocator-invariance argument — credibility comes from stating where silk loses, and explaining *why* it wins where it does.
- **Keep one number per slide** where possible. The strongest single number is **~12.7× throughput at 160% stall load** (graceful degradation); the strongest single design point is **stackful = transparent third-party integration + O(1) deep call chains**; the strongest *structural* claim is **zero allocation on the hot path**; and the strongest *adoption* claim is **the whole API is callable from any thread (ProxyFiber)**.
