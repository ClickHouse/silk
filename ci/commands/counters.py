import json
from typing import Any

from ci.commands.process import log


def _fmt_ns(ns: int) -> str:
    if ns >= 1_000_000:
        return f"{ns / 1_000_000:.1f} ms"
    if ns >= 1_000:
        return f"{ns / 1_000:.1f} us"
    return f"{ns} ns"


def print_counters(data: dict[str, Any]) -> None:
    """Print a perf binary's scheduler-latency histograms, PMC block, and named counters, formatted."""
    scheduler_latency = data.get("scheduler_latency", {})
    if scheduler_latency:
        print()
        for kind_name, categories in scheduler_latency.items():
            print(f"  {kind_name}")
            for category, row in categories.items():
                print(
                    f"    {category}  count: {row['count']:>10,}"
                    f"  p50: {_fmt_ns(row['p50_ns']):>10}"
                    f"  p90: {_fmt_ns(row['p90_ns']):>10}"
                    f"  p99: {_fmt_ns(row['p99_ns']):>10}"
                    f"  p999: {_fmt_ns(row['p999_ns']):>10}"
                )

    pmc = data.get("pmc")
    if pmc:
        scaled = " (scaled)" if pmc.get("scaled") else ""
        print()
        print(f"  pmc{scaled}")
        print(f"    cycles               {pmc['cycles']:>18,}")
        print(f"    instructions         {pmc['instructions']:>18,}")
        print(f"    context_switches     {pmc['context_switches']:>18,}")
        print(f"    window_ios           {pmc['window_ios']:>18,}")
        print(f"    ipc                  {pmc['ipc']:>18.4f}")
        print(f"    cycles_per_io        {pmc['cycles_per_io']:>18,.1f}")
        print(f"    instructions_per_io  {pmc['instructions_per_io']:>18,.1f}")

    counters = data.get("counters", {})
    if not counters:
        return
    width = max(len(name) for name in counters)
    print()
    for name, value in counters.items():
        if name.endswith("Time"):
            print(f"  {name:<{width}}  {value // 1_000_000:>15,} ms")
        else:
            print(f"  {name:<{width}}  {value:>15,}")
    print()


def print_server_counters(stdout: str | None) -> None:
    """Print the counters JSON a server wrote on exit under a server heading; skip an empty or unparsable one."""
    if not stdout:
        return
    try:
        data = json.loads(stdout)
    except json.JSONDecodeError as error:
        log.warning("could not parse server counters: %s", error)
        return
    print("### server counters")
    print_counters(data)
