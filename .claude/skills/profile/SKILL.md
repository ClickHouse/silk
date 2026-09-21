---
name: profile
description: Profile a silk perf target and read the result - flamegraph, folded stacks, scheduler-latency histograms, counters, paired runs against a baseline. Use when the user asks why a run is slow, wants a flamegraph or counters, or needs a throughput or latency number.
---

Profile a release build only. Numbers for the docs come from a release build, one run per point, and go into docs/perf.md; do not re-run to reconfirm.

## Generate a flamegraph

Append `--flamegraph` to a perf-target run:

```
./bb -b release net-perf --flamegraph
```

The SVG lands at `build/release/<target>.flamegraph.svg`, the raw folded stacks at `build/release/<target>.flamegraph.folded`.

The profiler is silk's own BPF profiler (`bin/profiler --on-cpu --off-cpu --kernel-stacks`), not `perf`. It walks stacks by frame pointers, so there is no `--call-graph dwarf`, and frames can be dropped: frame-pointer omission in small release functions, and at syscall boundaries.

## Read the folded file

Each line is a combined on-CPU + off-CPU sample with TWO trailing numbers: `<frame;frame;...> <on_cpu_ns> <off_cpu_ns>`. Frame names contain spaces (demangled templates), so parse the last two whitespace tokens, never `$NF` alone.

- On-CPU lines have `off_cpu_ns == 0`. Their sum is the real CPU cost.
- Off-CPU lines have `on_cpu_ns == 0` and end in `schedule;__schedule;__bpf_trace_sched_switch`. This is blocked time, about 99% of it idle scheduler-thread park in `parkThread;io_uring_enter2`. Exclude it before ranking.
- `bb` sums both columns when it renders the SVG, so the SVG mixes CPU time and wait time.

Top self-time leaves as a share of all on-CPU time:

```
awk '$NF == 0 { on = $(NF-1); stack = $0; sub(/ [0-9]+ [0-9]+$/, "", stack); n = split(stack, frame, ";"); self[frame[n]] += on; total += on }
     END { for (leaf in self) printf "%6.2f%%  %s\n", 100 * self[leaf] / total, leaf }' \
    build/release/<target>.flamegraph.folded | sort -rn | head -30
```

Inclusive time of one frame: `grep -F '<Name>' <folded> | awk '$NF == 0 { sum += $(NF-1) } END { print sum }'`.

**Frame loss mis-parents self time onto the deepest surviving frame.** `silk::SpinLock::lockSlow` calls `sched_yield` as backoff, but the `lockSlow` frame does not survive the syscall, so its time shows as `<caller>;sched_yield`. Attribute `sched_yield` under a lock caller back to `lockSlow`. Treat per-leaf self time as approximate near syscalls and hot spin loops.

## Counters and latency histograms

Append `--print-counters` instead for aggregate rates and latencies without the frame-loss problem. It prints the run config, a throughput summary, log2 latency histograms with p50/p90/p99/p999 per scheduling phase (`suspend_wait`, `io_wait`, `sq_wait`, `submit_io`, `cq_wait`, `ready_wait`, `fiber_run`; the "Latency profiler" section of docs/perf.md defines each interval), a `pmc` block (cycles, instructions, context switches, IPC, cycles and instructions per IO), and the named scheduler counters (`FiberSuspended`, `FiberStolen`, `SchedulerThreadParked` / `Waked`, `SchedulerUserTime` / `SystemTime` / `IdleTime`, ...). Use it to confirm ratios the flamegraph cannot give cleanly: park/wake balance, suspend-wait latency, cost per IO.

`--print-counters` samples every operation and lowers throughput. Take throughput and latency numbers from a plain run, and the histograms and counters from a second run with the flag.

## Paired runs against a baseline

Build the control with a stash roundtrip in the same build tree, never with a worktree: a worktree forces a submodule checkout and a cold build.

1. Copy the modified files to `build/tmp/wt-backup/`.
2. `git stash push -m <name>`, build and run the control, capture the output under `build/tmp/`.
3. `git stash pop`, then `cmp` every modified file against its backup.

Run control and candidate back to back with the same flags and duration.
