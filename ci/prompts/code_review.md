# Silk code-review guidance

Project-specific guidance for the `praktika review` job. This is appended to the
fixed review protocol; keep it focused on what a generic reviewer would not know
about this repository.

## What this repo is
Silk is a cooperative fiber scheduler for Linux: per-CPU scheduler threads
pinned to cores, io_uring-based async IO, topology-aware work stealing, and
fiber synchronization primitives (futures, events, mutexes, futexes, sequencers,
multi-locks), plus a utility library (lock-free structures, memory pools, TSC
timing, perf counters, a BPF profiler). It is C++ and performance is a
correctness requirement. The design docs under `docs/` are the source of truth.

## What to prioritise
- **Concurrency correctness**: this is the heart of the project. Flag data
  races, missing or wrong memory ordering, ABA hazards, and lifetime bugs in the
  lock-free fast paths (sharded-stack, memory-pool, the queues, rseq sequences).
  compare_exchange must be the weak form. A fiber must never extend its 64 KiB
  stack. Fire-then-wait on a future is a synchronization bug; synchronize with
  `future = nullptr`.
- **Performance regressions on hot paths**: no allocations, no std containers or
  strings, no exceptions in library code. Steady-state memory comes from pools
  and preallocated per-CPU state. Flag anything that allocates, locks, or
  syscalls on a steady-state scheduling path.
- **Error handling**: the model is errno-only (`noexcept` + `int` returns, no
  exceptions, no Result<T>). After a failing syscall, errno must be captured
  immediately before any call that could clobber it. Errors propagate through the
  SILK_CHECK_* macros and a trailing `silk::Error *`; a bare errno stays bare.

## What to skip
- Formatting, naming, and lint nits - other jobs and CLAUDE.md cover these.
- Speculative refactors unrelated to the diff, and touching the delicate
  lock-free fast paths beyond the diff's explicit scope.

## Style
- Prefer a small number of high-signal findings over many low-value ones.
- When you flag something, say concretely how it fails (inputs -> wrong outcome).
