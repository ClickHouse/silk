import json
import os
import subprocess
from dataclasses import dataclass, field
from enum import Enum

from ci.commands.counters import print_counters, print_server_counters
from ci.commands.process import (
    ROOT,
    cpu_split,
    run_capture,
    start_server,
    verbose_flags,
)
from ci.commands.profiler import run_under_profiler
from ci.commands.report import (
    bandwidth_cell,
    latency_cells,
    perf_row,
    perf_sep,
    rps_cell,
)


class NetPerfEngine(Enum):
    """The net-perf binaries: silk fibers over io_uring, Boost.Asio C++20 coroutines, and a raw epoll loop."""

    FIBERS = "net-perf"
    ASIO = "net-perf-asio"
    EPOLL = "net-perf-epoll"


@dataclass
class NetPerfParams:
    host: str = "127.0.0.1"
    port: int = 17777
    msg_size: int = 64
    duration: str = "10s"
    warmup: str = "2s"
    connections: list[int] = field(default_factory=lambda: [1000])
    delay: str = "0"
    stall_rate: float = 0.0
    stall_duration: str = "0"
    server_cpus: str = ""
    client_cpus: str = ""
    flamegraph: bool = False
    print_counters: bool = False
    timeout: int = 180


_HEADERS = ["connections", "RPS", "BW", "avg", "p50", "p95", "p99", "p99.9"]
_WIDTHS = [11, 8, 10, 8, 8, 8, 8, 8]


def _start_local_server(
    net_perf: str, server_cpus: str, params: NetPerfParams
) -> subprocess.Popen[str]:
    args = [
        "taskset",
        "-c",
        server_cpus,
        net_perf,
        "server",
        "--host",
        params.host,
        "--port",
        str(params.port),
        "--msg-size",
        str(params.msg_size),
        "--delay",
        params.delay,
    ]
    if params.print_counters:
        args += ["--print-counters"]
    stdout = subprocess.PIPE if params.print_counters else None
    return start_server(
        *args, *verbose_flags(), host=params.host, port=params.port, stdout=stdout
    )


def _client_args(
    net_perf: str, client_cpus: str, params: NetPerfParams, connections: int
) -> list[str]:
    args = [
        "taskset",
        "-c",
        client_cpus,
        net_perf,
        "client",
        "--host",
        params.host,
        "--port",
        str(params.port),
        "--connections",
        str(connections),
        "--msg-size",
        str(params.msg_size),
        "--duration",
        params.duration,
        "--warmup",
        params.warmup,
    ]
    if params.stall_rate > 0:
        args += [
            "--stall-rate",
            str(params.stall_rate),
            "--stall-duration",
            params.stall_duration,
        ]
    return args + verbose_flags()


def cmd_net_perf(preset: str, engine: NetPerfEngine, params: NetPerfParams) -> None:
    binary = engine.value
    print()
    print(f"## {binary} -- async network I/O")
    print()
    print(
        f"{params.host}:{params.port}, msg_size={params.msg_size}, duration={params.duration}, warmup={params.warmup}, delay={params.delay}"
    )
    print()

    net_perf = os.path.join(ROOT, f"build/{preset}/bin/{binary}")
    server_cpus, client_cpus = cpu_split(params.server_cpus, params.client_cpus)

    server: subprocess.Popen[str] | None = None
    if params.host in ("127.0.0.1", "localhost"):
        server = _start_local_server(net_perf, server_cpus, params)

    try:
        if params.flamegraph:
            run_under_profiler(
                preset,
                binary,
                _client_args(net_perf, client_cpus, params, params.connections[0]),
                server.pid if server else None,
            )
        else:
            print(perf_row(_HEADERS, _WIDTHS))
            print(perf_sep(_WIDTHS))

            for connections in params.connections:
                args = _client_args(net_perf, client_cpus, params, connections)
                if params.print_counters:
                    args += ["--print-counters"]
                result = run_capture(*args, timeout=params.timeout or None)
                data = json.loads(result.stdout)
                cells = [
                    connections,
                    rps_cell(data),
                    bandwidth_cell(data),
                    *latency_cells(data),
                ]
                print(perf_row(cells, _WIDTHS))
                if params.print_counters:
                    print()
                    print("### client counters")
                    print_counters(data)
    finally:
        if server:
            server.terminate()
            server_stdout, _ = server.communicate()
            if params.print_counters:
                print_server_counters(server_stdout)
