# gdb -batch script that dumps a hung process as AGGREGATED thread + fiber stacks: identical stacks
# are grouped and shown once (with a count and one full backtrace), so the report stays readable on a
# many-threaded scheduler. The crash-dumper spawns gdb with only "-ex source <this>".

import gdb
import os
from collections import OrderedDict

gdb.execute("set pagination off")
gdb.execute("set debuginfod enabled off")

# fiber.py is installed alongside this script (the build copies both next to the binary).
_here = os.path.dirname(os.path.abspath(__file__))
_fiber_script = os.path.join(_here, "fiber.py")
gdb.execute("source " + _fiber_script)


def _frame_names(frame):
    names = []
    while frame is not None:
        names.append(frame.name() or "??")
        frame = frame.older()
    return tuple(names)


# OS threads, grouped by identical stack (the per-CPU scheduler threads are all the same).
def _dump_threads():
    groups = OrderedDict()
    for thread in gdb.selected_inferior().threads():
        thread.switch()
        signature = _frame_names(gdb.newest_frame())
        groups.setdefault(signature, []).append(thread)

    total = sum(len(threads) for threads in groups.values())
    print("\n===== OS threads: %d total, %d unique stacks =====" % (total, len(groups)))
    for threads in groups.values():
        labels = ", ".join("#%d (LWP %d)" % (t.num, t.ptid[1]) for t in threads)
        print("\n--- %d thread(s): %s ---" % (len(threads), labels))
        threads[0].switch()
        print(gdb.execute("bt", to_string=True), end="")


# fiber-list resolves silk::FiberScheduler relative to the selected frame's compilation
# unit; after attach the selected frame is in libc, so select a frame inside silk first.
def _select_silk_frame():
    for thread in gdb.selected_inferior().threads():
        thread.switch()
        frame = gdb.newest_frame()
        while frame is not None:
            name = frame.name() or ""
            if "FiberScheduler" in name or "silk::Fiber" in name:
                frame.select()
                return True
            frame = frame.older()
    return False


# Parse fiber-list, group fibers by (state, fiber-main, suspend-site), and print one full backtrace
# per group (switching into a representative suspended fiber's saved context).
def _dump_fibers():
    if not _select_silk_frame():
        print("crash-dumper: no silk frame found; skipping fiber-list")
        return

    entries = []
    for line in gdb.execute("fiber-list", to_string=True).splitlines():
        stripped = line.strip()
        if stripped.startswith("0x"):
            parts = stripped.split(None, 3)
            entries.append(
                [
                    parts[0],
                    parts[1] if len(parts) > 1 else "?",
                    parts[3] if len(parts) > 3 else "?",
                    "",
                ]
            )
        elif stripped.startswith("suspended at") and entries:
            entries[-1][3] = stripped[len("suspended at") :].strip()

    groups = OrderedDict()
    for addr, state, main, suspend in entries:
        key = (state, main, suspend)
        if key not in groups:
            groups[key] = [0, addr]
        groups[key][0] += 1

    print(
        "\n===== silk fibers: %d total, %d unique =====" % (len(entries), len(groups))
    )

    gdb.execute("fiber-savecontext")
    try:
        for (state, main, suspend), (count, addr) in groups.items():
            print("\n--- %d x  %s  %s ---" % (count, state, main))
            try:
                gdb.execute("fiber-switchcontext " + addr)
                print(gdb.execute("bt", to_string=True), end="")
            except gdb.error as error:
                print("  (no backtrace: %s)" % error)
                if suspend:
                    print("  suspended at %s" % suspend)
    finally:
        gdb.execute("fiber-restorecontext")


# Sleep + io_uring ring state, via fiber-dump-sleep / fiber-dump-uring (defined in the fiber.py sourced
# above). They read the sleepTree/queues and the SQ/CQ rings directly: a lost sleep wakeup shows as a
# sleepTree entry with positive overdueCycles still un-set; a park wedge shows as a parked proc
# (sleeping=1) with cqReady>0. Needs a silk frame selected (same reason as fiber-list) since _dump_fibers
# restored back into libc.
def _dump_sleep_state():
    if not _select_silk_frame():
        print("crash-dumper: no silk frame found; skipping sleep/uring dump")
        return

    for command in ("fiber-dump-sleep", "fiber-dump-uring"):
        print("")
        try:
            gdb.execute(command)
        except gdb.error as error:
            print("crash-dumper: %s failed: %s" % (command, error))


_dump_threads()
_dump_fibers()
_dump_sleep_state()
