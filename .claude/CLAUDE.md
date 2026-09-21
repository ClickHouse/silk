# CLAUDE.md

## Communication style
- Follow ASD-STE100.

## Project

Silk is a cooperative fiber scheduler for Linux: per-CPU scheduler threads pinned to cores, io_uring-based async IO, topology-aware work stealing, and fiber synchronization primitives (futures, events, mutexes, futexes, sequencers, multi-locks), plus a utility library (lock-free structures, memory pools, TSC timing, perf counters, a BPF profiler, and gdb/crash-dump tooling).

The design docs under `docs/` are the source of truth for the architecture (`scheduler.md`, `work-stealing.md`, `sync.md`, `coroutines.md`, `tls.md`, `util.md`, `perf.md`).

## Build System

`./bb` is the standard build tool — always use it instead of invoking `cmake`, `ninja`, or `ctest` directly. See `README.md` for the full command reference.

`bb` itself is a stub that runs `ci/commands/main.py`; the commands live one module per concern under `ci/commands/` (`process.py` runs children, `cmake.py` configures / builds / tests, one `<name>_perf.py` per perf tool, `main.py` builds the argparse tree from the params dataclasses and dispatches). After editing them, `./bb fmt` (black) and `./bb lint` (mypy --strict) must pass — CI runs both.

**Always build debug unless running benchmarks or sanitizer runs.**

Build presets: `debug`, `release`, `debug-{sanitizer}`, `release-{sanitizer}`. Build directories live under `build/<preset>/`.

When capturing command output to a file (e.g. tee-ing build or test output for later grepping), write to `build/tmp/` — never to the system `/tmp`. Create the directory with `mkdir -p build/tmp` if needed. Capture with `tee`, then grep the file — never pipe a run straight into `grep` or `tail`.

The `profile` and `debug` skills under `.claude/skills/` hold the profiling, flake-reproduction, crash-dump and coverage recipes.

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
- Reuse the codebase's exact identifier for a concept everywhere — boring names composed from existing nouns, no synonyms or metaphors, no AI-slop qualifiers ("load-bearing", "key insight"); wire-to-struct is always "decode"; bare "fan" is banned, "fanout" is fine; if tempted to invent a term, ask first
- Function names contain a verb — `getX` accessors, `isX` predicates; never a bare noun (`sealed`, `entrySize`)
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
- Explicit types over `auto *` — the type documents the layout; iterator locals (`auto it = map.find(key)`) are the exception
- Types before functions before data inside a class
- Group third-party headers (boost / liburing / gtest / benchmark) into one include block
- Strict scope: touch only what the task names — no opportunistic refactor of adjacent code
- Prefer named functions over lambdas for anything beyond a trivial inline predicate; never write recursive `auto & self` lambdas
- No function bodies in a class declaration — only one-line `getX` / `isX` accessors and a pure-forward `xxxFiberMain` stay inline; constructors, `start` / `stop`, helpers and fixture `SetUp` / `TearDown` are defined out-of-line
- Class member order: `// Constants.`, `// Data structures.`, `// Fiber main functions.` (params structs and `xxxFiberMain` trampolines only), topical helper groups (each `runXxx` in its topic), then `// State.`
- `emplace_back(args...)`, never `push_back({...})`; `push_back(std::move(x))` of an existing object stays
- Fixed-width types with their limit macros (`uint32_t` + `UINT32_MAX`) — `std::numeric_limits` never appears in library code
- Integer narrowing is implicit (no `-Wconversion`) — `static_cast` only for pointer downcasts, `void *`, and enum-to-varargs
- `sizeof(Type)`, never `sizeof(object)`; arrays exempt
- Structured bindings for pair loops (`[step, count]`), not `.first` / `.second`
- Set a flag with an explicit `if`, not a bool-expression assignment
- `silk::intHash` from `platform.h` — never a hand-rolled hash constant
- Generic helpers go into the existing `include/silk/util/` header for their topic — never a file-local copy

## Functions and APIs

