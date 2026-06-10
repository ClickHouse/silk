# Utility Library

All utilities live in `src/util/`. They are used by the fiber scheduler and its synchronization primitives but have no dependency on them.

---

## Platform (`platform.h`)

Cross-platform constants and thin OS wrappers.

| Symbol | Description |
|---|---|
| `UNUSED(x)` | Suppresses unused-variable warnings via `(void)(x)` |
| `PAGE_SIZE` | System page size (4096 bytes) |
| `CACHELINE_SIZE` | Cache line size (64 bytes) |
| `getProcessorCount()` | Number of online CPUs (`sysconf(_SC_NPROCESSORS_ONLN)`) |
| `getCurrentProcessor()` | Index of the CPU the calling thread is on (`sched_getcpu`) |
| `schedYield()` | Yield the calling thread's timeslice (`sched_yield`) |
| `cpuPause()` | CPU pause hint for spin loops |
| `getTimeNanoseconds()` | Monotonic clock in nanoseconds (`CLOCK_MONOTONIC`) |

**`cpuPause()`** emits `PAUSE` on x86-64 and `ISB` on aarch64. `ISB` is used instead of `YIELD` because `YIELD` is a NOP on non-SMT Graviton cores, providing no backoff. `ISB` flushes the pipeline and takes ~13 ns, equivalent to x86 `PAUSE`.

**Prefer `Tsc::getCycles()` over `getTimeNanoseconds()`** for hot-path timing. `getTimeNanoseconds()` calls `clock_gettime` (a syscall on some kernels); `Tsc` is a single instruction with no syscall overhead.

---

## TSC (`tsc.h`)

Reads the CPU timestamp counter and converts between cycles and nanoseconds. Frequency is detected once at startup and cached; conversions use fixed-point multiply-shift (`cycles * fp >> 20`) with no division.

```cpp
uint64_t start = Tsc::getCycles();
// ... work ...
uint64_t elapsed = Tsc::cyclesToNanoseconds(Tsc::getCycles() - start);

uint64_t deadline = Tsc::getCycles() + Tsc::nanosecondsToCycles(1'000'000); // 1 ms
```

**`getCycles()`** is `rdtsc` on x86-64 and `cntvct_el0` on aarch64. No serializing barrier is issued; instruction reordering is not a concern for deadline calculation. For precise interval measurement, bracket the reads with `lfence`.

**Frequency detection** (x86-64, in order):
1. KVM hypervisor leaf `0x40000010` EAX (kHz) -- AWS Nitro, GCP
2. CPUID leaf `0x15` TSC/crystal ratio (bare metal, Azure Hyper-V)
3. CPUID leaf `0x16` base frequency in MHz (fallback)

On aarch64, frequency comes from `cntfrq_el0`.

---

## Spin Utilities (`spinlock.h`)

**`spinWait(pred, count = 64)`** -- spins up to `count` iterations calling `cpuPause()` and checking `pred` after each. Returns `true` if `pred` returned `true`, `false` if the limit was exhausted. Default 64 iterations covers ~2 us on Skylake, roughly one OS context-switch latency.

**`SpinLock`** -- test-and-set spinlock with two-phase backoff: spin with `cpuPause()` for `SPIN_COUNT` iterations, then fall back to `schedYield()` and repeat. Conforms to `BasicLockable`. Used where fiber-aware suspension is not available (e.g. inside `MemoryPool`).

---

## LockFreeStack (`stack.h`)

Lock-free intrusive LIFO stack. ABA-safe via 128-bit tagged pointer (`CMPXCHG16B` on x86-64, `CASP LSE` on aarch64).

Objects must embed a `StackEntry` member. The typed wrapper takes a member pointer at compile time and handles object/entry conversion.

```cpp
struct MyNode {
    StackEntry stackEntry;
    int value;
};

LockFreeStack<MyNode, &MyNode::stackEntry> stack;
stack.push(&node);
MyNode * node = stack.pop();       // nullptr if empty
MyNode * list = stack.popAll();    // atomically drain entire stack
while (list) {
    MyNode * next = LockFreeStack<MyNode, &MyNode::stackEntry>::next(list);
    // process list ...
    list = next;
}
```

