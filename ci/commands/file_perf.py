import json
import os
from dataclasses import dataclass, field
from typing import Any, Literal

from ci.commands.counters import print_counters
from ci.commands.process import ROOT, run_capture, verbose_flags
from ci.commands.profiler import run_under_profiler
from ci.commands.report import (
    bandwidth_cell,
    latency_cells,
    parse_duration_seconds,
    perf_row,
    perf_sep,
    rps_cell,
    us,
)

FileRw = Literal["randread", "randwrite", "seqread"]


@dataclass
class FioPerfParams:
    """The fio job both fio-perf and file-perf run."""

    file: str = "/dev/shm/file-perf.bin"
    bs: str = "4k"
    size: str = "1g"
    duration: str = "10s"
    warmup: str = "2s"
    numjobs: list[int] = field(default_factory=lambda: [1])
    iodepth: list[int] = field(default_factory=lambda: [16])
    rw: list[FileRw] = field(default_factory=lambda: ["randread"])
    timeout: int = 180


@dataclass
class FilePerfParams(FioPerfParams):
    """The fio job plus the switches only silk's file-perf takes."""

    flamegraph: bool = False
    print_counters: bool = False
    fixed_buffers: bool = False


_HEADERS = [
    "numjobs",
    "iodepth",
    "mode",
    "IOPS",
    "BW",
    "avg",
    "p50",
    "p95",
    "p99",
    "p99.9",
]
_WIDTHS = [8, 8, 10, 8, 10, 8, 8, 8, 8, 8]


def _configs(params: FioPerfParams) -> list[tuple[int, int, str]]:
    """Every (numjobs, iodepth, mode) row of the matrix, mode-major."""
    return [
        (jobs, depth, mode)
        for mode in params.rw
        for jobs in params.numjobs
        for depth in params.iodepth
    ]


def _print_heading(title: str, params: FioPerfParams) -> None:
    print()
    print(f"## {title}")
    print()
    print(
        f"file={params.file}, bs={params.bs}, size={params.size}, duration={params.duration}, warmup={params.warmup}"
    )
    print()


def _file_perf_args(
    file_perf: str, params: FilePerfParams, jobs: int, depth: int, mode: str
) -> list[str]:
    args = [
        file_perf,
        "--numjobs",
        str(jobs),
        "--iodepth",
        str(depth),
        "--bs",
        params.bs,
        "--rw",
        mode,
        "--size",
        params.size,
        "--runtime",
        params.duration,
        "--warmup",
        params.warmup,
        "--filename",
        params.file,
    ]
    if params.fixed_buffers:
        args += ["--fixed-buffers"]
    return args + verbose_flags()


def cmd_file_perf(preset: str, params: FilePerfParams) -> None:
    configs = _configs(params)
    _print_heading("file-perf -- async file I/O", params)
    file_perf = os.path.join(ROOT, f"build/{preset}/bin/file-perf")

    try:
        if params.flamegraph:
            jobs, depth, mode = configs[0]
            run_under_profiler(
                preset,
                "file-perf",
                _file_perf_args(file_perf, params, jobs, depth, mode),
            )
        else:
            print(perf_row(_HEADERS, _WIDTHS))
            print(perf_sep(_WIDTHS))

            for jobs, depth, mode in configs:
                args = _file_perf_args(file_perf, params, jobs, depth, mode)
                if params.print_counters:
                    args += ["--print-counters"]
                result = run_capture(*args, timeout=params.timeout or None)
                data = json.loads(result.stdout)
                cells = [
                    jobs,
                    depth,
                    mode,
                    rps_cell(data),
                    bandwidth_cell(data),
                    *latency_cells(data),
                ]
                print(perf_row(cells, _WIDTHS))
                if params.print_counters:
                    print_counters(data)
    finally:
        if os.path.exists(params.file):
            os.unlink(params.file)


def _fio_cells(data: dict[str, Any], mode: str) -> list[str]:
    """The IOPS, BW, and latency cells of fio's JSON for the side mode exercises."""
    side = "write" if "write" in mode else "read"
    job = data["jobs"][0][side]
    clat_ns = job["clat_ns"]
    percentiles: dict[str, float] = clat_ns.get("percentile", {})

    def percentile_us(key: str) -> str:
        return us(round(percentiles.get(key, 0) / 1000, 2))

    return [
        f"{round(job['iops'] / 1000)}k",
        f"{round(job['bw_bytes'] / (1024 * 1024))} MiB/s",
        us(round(clat_ns["mean"] / 1000, 2)),
        percentile_us("50.000000"),
        percentile_us("95.000000"),
        percentile_us("99.000000"),
        percentile_us("99.900000"),
    ]


def cmd_fio_perf(params: FioPerfParams) -> None:
    configs = _configs(params)
    _print_heading("fio comparison (io_uring)", params)

    try:
        print(perf_row(_HEADERS, _WIDTHS))
        print(perf_sep(_WIDTHS))

        for jobs, depth, mode in configs:
            result = run_capture(
                "fio",
                "--name=bench",
                "--ioengine=io_uring",
                f"--iodepth={depth}",
                f"--numjobs={jobs}",
                f"--bs={params.bs}",
                f"--rw={mode}",
                f"--size={params.size}",
                f"--runtime={int(parse_duration_seconds(params.duration))}",
                f"--ramp_time={int(parse_duration_seconds(params.warmup))}",
                "--time_based",
                "--fallocate=native",
                f"--filename={params.file}",
                "--group_reporting",
                "--output-format=json",
                timeout=params.timeout or None,
            )
            cells = [jobs, depth, mode, *_fio_cells(json.loads(result.stdout), mode)]
            print(perf_row(cells, _WIDTHS))
    finally:
        if os.path.exists(params.file):
            os.unlink(params.file)
