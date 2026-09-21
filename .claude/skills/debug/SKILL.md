---
name: debug
description: Reproduce and diagnose a failing, flaky or hanging silk test or run - CI failures, timeouts, sanitizer reports, crash dumps - and measure test coverage. Use when a test fails, hangs or flakes, when a run wedges, or when the user asks for coverage.
---

Find the failing job first (the check-ci skill), then reproduce it here from a build matching CI's preset. Read the evidence a run already produced before you run anything else.

## Build the same thing CI ran

`./bb -b <debug|release> -s <address|thread|undefined|memory> test -R <regex>`, with `-b` and `-s` as separate flags. Reproduce a timing-dependent failure on `release`: debug timing hides the race. `bb test` and `bb bench` forward unknown flags to ctest, so any ctest option works (`--repeat until-fail:100`, `--output-on-failure`); use the built-in option when `bb` has one (`--timeout`, `--coverage`). `--gtest_filter` never works, because test binaries are never run directly. `-N` lists the matching tests without running them.

## Where the evidence is

- The captured test output: `build/<preset>/Testing/Temporary/LastTest.log`. The next run overwrites it, so read it first.
- An assertion (`SILK_ASSERT`, `SILK_FAIL`) prints the condition, file, line, message and a libbacktrace-symbolized stack trace into that output.
- A hang produces a crash dump. Every test directory sets ctest's `TIMEOUT_SIGNAL_NAME SIGQUIT` with a 60 s grace period, and every test main and perf tool calls `silk::installCrashDumper`. On SIGQUIT or a crash signal a pre-forked dumper attaches gdb (`src/gdb/crash-dumper.py` with `fiber.py`, installed next to the binary) and writes the dump to the process's stderr; the process then cores (crash) or exits 124 (hang).

To self-dump a wedged run outside ctest: `timeout --signal=SIGQUIT <seconds> ./bb ...`.

## The gdb scripts

`src/gdb/fiber.py` teaches gdb the scheduler's data structures. The build copies it and `crash-dumper.py` next to the binaries, into `build/<preset>/bin/`. Load it in any gdb session on a silk process or core, then use its commands:

```
(gdb) source build/<preset>/bin/fiber.py
```

- `fiber-list [--proxy]` lists every fiber the scheduler knows: the `Fiber *` address, the state, the fiber-main symbol, and for a suspended fiber the suspension site. `--proxy` adds the proxy fibers that stand for non-fiber OS threads. Inspect one with `p *(silk::Fiber *) 0xADDR`.
- `fiber-savecontext`, then `fiber-switchcontext <Fiber *>` loads a SUSPENDED or READY fiber's saved registers so that `bt` shows its stack; `fiber-restorecontext` returns to the saved thread. A RUNNING fiber is on an OS thread: use `thread N` instead.
- `fiber-dump-scheduler` prints the scheduler-wide state. `fiber-dump-sleep [cpu]` prints the per-processor sleep table and the SleepFutures in each sleepTree, sleepQueue and cancelQueue with deadline, `overdueCycles`, `isSet` and waiter. `fiber-dump-uring [cpu]` prints the SQ/CQ ring counters per processor and decodes any unconsumed CQE. `fiber-dump-counters [cpu]` prints every Perf simple counter, summed across CPUs or for one CPU's slot.
- The commands resolve `silk::FiberScheduler` relative to the selected frame's compilation unit. After an attach the selected frame is in libc, so select a frame inside silk first (`thread N`, then `frame M`) when a command reports that it cannot find the scheduler.

`crash-dumper.py` is the whole dump as one script: sourcing it runs the thread dump, the fiber dump and the four state dumps in order. Use it to take the dump of a live or wedged process, or of a core, without waiting for a timeout:

```
gdb -batch -p <pid> -ex 'source build/<preset>/bin/crash-dumper.py'
gdb -batch <binary> <core> -ex 'source build/<preset>/bin/crash-dumper.py'
```

## Read a crash dump

The dump has four parts, in this order:

1. **OS threads**, grouped by identical stack, one backtrace per group with a count. Idle per-CPU scheduler threads all sit in `ProcessorState::parkThread` / `io_uring_enter2`. The main thread inside `FiberFuture::wait` / `sem_wait` is the proxy fiber's park.
2. **silk fibers** from `fiber-list`, grouped by state, fiber main and suspension site, one backtrace per group taken through `fiber-switchcontext`.
3. **Scheduler state** from `fiber-dump-scheduler`, `fiber-dump-sleep` and `fiber-dump-uring`. A sleepTree entry with positive `overdueCycles` whose future is still unset is a lost sleep wakeup. A parked processor (`sleeping=1`) with `cqReady > 0` or the OVERFLOW flag set is a park wedge: `io_uring_enter2` blocked instead of honouring its timeout, and the decoded CQE names the stuck completion.
4. **Counters** from `fiber-dump-counters`. An imbalanced pair names the failed hand-off: `FiberStarted > FiberStopped` means an injected fiber never finished; `ProxyFiberParked > ProxyFiberWaked` means an external waiter never got its wake.

A dump where every queue is empty, every ring shows `cqReady=0, sqInflt=0, sqePend=0` and the awaited fiber is in no list means the fiber was lost between inject and dispatch, or between completion and wake; the counters decide which.

## Repeat until it fails

Finish every edit before a loop starts. A build, `bb fmt` or a source edit during a loop relinks the binaries and invalidates the run.

One log per stage and per repeat under `build/tmp/`, named before the stage starts; never one shared log that a later stage overwrites. Capture with `tee`, then grep the file:

```
mkdir -p build/tmp
for i in $(seq 1 50); do
  ./bb -b release test --timeout 120 -R '<regex>' 2>&1 | tee build/tmp/flake-$i.log >/dev/null
  grep -q ' 0 tests failed' build/tmp/flake-$i.log || break
done
```

Budget `--timeout` from the test's real passing time plus its internal deadlines. The default is 180 s; a tight ceiling misreads a slow pass as a hang.

For a concurrency suite, run TSan yourself and repeat it: `./bb -s thread test -R <suite> --repeat until-fail:100`. One green run proves little, and a subagent's "clean" is a claim, not a result. Run the benchmarks under TSan too, `./bb -s thread bench -R <regex>`; they reach concurrency the unit tests do not.

TSan does not see a missing fence: a store-load reordering is an ordering bug, not a data race. Only long stress runs, a written-out interleaving and the disassembly of both halves of the handshake find it.

## Coverage

`./bb test --coverage -R '^Suite\.'` builds the `debug-coverage` preset, runs the matching tests and writes `build/debug-coverage/coverage.xml` (Cobertura) plus an HTML report under `build/debug-coverage/html/`. Read the XML: per-file `line-rate` / `branch-rate`, per-line `hits`, per-branch `condition-coverage`. Never hand-parse `coverage.lcov`. Each run overwrites the XML, so copy it to `build/tmp/` before measuring the next component.