`popAll()` detaches the entire chain atomically. Iteration uses the static `next()` helper to follow `stackEntry.next` pointers.

---

## ShardedStack (`sharded-stack.h`)

Per-CPU intrusive LIFO stack backed by restartable sequences (rseq). The fast path (push/pop when the per-CPU list is non-empty/non-full) executes zero atomic instructions. The kernel aborts and restarts the rseq critical section on preemption or migration, so plain loads/stores suffice for per-CPU state. The slow path transfers a full batch to/from a global pool via a single 128-bit CAS, amortising its cost over `batchSize` operations.

Objects must embed a `StackEntry` member; the typed wrapper `ShardedStack` takes a member pointer as a template argument.

```cpp
struct MyNode {
    StackEntry stackEntry;
    int value;
};

ShardedStack<MyNode, &MyNode::stackEntry> stack(/*batchSize=*/64);
stack.push(&node);
MyNode * node = stack.pop(); // nullptr if empty
stack.flush();               // move per-CPU buffers to the global pool
```

**Performance** (`ShardedStackBench/PushPop`, release build, 32-CPU Intel Xeon Platinum 8488C):

| threads | ShardedStack ns/op | LockFreeStack ns/op | speedup |
|---|---|---|---|
| 1 | 4.57 | 26.9 | 5.9x |
| 2 | 4.61 | 630 | 137x |
| 4 | 4.67 | 1039 | 222x |
| 8 | 4.98 | 2369 | 476x |
| 16 | 5.53 | 6092 | 1101x |
| 32 | 10.7 | 22123 | 2068x |

`ShardedStack` stays below 6 ns up to 16 threads; `LockFreeStack` degrades rapidly because every operation issues a `CMPXCHG16B` that bounces the cache line across all contending CPUs.

---

## Queue (`queue.h`)

Lock-free MPMC FIFO queue (Michael & Scott algorithm). Not intrusive -- nodes are allocated from a shared `MemoryPool<QueueNode>` and store a `void*` value.

```cpp
Queue<MyObject> queue;
queue.enqueue(&obj);
MyObject * obj = queue.dequeue(); // nullptr if empty
```

`empty()` is a relaxed check and may transiently return `true` during a concurrent enqueue. Use it as a hint only.

---

## BoundedQueue (`bounded-queue.h`)

Bounded lock-free MPMC FIFO queue (Dmitry Vyukov sequence-number algorithm). Capacity is fixed at construction and must be a power of two. Slots are heap-allocated once; no per-operation allocation.

```cpp
BoundedQueue<Fiber *> readyQueue(1024);
bool ok = readyQueue.enqueue(fiber); // false if full
Fiber * fiber;
bool ok = readyQueue.dequeue(&fiber); // false if empty
```

Slots are cache-line aligned to prevent false sharing between the enqueue and dequeue positions. Used for the per-CPU ready queue in the fiber scheduler.

---

## Tree (`tree.h`)

Intrusive red-black tree wrapping `boost::intrusive::set` / `multiset`. All operations are O(log n).

Objects must embed a `TreeEntry` member (`boost::intrusive::set_member_hook<>`). Template parameters: element type, member pointer to hook, comparator, `AllowDuplicates` (defaults to `false`).

```cpp
struct Task {
    TreeEntry treeEntry;
    uint64_t deadline;
};

struct DeadlineCompare {
    bool operator()(const Task & a, const Task & b) const { return a.deadline < b.deadline; }
};

Tree<Task, &Task::treeEntry, DeadlineCompare> tree;
tree.insert(&task);
Task * earliest = tree.min();
tree.remove(earliest);
```

`insert()` returns `nullptr` on success; for unique trees it returns the existing element if the key is already present. `remove()` returns the in-order successor, or `nullptr` if none. Used for sleep deadline ordering in the fiber scheduler and for the ordered waiter tree in `FiberSequencer`.

