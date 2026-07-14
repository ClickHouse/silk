# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Silk is a cooperative fiber scheduler for Linux: per-CPU scheduler threads pinned to cores, io_uring-based async IO, topology-aware work stealing, and fiber synchronization primitives (futures, events, mutexes, futexes, sequencers, multi-locks), plus a utility library (lock-free structures, memory pools, TSC timing, perf counters, a BPF profiler, and gdb/crash-dump tooling).

The design docs under `docs/` are the source of truth for the architecture (`scheduler.md`, `work-stealing.md`, `sync.md`, `coroutines.md`, `tls.md`, `util.md`, `perf.md`).

## Build System

`./bb` is the standard build tool — always use it instead of invoking `cmake`, `ninja`, or `ctest` directly. See `README.md` for the full command reference.

**Always build debug unless running benchmarks or sanitizer runs.**

Build presets: `debug`, `release`, `debug-{sanitizer}`, `release-{sanitizer}`. Build directories live under `build/<preset>/`.

When capturing command output to a file (e.g. tee-ing build or test output for later grepping), write to `build/tmp/` — never to the system `/tmp`. Create the directory with `mkdir -p build/tmp` if needed.

## Profiling and Flamegraphs

Generate a flamegraph by appending `--flamegraph` to a perf-target run, e.g. `bb -b release net-perf --flamegraph`. Always profile a `release` build. Output lands at `build/release/<target>.flamegraph.svg` and the raw folded stacks at `build/release/<target>.flamegraph.folded`.

The profiler is **silk's own BPF profiler** (`bin/profiler --on-cpu --off-cpu --kernel-stacks`), NOT `perf`. It walks stacks via frame pointers, so there is no `perf record --call-graph dwarf` option, and frames can be dropped (FP omission in small release functions, and at syscall boundaries).

**The folded file is a COMBINED on-CPU + off-CPU profile — each line carries TWO trailing numbers, not one:** `<semicolon;stack> <on_cpu_samples> <off_cpu_ns>`. Frame names themselves contain spaces (demangled templates), so parse the last two whitespace tokens, never `$NF` alone. On-CPU lines have `off_cpu_ns == 0`; off-CPU lines have `on_cpu_samples == 0` and end in the `schedule;__schedule;__bpf_trace_sched_switch` tail. On-CPU samples are the real CPU cost; off-CPU is blocked/wait time and is usually dominated (~99%) by idle scheduler-thread park in `parkThread;io_uring_enter2`. `bb` itself sums the two columns when rendering the SVG.

**Frame-loss caveat:** a leaf frame's self-time over-credits the deepest *surviving* frame. The clearest case: `silk::SpinLock::lockSlow` calls `sched_yield` as backoff, but the `lockSlow` frame does not survive the syscall — so its time shows mis-parented as `<caller>;sched_yield`. Attribute `sched_yield` under a lock caller back to `lockSlow`, and treat per-leaf self-time as approximate near syscalls and hot spin loops.

For aggregate rates and latencies without the frame-loss problem, append `--print-counters` instead. It prints the run config, a throughput summary, latency histograms with p50/p90/p99/p999 for the silk scheduling phases (`ready_wait`, `fiber_run`, `suspend_wait`, `cq_wait`), and the named scheduler counters (`FiberSuspended`, `FiberStolen`, `SchedulerThreadParked`/`Waked`, `SchedulerUserTime`/`SystemTime`/`IdleTime`, etc.). Use it to confirm ratios the flamegraph cannot give cleanly — e.g. park/wake balance or suspend-wait latency.

## Layout and namespaces

All code is wrapped in `namespace silk` — never `using namespace`.

Public headers live under `include/silk/<component>/` and are included as `<silk/util/...>` and `<silk/fibers/...>`. Implementation lives under `src/<component>/` alongside the matching `tests/` and `benchmarks/` subdirs; private headers (test fixtures, TU-internal helpers) sit next to their `.cpp` files.

## Performance discipline

**Silk MUST be fast — treat performance as a correctness requirement.**

- No exceptions — all code is `noexcept`; errors are errno returns (see Error handling)
- No std containers or strings in library code — use the structures under `include/silk/util/` (`List`, `IntrusiveQueue`, `BoundedQueue`, `MemoryPool`, `ShardedStack`, `Stack`, `Tree`, `Bitmap`) and `std::string_view` for borrowed ranges; std containers are OK in tests
- No allocations on a hot path — allocate at initialization; steady-state memory comes from pools and preallocated per-CPU state

## Naming and vocabulary

