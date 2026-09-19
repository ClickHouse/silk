import json
import os
import subprocess
from dataclasses import dataclass, field

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
    latency_cells,
    parse_duration_seconds,
    perf_row,
    perf_sep,
    rps_cell,
)

TMP_DIR = os.path.join(ROOT, "build/tmp")


@dataclass
class HttpPerfParams:
    host: str = "127.0.0.1"
    port: int = 18080
    duration: str = "10s"
    warmup: str = "2s"
    connections: list[int] = field(default_factory=lambda: [1000])
    delay: str = "0"
    threads: bool = False
    nginx: bool = False
    flamegraph: bool = False
    print_counters: bool = False
    server_cpus: str = ""
    client_cpus: str = ""
    timeout: int = 180


_HEADERS = ["connections", "mode", "RPS", "avg", "p50", "p95", "p99", "p99.9"]
_WIDTHS = [11, 8, 8, 8, 8, 8, 8, 8]

_NGINX_CONF = """\
{load_modules}worker_processes {workers};
pid {pid_file};
error_log /dev/null;
events {{ worker_connections 4096; }}
http {{
    access_log off;
    server {{
        listen {port};
        location / {{
            {handler}
        }}
    }}
}}
"""


def _start_nginx_server(
    params: HttpPerfParams, server_cpus: str
) -> subprocess.Popen[str]:
    """Start nginx with one worker per server CPU half, sleeping the delay per request through lua."""
    delay_seconds = parse_duration_seconds(params.delay)
    if delay_seconds > 0:
        load_modules = "load_module modules/ndk_http_module.so;\nload_module modules/ngx_http_lua_module.so;\n"
        handler = f"content_by_lua_block {{ ngx.sleep({delay_seconds}); ngx.exit(ngx.HTTP_OK); }}"
    else:
        load_modules = ""
        handler = "return 200;"

    os.makedirs(TMP_DIR, exist_ok=True)
    conf_path = os.path.join(TMP_DIR, "http-perf-nginx.conf")
    with open(conf_path, "w") as handle:
        handle.write(
            _NGINX_CONF.format(
                port=params.port,
                workers=(os.cpu_count() or 2) // 2,
                handler=handler,
                load_modules=load_modules,
                pid_file=os.path.join(TMP_DIR, "http-perf-nginx.pid"),
            )
        )

    return start_server(
        "taskset",
        "-c",
        server_cpus,
        "nginx",
        "-c",
        conf_path,
        "-g",
        "daemon off;",
        host=params.host,
        port=params.port,
    )


def _start_internal_server(
    preset: str, params: HttpPerfParams, server_cpus: str
) -> subprocess.Popen[str]:
    http_perf = os.path.join(ROOT, f"build/{preset}/bin/http-perf")
    args = [
        "taskset",
        "-c",
        server_cpus,
        http_perf,
        "server",
        "--port",
        str(params.port),
    ]
    if parse_duration_seconds(params.delay) > 0:
        args += ["--delay", params.delay]
    if params.print_counters:
        args += ["--print-counters"]
    stdout = subprocess.PIPE if params.print_counters else None
    return start_server(
        *args, *verbose_flags(), host=params.host, port=params.port, stdout=stdout
    )


def _client_args(
    http_perf: str, client_cpus: str, params: HttpPerfParams, connections: int
) -> list[str]:
    args = [
        "taskset",
        "-c",
        client_cpus,
        http_perf,
        "client",
        "--host",
        params.host,
        "--port",
        str(params.port),
        "--connections",
        str(connections),
        "--duration",
        params.duration,
        "--warmup",
        params.warmup,
    ]
    if params.threads:
        args += ["--threads"]
    return args + verbose_flags()


def cmd_http_perf(preset: str, params: HttpPerfParams) -> None:
    mode = "threads" if params.threads else "fibers"
    server_kind = "nginx" if params.nginx else "internal"
    print()
    print(f"## http-perf (server={server_kind}, client={mode}) -- HTTP/1.1 GET")
    print()
    print(f"duration={params.duration}, warmup={params.warmup}, delay={params.delay}")
    print()

    server_cpus, client_cpus = cpu_split(params.server_cpus, params.client_cpus)
    if params.nginx:
        server = _start_nginx_server(params, server_cpus)
    else:
        server = _start_internal_server(preset, params, server_cpus)

    http_perf = os.path.join(ROOT, f"build/{preset}/bin/http-perf")

    try:
        if params.flamegraph:
            run_under_profiler(
                preset,
                "http-perf-" + mode,
                _client_args(http_perf, client_cpus, params, params.connections[0]),
            )
        else:
            print(perf_row(_HEADERS, _WIDTHS))
            print(perf_sep(_WIDTHS))

            for connections in params.connections:
                args = _client_args(http_perf, client_cpus, params, connections)
                if params.print_counters:
                    args += ["--print-counters"]
                result = run_capture(*args, timeout=params.timeout or None)
                data = json.loads(result.stdout)
                cells = [connections, mode, rps_cell(data), *latency_cells(data)]
                print(perf_row(cells, _WIDTHS))
                if params.print_counters:
                    print()
                    print("### client counters")
                    print_counters(data)
    finally:
        server.terminate()
        server_stdout, _ = server.communicate()
        if params.print_counters and not params.nginx:
            print_server_counters(server_stdout)
