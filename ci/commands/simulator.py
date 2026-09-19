import glob
import json
import os
import sys
from dataclasses import dataclass, field
from typing import Any

from ci.commands.counters import print_counters
from ci.commands.process import ROOT, log, run_capture, verbose_flags
from ci.commands.profiler import run_under_profiler
from ci.commands.report import perf_row, perf_sep

CONFIGS_DIR = os.path.join(ROOT, "src/perf/simulator/configs")


@dataclass
class SimulatorParams:
    """The fibers-simulator switches; an empty duration or warmup keeps the config's own."""

    duration: str = ""
    warmup: str = ""
    cpus: str = ""
    param: list[str] = field(default_factory=list)
    flamegraph: bool = False
    print_counters: bool = False
    disable_cpu_adjust: bool = False
    timeout: int = 180


def _resolve_config(config: str) -> str:
    """The config file for a path or a bundled config name; an unknown one lists the bundled names and fails."""
    if os.path.isfile(config):
        return config
    named = os.path.join(CONFIGS_DIR, f"{config}.cfg")
    if os.path.isfile(named):
        return named
    names = sorted(
        os.path.splitext(os.path.basename(path))[0]
        for path in glob.glob(os.path.join(CONFIGS_DIR, "*.cfg"))
    )
    log.error(
        "unknown simulator config %r; bundled configs: %s", config, ", ".join(names)
    )
    sys.exit(1)


def _latency_cell(latency: dict[str, Any], key: str) -> str:
    return f"{latency[key]:.0f} µs" if key in latency else "-"


def cmd_simulator(preset: str, config: str, params: SimulatorParams) -> None:
    config = _resolve_config(config)
    binary = os.path.join(ROOT, f"build/{preset}/bin/fibers-simulator")
    taskset = ["taskset", "-c", params.cpus] if params.cpus else []

    args = [*taskset, binary, "--config", config, *verbose_flags()]
    if params.duration:
        args += ["--duration", params.duration]
    if params.warmup:
        args += ["--warmup", params.warmup]
    for param in params.param:
        args += ["--param", param]
    if params.print_counters:
        args += ["--print-counters"]
    if params.disable_cpu_adjust:
        args += ["--disable-cpu-adjust"]

    param_note = f" ({', '.join(params.param)})" if params.param else ""
    print()
    print(f"## fibers-simulator -- {os.path.basename(config)}{param_note}")
    print()

    if params.flamegraph:
        run_under_profiler(preset, "fibers-simulator", args)
        return

    result = run_capture(*args, timeout=params.timeout or None)
    data = json.loads(result.stdout)

    print(
        f"duration={data.get('duration_s', 0):.1f}s, measured={data.get('measured_s', 0):.1f}s"
    )
    print()

    steps = data.get("steps", {})
    headers = [
        "step",
        "type",
        "executions",
        "rate/s",
        "avg",
        "p50",
        "p90",
        "p99",
        "p99.9",
        "max",
    ]
    widths = [
        max([len(headers[0]), *(len(name) for name in steps)]),
        10,
        12,
        12,
        9,
        9,
        9,
        9,
        9,
        9,
    ]
    print(perf_row(headers, widths))
    print(perf_sep(widths))

    for name, step in steps.items():
        latency = step.get("latency_us", {})
        cells = [
            name,
            step.get("type", "?"),
            f"{step.get('executions', 0):,}",
            f"{step.get('rate', 0):,.0f}",
            _latency_cell(latency, "avg"),
            _latency_cell(latency, "p50"),
            _latency_cell(latency, "p90"),
            _latency_cell(latency, "p99"),
            _latency_cell(latency, "p999"),
            _latency_cell(latency, "max"),
        ]
        print(perf_row(cells, widths))

    if params.print_counters:
        print_counters(data)
