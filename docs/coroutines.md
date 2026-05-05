# Stackless Coroutines vs Stackful Fibers

A design and performance comparison, motivated by the question of whether C++20 stackless coroutines are a better foundation for an async server runtime than stackful fibers.

---

## Execution Model

### Stackful fibers

A fiber has a real stack -- a single heap allocation (typically 64 KB--2 MB) that holds the entire call chain. Any function at any depth can suspend; the scheduler saves the stack pointer and register state and resumes another fiber. From the programmer's perspective, code is synchronous: call a blocking function, it suspends the fiber transparently.

```
fiber stack (one allocation):
  [ handler frame        ]
  [ parseRequest frame   ]
  [ readFromSocket frame ]  <-- suspends here; scheduler runs another fiber
```

### Stackless coroutines (C++20)

A coroutine is transformed by the compiler into a heap-allocated state machine (the "frame"). The frame holds only the variables that are live across a suspension point. When a coroutine suspends, it returns to its caller immediately -- the thread stack unwinds completely back to the scheduler. Each coroutine in a call chain has its own frame.

```
heap:
  [ handler frame  ] --> [ parseRequest frame ] --> [ readFromSocket frame ]
                                                      ^-- suspends here; thread stack is empty
```

The key consequence: every function in the call chain that can suspend must itself be a coroutine. A plain function cannot propagate suspension upward. This is the "viral" property.

---

## Design Differences

### Memory layout

| | Stackful | Stackless |
|---|---|---|
| Allocation per fiber/task | One (the stack) | One per coroutine in the chain |
| Stack/frame size | Fixed upfront (e.g. 64 KB) | Exactly the live-across-suspend state |
| Deep call chain (depth N) | One allocation | N allocations |
| Memory for 100k fibers (64 KB stack) | ~6 GB virtual, much less RSS (lazy pages) | N * frame_size per active chain |

With lazy page allocation, a 64 KB fiber stack that only uses 2 KB of actual stack depth maps only ~2 KB of physical memory. The 6 GB figure is virtual address space, not RSS.

### Suspension propagation

Stackful: any callee at any depth can suspend. Third-party synchronous code works as-is.

Stackless: only `co_await` points can suspend, and only in functions that are themselves coroutines. To suspend through a call chain of depth N, all N functions must be coroutines.

Example: integrating `Poco::Net::HttpClientSession` (a stateful synchronous HTTP client).

- Stackful: call `session.sendRequest()` directly; the fiber suspends transparently inside Poco's blocking I/O without any changes to Poco.
- Stackless: every blocking call inside Poco must be replaced with a `co_await` equivalent. Poco's internal call stack must be fully rewritten as coroutines, or the blocking calls must be offloaded to a thread pool (reintroducing threads).

### The viral problem in practice

For a realistic HTTP handler calling sendRequest -> receiveResponse -> stream reads, a stackless implementation requires every level to be a coroutine:

```cpp
// Every function in the chain must be a coroutine:
Task<Response> handler()        { co_return co_await sendRequest(...); }
Task<Response> sendRequest(...) { co_return co_await receiveResponse(...); }
Task<Response> receiveResponse(...) { co_return co_await readStream(...); }
// ... down to the actual socket co_await
```

Each level is a separate heap allocation. A chain 10 levels deep = 10 heap allocations to reach the I/O operation.

### Scheduler

The C++ standard provides the coroutine machinery (`co_await`, `coroutine_handle`, promise protocol) but no scheduler, executor, or thread pool. To resume a coroutine on a different thread you must build:

- A thread-safe queue of `coroutine_handle`s
- Worker threads that call `handle.resume()`
- A wakeup/notification mechanism (eventfd, condition variable, etc.)
- io_uring or epoll integration

This is the same scheduler that a fiber runtime provides. The scheduling complexity is identical; only the unit being scheduled differs (coroutine handle vs. fiber stack pointer).

### Type safety

Both models can be made equally type-safe at the user-facing API. A stackful fiber returning a result via `FiberFuture<T>` is as statically typed as a `Task<T>` coroutine. The underlying context switch machinery is type-erased in both cases.

### HALO (heap allocation elision optimization)

The compiler can elide a coroutine's heap allocation if:
1. The coroutine's lifetime is strictly nested within the caller's lifetime
2. The coroutine handle does not escape to a scheduler queue

Condition 2 is violated by any real scheduler (work-stealing or otherwise). HALO fires only for coroutines that never leave the current thread and call stack -- which means they never reach a scheduler, which means they serve no async purpose. Any coroutine that does real async work pays the heap allocation unconditionally.

