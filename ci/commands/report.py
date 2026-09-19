import sys
from collections.abc import Sequence
from typing import Any

from ci.commands.process import log


def parse_duration_seconds(text: str) -> float:
    """Parse a duration with an ns/us/ms/s/m suffix (a bare number is seconds) to seconds; a bad one fails the
    run.
    """
    for suffix, scale in (
        ("ns", 1e-9),
        ("us", 1e-6),
        ("ms", 1e-3),
        ("m", 60.0),
        ("s", 1.0),
    ):
        if text.endswith(suffix):
            text, factor = text[: -len(suffix)], scale
            break
    else:
        factor = 1.0

    try:
        return float(text) * factor
    except ValueError:
        log.error("could not parse the duration %r", text)
        sys.exit(1)


def us(value: Any) -> str:
    return f"{value} µs"


def rps_cell(data: dict[str, Any]) -> str:
    """A perf binary's rps in thousands."""
    return f"{round(data['rps'] / 1000)}k"


def bandwidth_cell(data: dict[str, Any]) -> str:
    """A perf binary's bw_bytes in MiB/s."""
    return f"{round(data['bw_bytes'] / (1024 * 1024)):.1f} MiB/s"


def latency_cells(data: dict[str, Any]) -> list[str]:
    """The avg, p50, p95, p99, and p99.9 cells of a perf binary's latency_us block."""
    latency = data["latency_us"]
    return [us(latency[key]) for key in ("avg", "p50", "p95", "p99", "p99_9")]


def perf_row(cells: Sequence[object], widths: list[int]) -> str:
    return (
        "".join(f"| {str(cell):<{width}} " for cell, width in zip(cells, widths)) + "|"
    )


def perf_sep(widths: list[int]) -> str:
    return "|" + "|".join("-" * (width + 2) for width in widths) + "|"
