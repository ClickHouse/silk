# Thread-Local Storage and Fibers

## Overview

silk fibers are stackful coroutines that migrate between OS threads: a fiber can suspend on one scheduler thread and resume on another (work-stealing, the SQ-ring-overflow `yield` in `enqueueIo`, ready-queue overflow to the worker pool, thread mode). Native thread-local storage assumes the opposite, that a function runs on one fixed thread for its whole duration. That mismatch makes ordinary `thread_local` access unsafe in fiber code unless you follow the rules below.

This is not a silk bug you can fully fix from inside the library: it affects any code that runs on a fiber and reads a `thread_local`, including the host application and libc. silk hardens its own context accessors (see below) and documents the rules so callers can stay correct.

Two failure modes follow from the same fiber/thread mismatch. The first, which most of this document is about, is the compiler caching the thread pointer across a suspension so a `thread_local` read lands on the previous thread. The second is per-thread state that the language runtime, the C++ ABI, or a sanitizer keeps on your behalf and that a manual stack switch does not carry: silk saves and restores that state at the switch where it can (C++ exception state), and where the toolchain offers no hook (MemorySanitizer) the resulting reports are false positives to work around. Both are covered below.

## Why it is dangerous

A `thread_local` access is "thread pointer + offset". The thread pointer lives in a fixed register (`%fs` base on x86-64, `TPIDR_EL0` on aarch64). The C++ abstract machine guarantees a thread-local's address is stable for the lifetime of the thread, so the compiler is free to read the thread pointer once, materialize it into a callee-saved register, and reuse it across function calls and loop iterations.

Fibers break that guarantee. When a fiber suspends, `switchToThreadContext` calls `jump_fcontext`, which saves the fiber's callee-saved registers as part of its context and restores them when the fiber resumes. If the fiber resumes on a **different** OS thread, the restored registers still hold the **original** thread's thread pointer. Every subsequent thread-local read in that function then addresses the previous (now idle) thread's storage, returning a stale value or null.

There is no compiler flag to disable this on GCC or Clang. MSVC has `/GT` ("fiber-safe TLS") for exactly this case; the GNU toolchain has no equivalent (see GCC PR 26461). The hazard is latent at low optimization and becomes live with more inlining, and especially with unity builds and LTO, which inline the small TLS accessors into more call sites where the thread pointer can be hoisted across a suspension. Crashes that "cannot happen" by reading the source are the usual symptom.

## The single migration point

All fiber-to-thread migration funnels through `FiberScheduler::suspend` (`switchToThreadContext` then `jump_fcontext`). User code reaches it indirectly through any call that can suspend:

- `FiberScheduler::yield`
- `FiberScheduler::read`, `write`, `poll`, `sleep`
- `FiberFuture::wait`, `waitWithTimeout`, `waitForMultiple`
- `FiberMutex::lock`, `FiberFutex::wait`, and the other synchronization primitives in `docs/sync.md`

Treat every such call as a point where the current OS thread, the current CPU index, and the value of any `thread_local` may change underneath you.

## Symptoms