---

## Performance Data

### Context switch cost

From "Stackless vs. Stackful Coroutines: A Comparative Study" (SC '25 Workshops, 2025):
https://dl.acm.org/doi/10.1145/3731599.3767502

| | Context switch | Task creation |
|---|---|---|
| Stackless (C++20) | ~32 ns | ~98 ns |
| Stackful (Boost.Context) | ~109 ns | ~40 ns |

Stackless is 3.5x faster at switching. Stackful is 2.4x faster at task creation (no heap allocation -- just register save).

Measured on this implementation (AWS 32-CPU Intel Xeon Platinum 8488C, release build):

| | Cost |
|---|---|
| Raw Boost.Context round-trip (2 switches) | 6.55 ns -- 3.3 ns per switch |
| Full scheduler round-trip via yield, work-stealing enabled | 7.27 ns -- 3.6 ns per switch |
| Full scheduler round-trip via yield, work-stealing disabled | 126 ns per switch |

The near-zero overhead of work-stealing explains the gap with the paper's 109 ns figure. With work-stealing, a stealer thread is already spinning on another CPU when the fiber yields -- the fiber is picked up immediately with no synchronization wait. Without stealing, the scheduler must wait for the service loop to re-enqueue the fiber, which adds ~120 ns. The paper's "stackful" benchmark likely used a conventional scheduler without work-stealing.

The 7.27 ns figure reflects per-fiber latency only. Steal threads spin continuously on other CPUs, consuming real CPU even when there is no work to steal. Work-stealing trades CPU utilization for latency -- it is a win under high load where stolen work justifies the spinning, but burns CPU when fibers are sparse.

Note: context switch cost is dominated by io_uring completion latency in any real I/O workload (microseconds), making these differences immaterial in practice.

### Task creation cost

Measured on this implementation:

| | Wall time | CPU time |
|---|---|---|
| Fiber create + join, work-stealing enabled | 10.7 µs | 4.1 µs |
| Fiber create + join, work-stealing disabled | 8.4 µs | 3.7 µs |
| Thread create + join | 47.0 µs | 19.2 µs |

With work-stealing disabled, RunJoin is faster: the fiber stays on the local CPU with no cross-CPU competition. With stealing enabled, idle steal threads compete for the fiber, adding cache contention. For serial create+join workloads, stealing is pure overhead; it pays off only when there is genuine parallelism to exploit.

Fiber creation is 4.4-5.6x faster in wall time than a thread. The paper's 40 ns figure for stackful task creation is not comparable -- it likely measures only the register save, not stack allocation, scheduler enqueue, context switch, and join. The figures above cover the full round-trip.

A stackless coroutine task creation (heap frame allocation + `get_return_object`) would fall somewhere between these, but requires a scheduler round-trip of the same cost on top.

### Deep call chains

From "Stackful Coroutine Made Fast" (Alibaba Photon, October 2024):
https://photonlibos.github.io/blog-20241014/stackful-coroutine-made-fast.html

Tower of Hanoi benchmark (recursive workload, increasing call depth):

- Stackless overhead grows linearly with recursion depth, reaching **50x** at 20 levels
- Stackful (Boost.Context) maintains constant overhead at all depths
- Optimized stackful (CACS -- context-aware context switching) is **2x faster** than standard Boost.Context and massively outperforms stackless at depth

Note: Tower of Hanoi is a recursive workload that generates O(2^N) total coroutine allocations, not N. A linear handler call chain with N levels has exactly N frames alive at the suspension point. The benchmark represents a worst case for stackless; a realistic handler degrades less severely but still linearly with call depth.

### Cache locality

From the same Photon paper:

| | L1 data cache miss rate |
|---|---|
| Boost stackful | 13.08% |
| C++20 stackless | 0.01% |
| CACS-optimized stackful | ~0% |

The scattered heap frames of stackless coroutines have better cache miss rates than Boost.Context's stack-based frames in this benchmark. However, the CACS optimization eliminates the stackful cache miss problem entirely.

The 13% miss rate is a cache aliasing artifact, not an inherent property of stackful fibers. It arises when stacks are handed out from a power-of-two aligned slab allocator: all stack tops map to the same L1 cache sets. CACS fixes this by randomizing the stack entry point within each allocation.

With `mmap`-based stack allocation (4 KB alignment, ASLR), different fibers get naturally randomized virtual addresses and the aliasing problem does not arise. This implementation uses `mmap` for initial allocation and pools the allocation for reuse, so it gets both ASLR randomization (no aliasing) and no repeated `mmap`/`munmap` syscalls on fiber churn. The CACS optimization is solving a problem introduced by switching from `mmap` to a slab allocator for performance -- a tradeoff this implementation avoids.

### Heap allocation overhead per request

From "CoroBase: Coroutine-Oriented Main-Memory Database Engine" (VLDB 2021):
http://vldb.org/pvldb/vol14/p431-he.pdf

Measured **808 bytes per request** in nested coroutine frames (5+ frames in a chain), demonstrating how heap allocation cost accumulates with call depth.

### Real server workloads -- measured

TCP echo benchmark (64 B messages, loopback, 10 s measurement, 2 s warmup) run on the same machine with the same taskset CPU split. net-perf uses the fiber scheduler with io_uring; net-perf-asio is a direct C++20 coroutine reimplementation using Boost.Asio with epoll. Both use one thread per available CPU.

| connections | net-perf (fibers + io_uring) | net-perf-asio (coroutines + epoll) | ratio |
|---|---|---|---|
| 1 | 44k RPS, p50 27 µs | 3k RPS, p50 348 µs | **~15x** |
| 256 | 1522k RPS | 390k RPS | **~4x** |
| 512 | 1615k RPS | 414k RPS | **~4x** |
| 1024 | 1566k RPS | 409k RPS | **~4x** |

Switching net-perf-asio to Asio's io_uring backend (`BOOST_ASIO_HAS_IO_URING` + `BOOST_ASIO_DISABLE_EPOLL`) made things significantly worse: ~2.5x lower throughput at all connection counts. This rules out io_uring vs epoll as a factor. Asio's io_uring backend is apparently less mature than its epoll path for this workload. The bottleneck is in Asio's handler dispatch machinery, not the I/O backend.

The gap is explained by Asio's handler dispatch model: every completion -- even an "immediate" one where recv/send succeeds without EAGAIN -- is posted to a mutex-protected handler queue. A thread must lock the queue, dequeue the handler, and resume the coroutine via `coroutine_handle::resume()`. The fiber scheduler bypasses this entirely: a CQE goes directly into a per-CPU lock-free ready queue, and a spinning steal thread picks it up in nanoseconds via a direct Boost.Context register swap.

Linking with jemalloc also had no effect, ruling out allocation overhead.

The 1-connection case (15x gap) isolates pure scheduling overhead; at higher connection counts the I/O bandwidth ceiling narrows the ratio to ~4x. Neither jemalloc nor thread count adjustment (the Asio version correctly uses `sched_getaffinity` to match available CPUs) closed the gap.

For published third-party data: the SC '25 paper concludes both approaches yield nearly identical overall performance for fine-grained tasks (~17 µs duration). The anecdotal asio-grpc report (https://github.com/Tradias/asio-grpc/issues/3) found the C++20 coroutine version slower than Boost.Fiber without publishing numbers. The Photon paper reports improvements from their CACS optimization vs. their own Boost.Context baseline (not vs. stackless): HTTP +11.2%, RPC +26.2% to +45.2%.

---

## Summary

| Criterion | Stackless | Stackful |
|---|---|---|
| Context switch | ~32 ns | ~109 ns |
| Task creation | ~98 ns | ~40 ns |
| Deep call chain overhead | O(N) -- grows with depth | O(1) -- constant |
| Third-party library integration | Requires full rewrite | Transparent |
| Scheduler required | Build it yourself | Already in the runtime |
| Viral annotation | Yes -- every level must be a coroutine | No |
| Memory per fiber (100k) | N * frame_size per chain | ~6 GB virtual / much less RSS |

**When stackless wins**: greenfield async code, full control of the call tree, all dependencies already async-native, and per-coroutine memory is the binding constraint (e.g. 100k+ simple connections with shallow call chains).

**When stackful wins**: any codebase integrating synchronous third-party libraries (Poco, AWS SDK, etc.), deep call chains, or where rewriting the full call tree as coroutines is impractical. The scheduling infrastructure complexity is identical in both cases.

For a runtime built around io_uring with Poco HTTP integration, stackful fibers are the correct choice. The 3.5x context switch advantage of stackless (from the SC '25 paper) does not translate to real workloads: measured on the same TCP echo benchmark, the fiber scheduler with io_uring outperforms Boost.Asio coroutines with epoll by 4-15x. The viral annotation cost and inability to integrate synchronous libraries are additional permanent constraints that stackless cannot address.