- Output and borrowed params are pointers (`T *`); inputs are `const T &` — never a non-const `T &`
- Async-capable calls take a trailing `silk::FiberFuture * future = nullptr` (or `IoFuture *`); result params get semantic names, not generic ones
- Don't initialize out-param locals — the callee writes them on success; no exceptions: no failure-path `INVALID_*` fills, no union-arm activation
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
- Every input pointer / view of a future-form call stays valid until the future completes — the callee never copies an input to outlive the call
- A `void start` does nothing fallible — no `SILK_ERROR`-and-continue
- An `xxxFiberMain` is a pure forward to `runXxx`, which takes the real params, never the params box; state one fiber owns is a local on its stack
- No parallel arrays — one struct per entity, mirroring the sibling side's struct
- No hidden field writes — no seal-style byte-offset mutators; pure compute plus a named-field assignment at the call site

## Error handling

- errno-only error model: `noexcept` + `int` errno returns; no `Result<T>`, no exceptions
- Convention is an `int` errno return plus a trailing `silk::Error *`, driven by the `SILK_CHECK_*` macros and `SILK_SCOPE_EXIT`
- Only the error macros push (`SILK_RETURN_ERROR` / `SILK_CHECK_ERROR` / `SILK_CHECK_BOOL`) — never call `error->push*` by hand; a bare `ENOENT` stays bare
- After a failing syscall, capture errno immediately: `int r = errno; return r;` — never return or use `errno` after any call that might clobber it
- Log format is errno first: `r=%d`, then context, then `error.format()`
- `silk::strerror` for errno strings (thread-safe; omit on nullptr)
- Don't inline cold-path helpers — `Error::push*` stays out-of-line
- Replace a `// TBD` by writing the comment — don't delete it
- No effectful calls inside `ASSERT_*` / `EXPECT_*`, `SILK_CHECK_ERROR` / `SILK_CHECK_BOOL` — call, store in a temp, then test; a trivial side-effect-free read (`size` / `empty`, atomic `load`, a plain getter, a pure computation) may stay inline
- Each layer validates only its own invariants — don't pre-check in the caller what the callee already enforces
- Allocation failure is an errno path — never `SILK_ASSERT` an allocation; a fiber spawn in a test or a one-time initialization may assert
- No error path for a race the stated invariant excludes — read the invariant at the state's writers first
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
- Use `/** */` block doc comments on every class / struct member, field, and nested type, and on the type itself — the type's own doc never catalogues its fields; reserve `//` for inside function bodies
- Comments must read cold — no chat shorthand ("variant 1"), no "previously" / "as discussed", no "see above / below"; name the actual code element
- No ownership annotations ("borrowed" / "owned") — keep only validity windows and conditional ownership
- A public `/** */` states the caller-visible contract only — never the mechanism (fibers, queues, flags); a class doc says why the class exists and its concurrency model, not a member list
- "Put a TODO" means a `// TODO:` at the call site; a tracker entry never replaces it

## Concurrency

- `compare_exchange_weak` always — never `_strong`
- Never extend the fiber stack (keep the default 64 KiB) — fix the callee's usage instead, and ask first
- Synchronize with `future = nullptr` — don't fire-then-wait; go async only when firing many
- Use existing util primitives (`platform.h` / util), not raw syscalls
- The rseq / lock-free fast paths (`sharded-stack`, `memory-pool`, the queues) are delicate — never modify them beyond the task's explicit scope; propose first
- `SILK_ASSERT` is release-active; `SILK_ASSERT_DEBUG` is debug-only
- A `silk::FiberFuture` is a single-owner completion token — one setter, one waiter; reset only under one owner with no concurrent observer; anything several fibers set, wait or observe is a `silk::FiberEvent`, never a shared future
- `stop` is cancel-and-join — no checkpoint, drain or grace; queued work completes `ECANCELED`; a graceful wait in `stop` carries its own deadline
- Teardown is block / acquire / drain / wait on per-object futures set as the completing context's last statement — no spin loops, no invented counters, flags or bits
- A future param on a serialized state machine means a pooled request, one queue and one worker fiber — never a mutex or a fiber per request; a pure IO passthrough is op + `IoFuture` + subscribe, never a worker fiber
- Cache-line regions are anonymous `struct alignas(silk::kCacheLineSize)` blocks, one per usage pattern (writer and rate), every member of a hot class inside one, the boundary stated in the region doc; verify with `-fdump-record-layouts`
- The fences in `FiberSequencer`, the mutex and the stacks record paid-for bugs — when mirroring one of these protocols, carry every fence and its pairing comment, or write out the store-buffer interleaving that proves the omission safe; TSan cannot see a missing fence
- Sanitizer detection and annotations come from `silk/util/sanitizers.h` — never hand-roll `__has_feature`
- Every `FiberScheduler` API (sleep, wait, run) is safe from a plain thread through proxy fibers — the main thread may sleep and wait directly

