"""
GDB automation script for fiber.py integration tests.

Loaded by CTest via:
  gdb --batch -q
      -ex "source src/gdb/fiber.py"
      -ex "source src/gdb/tests/autotest.py"
      ./build/debug/bin/gdb-test

fiber.py must already be sourced before this script runs.

Exits GDB with code 0 on success, 1 on any failure.
"""

import gdb
import re

# GDB Python scripts share a single interpreter namespace.  _fiber_val is
# defined by fiber.py; its absence means fiber.py was not sourced first.
if "_fiber_val" not in globals():
    print("FATAL: fiber.py was not sourced before autotest.py")
    gdb.execute("set confirm off")
    gdb.execute("quit 1")

_failures = []


def _fail(msg):
    _failures.append(msg)
    print(f"FAIL: {msg}")


def _check(condition, msg):
    if condition:
        print(f"PASS: {msg}")
    else:
        _fail(msg)


def _run_tests():
    # ── fiber-list ────────────────────────────────────────────────────────────

    out = gdb.execute("fiber-list", to_string=True)

    _check("RUNNING" in out, "fiber-list: spinner appears as RUNNING")

    n_suspended = out.count("SUSPENDED")
    # holder (waiting on g_release) + N_WAITERS (blocked on g_mutex) are all
    # SUSPENDED and visible via the per-CPU suspended lists.
    _check(
        n_suspended >= N_WAITERS + 1,
        f"fiber-list: at least {N_WAITERS + 1} SUSPENDED fibers, got {n_suspended}",
    )

    # Extract one SUSPENDED fiber address (format: "  0x<16 hex>  SUSPENDED  ...")
    suspended_ptr = None
    for line in out.splitlines():
        m = re.search(r"(0x[0-9a-f]{16})\s+SUSPENDED", line)
        if m:
            suspended_ptr = m.group(1)
            break

    _check(suspended_ptr is not None, "fiber-list: parsed a SUSPENDED Fiber* address")
    if suspended_ptr is None:
        return  # cannot continue without an address

    # ── fiber-savecontext ─────────────────────────────────────────────────────

    out = gdb.execute("fiber-savecontext", to_string=True)
    _check("saved" in out, "fiber-savecontext: reports success")

    # ── fiber-switchcontext ───────────────────────────────────────────────────

    out = gdb.execute(f"fiber-switchcontext {suspended_ptr}", to_string=True)
    _check("switched to" in out, "fiber-switchcontext: reports success")

    out = gdb.execute("bt", to_string=True)
    frames = [l for l in out.splitlines() if l.strip().startswith("#")]
    _check(
        len(frames) >= 2, f"bt in fiber context: at least 2 frames, got {len(frames)}"
    )

    # ── fiber-restorecontext ──────────────────────────────────────────────────

    out = gdb.execute("fiber-restorecontext", to_string=True)
    _check("restored" in out, "fiber-restorecontext: reports success")

    out = gdb.execute("bt", to_string=True)
    _check(
        "sleep" in out or "main" in out,
        "bt after restorecontext: original stack visible",
    )

    # ── fiber-dump-sleep ────────────────────────────────────────────────────────

    out = gdb.execute("fiber-dump-sleep", to_string=True)
    _check("silk sleep state" in out, "fiber-dump-sleep: prints the sleep-state header")
    _check("sleepTree" in out, "fiber-dump-sleep: dumps a sleepTree section")

    sleep_futures = re.findall(r"SleepFuture 0x[0-9a-f]+", out)
    _check(
        len(sleep_futures) >= N_SLEEPERS,
        f"fiber-dump-sleep: at least {N_SLEEPERS} SleepFutures parked, got {len(sleep_futures)}",
    )
    _check("IN_TABLE" in out, "fiber-dump-sleep: a parked SleepFuture is IN_TABLE (sleepTree walk works)")
    _check("waiter=fiber 0x" in out, "fiber-dump-sleep: a parked SleepFuture's waiter decodes to a Fiber*")

    # cpu filter: a single processor index restricts output to that one section.
    out = gdb.execute("fiber-dump-sleep 0", to_string=True)
    sections = re.findall(r"cpu \d+ \(number", out)
    _check(
        len(sections) == 1,
        f"fiber-dump-sleep 0: restricts output to one processor, got {len(sections)}",
    )
    _check("cpu 0 (number" in out, "fiber-dump-sleep 0: shows the requested cpu")

    out = gdb.execute("fiber-dump-sleep notanumber", to_string=True)
    _check("must be an integer" in out, "fiber-dump-sleep: rejects a non-integer cpu")

    # ── fiber-dump-uring ────────────────────────────────────────────────────────

    out = gdb.execute("fiber-dump-uring", to_string=True)
    _check("io_uring" in out, "fiber-dump-uring: prints the io_uring state header")
    _check("cqReady" in out, "fiber-dump-uring: prints the ring table header")

    ring_rows = [l for l in out.splitlines() if re.match(r"\s+\d+\s+\S+\s+[01]\s+-?\d+\s+-?\d+", l)]
    _check(len(ring_rows) >= 1, f"fiber-dump-uring: lists at least one ring, got {len(ring_rows)}")

    out = gdb.execute("fiber-dump-uring 99999", to_string=True)
    _check("out of range" in out, "fiber-dump-uring: rejects an out-of-range cpu")


# Fiber counts created by gdb-test.cpp.
N_WAITERS = 3
N_SLEEPERS = 3


class _SleepBreakpoint(gdb.Breakpoint):
    def __init__(self):
        # gdb_ready() is a dedicated noinline breakpoint target in test.cpp,
        # avoiding ambiguity with FiberScheduler::sleep vs ::sleep.
        super().__init__("gdb_ready", internal=True)
        self.silent = True

    def stop(self):
        try:
            _run_tests()
        except Exception as exc:
            _fail(f"unexpected exception in test runner: {exc}")

        n = len(_failures)
        if n:
            print(f"\n{n} test(s) FAILED")
            gdb.execute("quit 1")
        else:
            print("\nAll tests PASSED")
            gdb.execute("quit 0")

        return True  # unreachable; quit already exited GDB


gdb.execute("set pagination off")
gdb.execute("set confirm off")
_SleepBreakpoint()
gdb.execute("run")