- When referring to a function by name in prose, comments, or docs — **never** append `()`. Write `allocate`, not `allocate()`. The `()` operator means invocation; it is not part of a name.
- Variable names must be fully descriptive — no single-letter abbreviations (`future` not `f`, `params` not `p`, `state` not `s`)
- Member variables use plain camelCase — no trailing underscores (`foo`, not `foo_`)
- Return-code variable is `int r` — never `rc`, `ret`, or `err`
- Reuse the codebase's exact identifier for a concept everywhere — don't coin synonyms or metaphors; if tempted to invent a term, ask first
- The only allowed single-letter names are `r` (return code), `b` (bool), `i` (index), `it` (iterator), `n` (count, though `count` is preferred); two-arg comparators use `left` / `right`, never `a` / `b`

## Code style and formatting

- Only ASCII characters in source files — no Unicode dashes, arrows, or other non-ASCII
- `for (;;)` not `while (true)` for infinite loops
- All `if` / `for` / `while` bodies use braces, even single-line
- Blank line before each logical block (each `if` / `for` / `while`)
- Put the common / happy path in the `if`-body — invert the condition rather than guarding the rare case in the body
- Every symbol is a distraction — no reflex casts, verbose `if` / `else`, or redundant locals
- Less code, fewer comments — cut redundant messages, comments, variables, and braces
- No inline complex expressions — lift atomic ops and aggregate-inits into named locals
- Use `std::exchange` to collapse a temp-swap-return
- Explicit types over `auto *` — the type documents the layout
- Types before functions before data inside a class
- Group third-party headers (boost / liburing / gtest / benchmark) into one include block
- Strict scope: touch only what the task names — no opportunistic refactor of adjacent code
- Prefer named functions over lambdas for anything beyond a trivial inline predicate; never write recursive `auto & self` lambdas

## Functions and APIs

- Output and borrowed params are pointers (`T *`); inputs are `const T &` — never a non-const `T &`
- Async-capable calls take a trailing `silk::FiberFuture * future = nullptr` (or `IoFuture *`); result params get semantic names, not generic ones
- Don't initialize out-param locals — the callee writes them on success
- No `/*name=*/` param comments — the IDE shows hints
- Use `SILK_UNUSED(x)` (from `<silk/util/platform.h>`, `(void)(x)`) to suppress unused-parameter warnings — never omit parameter names
- Interface overrides are declared in the class, defined out-of-line
- Leaf implementation classes are `final`
- No anonymous namespaces — use named scope; free helpers are `static`
- Shared helpers are private static class members, not free functions
- Private helper definitions go below their first caller
- Ask before any public-API change — never silently expose internals
- Trust internal callers — no defensive null-checks on internal params
- Pointer truthiness: `if (!ptr)`, never `== nullptr`
- `silk::memberOffset` over `offsetof` — type-safe, and works through an anonymous union
- No hidden release in helpers — the caller owns a borrowed resource; cleanup is visible at the call site
- `mutable` mutex members, not `const_cast`

## Error handling

- errno-only error model: `noexcept` + `int` errno returns; no `Result<T>`, no exceptions
- Convention is an `int` errno return plus a trailing `silk::Error *`, driven by the `SILK_CHECK_*` macros and `SILK_SCOPE_EXIT`
- Only the error macros push (`SILK_RETURN_ERROR` / `SILK_CHECK_ERROR` / `SILK_CHECK_BOOL`) — never call `error->push*` by hand; a bare `ENOENT` stays bare
- After a failing syscall, capture errno immediately: `int r = errno; return r;` — never return or use `errno` after any call that might clobber it
- Log format is errno first: `r=%d`, then context, then `error.format()`
- `silk::strerror` for errno strings (thread-safe; omit on nullptr)
- Don't inline cold-path helpers — `Error::push*` stays out-of-line
- Replace a `// TBD` by writing the comment — don't delete it
- No calls inside `ASSERT_*` / `EXPECT_*`, `SILK_CHECK_ERROR` / `SILK_CHECK_BOOL` — call, store in a temp, then test
- Each layer validates only its own invariants — don't pre-check in the caller what the callee already enforces
- Error / log messages name the operation that failed ("could not arm the doorbell"), not "Class::method failed"
- `SILK_ASSERT` takes a printf-style message; use `SILK_FAIL(msg, ...)` for an unconditional abort — never `SILK_ERROR` paired with `SILK_ASSERT(false)`

## Comments and doc comments