## Testing

- `ASSERT_*`, not `EXPECT_*` — every check is a blocker
- Test helpers and stress harnesses use `SILK_ASSERT`, C arrays for compile-time-sized collections, and thread the `silk::Error *` through
- Run `./bb test` after non-trivial changes; add a TSan run when touching atomics / concurrency — a single-threaded unit test cannot expose a race; the benchmarks under TSan (`bb -s thread bench`) reach concurrency the unit tests do not — propose that run rather than launching it
- While a test loop or matrix runs, no builds, `bb fmt`, or source edits — a relink invalidates the run
- Filter tests with `./bb test -R '<regex>'` — `bb` forwards ctest flags, so any ctest option works, but when `bb` has a built-in option for something (e.g. `--timeout`, `--coverage`), use it instead of the raw ctest flag; `--gtest_filter` never works — that is the test binary's flag
- Reproduce CI / timing-dependent failures only from a build matching CI's exact preset — debug timing hides races

## Performance and benchmarking

- Use `silk::Tsc::getCycles` / `silk::Tsc::cyclesToNanoseconds` for timing
- Throughput and latency numbers must come from a `release` build
- Run each benchmark once and update `docs/perf.md` — don't re-run just to reconfirm
- A `docs/perf.md` refresh covers every benchmark target — never skip one
- The `bb` "Time" suffix is reserved for true durations (e.g. ns to ms), not counts

## Build and workflow

- `bb -b <build> -s <sanitizer>` as separate flags — never a fused preset name, never `--preset`
- Subcommands build automatically — no manual `./bb build` first
- Build and `./bb test` need no confirmation — standing permission, never ask; bench, the perf targets and the simulator run only when asked — an unrequested run disturbs measurements in flight on this box
- Never commit or push unless explicitly asked — "apply fixes" means working-tree edits only; a previous commit request is not standing permission for the next change
- Never run a compiled binary directly (not even a smoke test) — go through `./bb` (`./bb test -R <name>`, `./bb -b release net-perf`); if `bb` lacks a flag, extend `bb` rather than bypass it
- Propose the robust explicit-lifetime design, not a clever one whose correctness rests on an implicit invariant policed only by a runtime assert
- Validate an install with `<cmd> --version` (bare command name, no full path)
- Never revert someone's work without asking first
- Commit messages are a title plus one paragraph — facts only: what changed and why; no background, no discussion, no narrative; the body wraps at 72 columns
- No `Co-Authored-By` / Claude trailer in commit messages, and no Claude attribution in PR bodies or anywhere else
- Commit granularity is semantic — a feature and its removal are one commit, a fix to uncommitted work folds into the commit it fixes; a doc or proposal never rides a code commit
- CMake stays plain — no generated `-P` scripts; optional tooling sits behind a CMake `option`; checkers are registered as ctest tests
- Self-review every changed line against these rules before submitting

## Design docs

- The docs under `docs/` are the source of truth — when a design settles or ships, update the doc, not a private note
- Prefer narrative prose with bold lead-ins and descriptive citations
- State the current fact, not the change — no temporal language ("previously" / "now" / "changed")
- One physical line per paragraph or bullet — no hard wrapping in markdown
- Fenced tables and diagrams may run to 100 columns — never squeeze them narrower at the cost of readability
- Name concepts by their C++ identifiers (`prefixCount`, `waitNs`) — never hyphenated or snake_case prose forms
- No commit SHAs in docs or PR descriptions — state the fact, name the option or the test