- A stale read of a thread-local pointer dereferenced as null: a `SIGSEGV` at a tiny fixed address equal to a struct field offset. The canonical instance was `getCurrentFiber` returning null and the caller reading `Fiber::isProxyFiber` at address `0x9` (the field's offset in a null `Fiber`).
- A stale `getCurrentProcessor`: IO submitted to the wrong CPU's io_uring ring, which is single-producer, so a data race and ring corruption.
- Behavior that changes with optimization level, inlining, unity build, or LTO.

## Rules for fiber code

### Safe: read migration-invariant data once

A fiber's identity does not change across migration. A `Fiber *` from `getCurrentFiber` refers to the same fiber regardless of which thread runs it, so reading it once before a suspension and using the value afterwards is correct. `enqueueIo`, for example, may read the running fiber once before its retry loop.

### Unsafe: caching a thread-affine value or a TLS address across a suspension

Do not hold any of these across a call that can suspend:

- the thread pointer (`__builtin_thread_pointer`, or `getCurrentProcessor`'s rseq read),
- the address of a `thread_local`,
- a value that is meaningful only on the thread you read it on: the current CPU index, `errno`, a per-thread cache pointer, or the host application's own thread-locals (for example a current-thread or memory-accounting pointer).

Re-read it after the suspension instead. Note that re-reading the host application's per-thread state is a correctness question beyond compilation: if a fiber accounts memory against thread A then migrates to thread B, even a perfectly fresh read attributes later work to B. Code with hard per-thread affinity should not run on a migrating fiber, or should pin.

### `errno` is a trap

`errno` expands to `*__errno_location()`, and glibc marks `__errno_location` `__attribute__((const))`, so the compiler caches the returned address very aggressively, including across opaque calls. Read `errno` into a local immediately after the syscall that set it, before any suspension, and never read `errno` after a suspension expecting the syscall's value:

```cpp
ssize_t n = ::send(fd, buf, len, MSG_NOSIGNAL);
int err = errno;          // capture before any poll()/suspension
if (n < 0 && (err == EAGAIN || err == EWOULDBLOCK))
{
    poll(...);            // suspension: errno is now meaningless to us
    continue;
}
```

### `thread_local volatile` does not help

`volatile` forces the value access to be performed; it does not force the address (the thread pointer) to be recomputed. The compiler still reuses a thread pointer it has already cached, so a `volatile` thread-local read after a migration is just as stale. Do not rely on it.

### What actually triggers the caching

A single `thread_local` scalar read compiles to thread-pointer-relative addressing (`%fs:var@TPOFF`, or `TPIDR_EL0`-relative), which re-reads the thread pointer on every access and is therefore safe even across a suspension. The compiler materializes and caches the thread pointer in a callee-saved register when:

- the address of a `thread_local` escapes to a function. A `thread_local` with a non-trivial destructor registers it via `__cxa_thread_atexit`, which takes its address.
- several thread-locals are accessed together,
- a thread-local is accessed inside a loop,
- you call `__builtin_thread_pointer` explicitly.

These are exactly the shapes that occur in hot fiber code, which is why the hazard is real rather than theoretical.

## How silk makes its own context accessors safe

- `getCurrentProcessor` (`include/silk/util/platform.h`) reads the thread pointer through volatile asm (`movq %fs:0` on x86-64, `mrs ..., tpidr_el0` on aarch64) rather than `__builtin_thread_pointer`. Volatile asm cannot be hoisted out of a loop or CSE'd across a call, so the rseq CPU index is always read against the current thread.
- `getCurrentFiber` and `getCurrentFiberId` (`src/fibers/fiber.cpp`) are `__attribute__((noinline))`. Their `threadFiber` and `proxyFiber` reads happen in a fresh call activation on whichever thread is currently running the fiber, regardless of how aggressively the caller is inlined, including under unity builds and LTO. The `noinline` is load-bearing, not a hint.
- `runFiber`'s `threadFiber = fiber` and `threadFiber = nullptr` writes are deliberately left as direct accesses: `runFiber` executes on a single scheduler thread that does not migrate, and `switchToFiberContext` returns on that same thread. The fiber migrates, the scheduler thread does not.

If you add a `thread_local` to silk that fiber code reads across a suspension, apply the same treatment: read its address only through a `noinline` accessor, or read the thread pointer through `getCurrentProcessor`'s volatile-asm pattern.

## Per-thread runtime state the switch must carry

The hazard above is about `thread_local`s that your code reads. A distinct hazard is per-thread state that the language runtime or the ABI keeps for you, that no line of your code names, and that a manual stack switch does not carry unless silk explicitly moves it. This state is logically per-execution-context (it belongs to the fiber, not the thread), so silk saves it on suspend and restores it on resume, the opposite strategy from the read-fresh rule above.

**C++ exception state.** The Itanium ABI keeps the per-thread stack of in-flight and being-handled exceptions in `__cxa_eh_globals` (reached through `__cxa_get_globals`); `__cxa_throw`, `__cxa_begin_catch`, and `__cxa_end_catch` mutate it and free the exception object through it. All fibers on a scheduler thread share one copy, so an exception whose unwind or `catch` block spans a suspension would be corrupted by another fiber's throw or catch on the same thread: `__cxa_end_catch` frees the wrong exception object (a use-after-free) and leaks the original. silk gives each fiber its own exception state, saving and restoring `__cxa_eh_globals` in `switchToFiberContext` and `switchToThreadContext`, so a throw whose handling spans a suspend and resume stays correct even across a migration.

When you add new fiber-visible per-thread state, classify it the same way: read-fresh after each suspension (the thread pointer, the CPU index), save and restore it at the switch (exception state), or reset it on entry. Other candidates worth auditing before you rely on them across a suspension are the FP control and rounding word, locale, and any library that keeps a per-thread cache.

## Sanitizers and fibers

Sanitizers keep per-thread bookkeeping (shadow stacks, shadow-passing registers) in TLS and must be told about a manual stack switch. AddressSanitizer and ThreadSanitizer expose hooks for this and silk calls them; MemorySanitizer does not, and cannot be made correct on fiber code from outside.

**AddressSanitizer.** silk brackets each switch with `__sanitizer_start_switch_fiber` and `__sanitizer_finish_switch_fiber` (`src/fibers/fiber.cpp`), handing ASan the incoming fiber's stack bounds and its per-fiber fake-stack, so `detect_stack_use_after_return` and the stack redzones stay correct across suspend, resume, and migration.

**ThreadSanitizer.** each fiber gets a TSan fiber handle (`__tsan_create_fiber`) and the switch calls `__tsan_switch_to_fiber` (through the `TSAN_FIBER_*` macros in `include/silk/util/sanitizers.h`), so TSan attributes accesses and happens-before edges to the right fiber rather than mixing all fibers on a thread together.

**MemorySanitizer.** MSan has no stack-switch hook (google/sanitizers#1232). Its shadow-passing TLS (`__msan_param_tls`, `__msan_retval_tls`) does not travel with a fiber, so after a suspend and resume MSan can read a by-value argument's shadow from the wrong thread and report a spurious use-of-uninitialized-value with `ORIGIN: invalid`. This is normally arm64-only: reaching `__msan_param_tls` there materializes the thread pointer into a callee-saved register `jump_fcontext` preserves across the switch (the same mechanism as the `thread_local` hazard above), while x86-64's `%fs`-relative access always targets the running thread. The access is compiler-emitted, so the volatile-asm fix does not apply; clear the affected value's address-based shadow with `MSAN_UNPOISON` instead. `FiberMutex::lockSlow` and `lockSharedSlow` do this for their by-value `State` parameter.

**Triage.** A sanitizer report on a fiber path can be a real bug or a tool artifact: the exception-state corruption above first surfaced as an arm64 LeakSanitizer leak, while the MSan `param_tls` report is a false positive. Distinguish them by root cause before acting, and document every `MSAN_UNPOISON` or suppression with its mechanism so the next report can be classified rather than blanket-silenced. The sanitizer matrix runs on both x86-64 and aarch64 and over `test`, `bench`, and `perf`, because these bugs need real suspend, resume, and contention, and often arm64 codegen, to appear at all.

## Diagnosing a suspected TLS-migration bug

- A `SIGSEGV` at a tiny fixed address (a struct field offset) inside a thread-local accessor such as `getCurrentFiber` is the signature.
- Disassemble the function. A thread pointer read (`mov %fs:0, %rN`, or `mrs xN, tpidr_el0`) hoisted before a loop or a suspending call and then reused through a callee-saved register afterwards is the bug. A fixed accessor re-reads the thread pointer after the suspension.
- It reproduces under load: many IO-bound fibers on a small CPU set so that the `enqueueIo` SQ-ring-overflow `yield` fires and work-stealing migrates a resuming fiber. It may disappear at lower optimization or with the accessor not inlined.

## References

- GCC PR 26461, "optimisation of TLS access" / request for an option to disable caching of `__thread` addresses.
- gcc@ mailing list thread, "Disabling TLS address caching to help QEMU on GNU/Linux".
- MSVC `/GT`, "Supports fiber safety for data allocated by using static thread-local storage", the compiler switch the GNU toolchain lacks.
- google/sanitizers issue 1232, "MemorySanitizer does not support manual stack switching", the upstream request for pre/post stack-switch hooks that was closed without an API.
- AddressSanitizer `__sanitizer_start_switch_fiber` / `__sanitizer_finish_switch_fiber` and ThreadSanitizer `__tsan_switch_to_fiber` (LLVM D54889), the fiber-switch hooks MSan lacks.