- Each method gets its own `/** */` doc comment
- Doc comments document usage — contract and wire-format, not build narrative
- Doc comments are short and precise — single line by default
- Multi-line doc block format is `/**` newline ` * text` newline ` */`
- Preserve existing `/** */` doc comments on rewrites
- Never mention an md document or a specific part of one in code — no "see scheduler.md", no "invariant 5", no "step 4"; name the concept ("the steal budget") or state the fact itself
- No backticks in C++ comments — bare identifiers
- Single dash ` - ` in comments, never ` -- `
- Delete noise comments that only paraphrase the code
- Use `/** */` block doc comments on every class / struct member, field, and nested type, and on the type itself; reserve `//` for inside function bodies
- Comments must read cold — no chat shorthand ("variant 1"), no "previously" / "as discussed", no "see above / below"; name the actual code element

## Concurrency

- `compare_exchange_weak` always — never `_strong`
- Never extend the fiber stack (keep the default 64 KiB) — fix the callee's usage instead, and ask first
- Synchronize with `future = nullptr` — don't fire-then-wait; go async only when firing many
- Use existing util primitives (`platform.h` / util), not raw syscalls
- The rseq / lock-free fast paths (`sharded-stack`, `memory-pool`, the queues) are delicate — never modify them beyond the task's explicit scope; propose first
- `SILK_ASSERT` is release-active; `SILK_ASSERT_DEBUG` is debug-only

## Testing

- `ASSERT_*`, not `EXPECT_*` — every check is a blocker
- Run `./bb test` after non-trivial changes; add a TSan run when touching atomics / concurrency
- Filter tests with `./bb test -R '<regex>'` — `bb` forwards ctest flags, so any ctest option works, but when `bb` has a built-in option for something (e.g. `--timeout`, `--coverage`), use it instead of the raw ctest flag; `--gtest_filter` never works (that is the test binary's flag, and binaries are never run directly)
- Read coverage from the Cobertura `coverage.xml` that `bb test --coverage` writes under the build dir (per-file `line-rate` / `branch-rate`, per-line `hits`, per-branch `condition-coverage`) — never hand-parse `coverage.lcov`
- `bb test --coverage` overwrites `coverage.xml` each run, so measure a component in isolation with `-R '^Suite\.'` and snapshot the XML before the next run
- Reproduce CI / timing-dependent failures only from a build matching CI's exact preset — debug timing hides races

## Performance and benchmarking

- Use `silk::Tsc::getCycles()` / `silk::Tsc::cyclesToNanoseconds()` for timing
- Throughput and latency numbers must come from a `release` build
- Run each benchmark once and update `docs/perf.md` — don't re-run just to reconfirm
- Run every benchmark target nightly — never skip one
- The `bb` "Time" suffix is reserved for true durations (e.g. ns to ms), not counts

## Build and workflow

- Default build is debug; release only for benchmarks / sanitizers
- `bb -b <preset>`, not `--preset`
- Subcommands build automatically — no manual `./bb build` first
- Build / test / bench need no confirmation — standing permission, never ask
- Never commit or push unless explicitly asked — "apply fixes" means working-tree edits only; a previous commit request is not standing permission for the next change
- Never run a compiled binary directly (not even a smoke test) — go through `./bb` (`./bb test -R <name>`, `./bb -b release net-perf`); if `bb` lacks a flag, extend `bb` rather than bypass it
- Propose the robust explicit-lifetime design, not a clever one whose correctness rests on an implicit invariant policed only by a runtime assert
- Validate an install with `<cmd> --version` (bare command name, no full path)
- Never revert someone's work without asking first
- Commit messages are a title plus one paragraph — facts only: what changed and why; no background, no discussion, no narrative
- No `Co-Authored-By` / Claude trailer in commit messages
- Self-review every changed line against these rules before submitting

## Design docs

- The docs under `docs/` are the source of truth — when a design settles or ships, update the doc, not a private note
- Prefer narrative prose with bold lead-ins and descriptive citations
- State the current fact, not the change — no temporal language ("previously" / "now" / "changed")
- One physical line per paragraph or bullet — no hard wrapping in markdown

## Dependencies

- **Boost.Context (fcontext)**: fiber context switching (`contrib/fcontext`)
- **liburing**: io_uring
- **librseq**: restartable sequences
- **libbacktrace**: symbolized stack traces
- **GTest/GMock**: unit tests
- **Google Benchmark**: microbenchmarks
- **cxxopts**: CLI parsing for perf tools
- **Poco / AWS SDK / jemalloc**: optional, perf tools only (`BUILD_POCO` / `BUILD_AWS` / `BUILD_JEMALLOC`)