---

## MemoryPool (`memory-pool.h`)

Lock-free per-CPU memory pool. Objects are allocated in 4-page chunks; freed objects go onto a per-CPU free list and are reused before a new chunk is allocated.

Objects must embed a `StackEntry` member. Pass a pointer-to-member as the second template argument to the typed wrapper.

```cpp
struct MyNode {
    StackEntry stackEntry;
    int value;
};

MemoryPool<MyNode, &MyNode::stackEntry> pool;
MyNode * node = pool.allocate(); // nullptr if OOM
pool.deallocate(node);
```

**Per-CPU design** -- the free list is a `ShardedStack`: each CPU holds a local linked list updated via rseq critical sections (zero atomic instructions on the fast path). When the list fills or empties, a batch is transferred to/from a global pool via a single 128-bit CAS. Allocation and deallocation stay local to one CPU with no contention on the fast path.

Objects are constructed once (via placement new) when first carved from a chunk, and destroyed (via `std::destroy_at`) when the pool is torn down.

---

## CPU Topology (`cpu.h`)

Reads CPU topology from sysfs and computes steal cost between two CPUs. Used by the work-stealing scheduler to order steal candidates.

```cpp
std::vector<CpuTopology> topologies(processorCount);
readCpuTopologies(topologies.data(), processorCount);
uint64_t cost = topologyCostCycles(topologies[a], topologies[b]);
```

**`readCpuTopologies()`** reads from:
- `/sys/devices/system/cpu/cpuN/topology/physical_package_id`
- `/sys/devices/system/cpu/cpuN/topology/core_id`
- `/sys/devices/system/node/nodeN/cpulist` (NUMA node membership)

Fields are left as `UINT32_MAX` for CPUs whose sysfs entries are absent (containers, some VMs).

**`topologyCostCycles()`** returns estimated steal cost in TSC cycles:

| Relationship | Cost |
|---|---|
| HT sibling (same package, same core) | ~1 us |
| Same NUMA node | ~50 us |
| Cross-NUMA (or unknown topology) | ~500 us |

---

## Assert (`assert.h`)

`ASSERT(condition[, message])` -- evaluates `condition` and, if false, prints a symbolized stack trace (via libbacktrace), the failed condition, file, line, and optional formatted message, then calls `abort()`. The condition string and location are captured at the call site; the optional message supports `std::format`-style arguments.

```cpp
ASSERT(ptr != nullptr);
ASSERT(index < size, "index {} out of range [0, {})", index, size);
```

---

## Logger (`logger.h`)

Thread-safe structured logger with a global level filter. Use the macros:

```cpp
LOG_DEBUG("fiber {} scheduled on cpu {}", fiberId, cpu);
LOG_INFO("scheduler started with {} CPUs", count);
LOG_WARN("io_uring submission queue full");
LOG_ERROR("failed to allocate fiber stack: {}", strerror(errno));
```

The level check is inlined at the call site so disabled levels have no formatting overhead. Default level is `INFO`. Change it with `Logger::setLevel(LogLevel::DEBUG)`.

---

## Sanitizer Support (`sanitizers.h`)

Macros that expand to sanitizer runtime calls when the build is instrumented, and to no-ops otherwise.

| Macro | Purpose |
|---|---|
| `TSAN_FIBER_CREATE()` | Register a new fiber with TSan |
| `TSAN_FIBER_SWITCH(fiber)` | Notify TSan of a context switch |
| `TSAN_FIBER_DESTROY(fiber)` | Deregister a fiber from TSan |
| `TSAN_ACQUIRE(addr)` / `TSAN_RELEASE(addr)` | Synthesize happens-before edges |
| `TSAN_IGNORE_BEGIN()` / `TSAN_IGNORE_END()` | Suppress TSan reports for a region |

These are called by the fiber scheduler around every `fcontext_t` switch so that TSan correctly tracks happens-before across fiber boundaries. Without them, TSan would report false positives for every access to fiber-local state.
