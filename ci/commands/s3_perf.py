import json
import os
import platform
import shutil
import tempfile
from dataclasses import dataclass, field
from typing import Literal
from urllib.parse import urlparse

from ci.commands.counters import print_counters
from ci.commands.process import (
    ROOT,
    cpu_split,
    log,
    run,
    run_capture,
    start_server,
    verbose_flags,
)
from ci.commands.profiler import run_under_profiler
from ci.commands.report import latency_cells, perf_row, perf_sep, us

TOOLS_DIR = os.path.join(ROOT, ".tools")

S3Rw = Literal["read", "write", "readwrite"]


@dataclass
class S3PerfParams:
    endpoint: str = "http://127.0.0.1:9000"
    bucket: str = "test-bucket"
    key: str = "test-object"
    region: str = "us-east-1"
    access_key: str = "minioadmin"
    secret_key: str = "minioadmin"
    size: int = 4096
    duration: str = "10s"
    warmup: str = "2s"
    numjobs: list[int] = field(default_factory=lambda: [1])
    iodepth: list[int] = field(default_factory=lambda: [16])
    rw: list[S3Rw] = field(default_factory=lambda: ["read"])
    threads: bool = False
    flamegraph: bool = False
    data_dir: str = "/dev/shm/minio-data"
    print_counters: bool = False
    server_cpus: str = ""
    client_cpus: str = ""
    timeout: int = 180


_HEADERS = [
    "numjobs",
    "iodepth",
    "mode",
    "executor",
    "OPS/s",
    "avg",
    "p50",
    "p95",
    "p99",
    "p99.9",
]
_WIDTHS = [8, 8, 12, 8, 8, 8, 8, 8, 8, 8]


def _ensure_minio() -> tuple[str, str]:
    """The minio and mcli binaries from PATH, else downloaded into .tools."""
    arch = "arm64" if platform.machine() == "aarch64" else "amd64"

    def ensure(name: str, url: str) -> str:
        path = shutil.which(name)
        if path:
            return path
        local = os.path.join(TOOLS_DIR, name)
        if not (os.path.isfile(local) and os.access(local, os.X_OK)):
            os.makedirs(TOOLS_DIR, exist_ok=True)
            log.info("downloading %s -> %s", name, local)
            run("wget", "-q", "-O", local, url)
            os.chmod(local, 0o755)
        return local

    minio = ensure(
        "minio", f"https://dl.min.io/server/minio/release/linux-{arch}/minio"
    )
    mcli = ensure("mcli", f"https://dl.min.io/client/mc/release/linux-{arch}/mc")
    return minio, mcli


def _client_args(
    s3_perf: str, params: S3PerfParams, jobs: int, depth: int, mode: str
) -> list[str]:
    args = [
        s3_perf,
        "--numjobs",
        str(jobs),
        "--iodepth",
        str(depth),
        "--rw",
        mode,
        "--endpoint",
        params.endpoint,
        "--bucket",
        params.bucket,
        "--key",
        params.key,
        "--region",
        params.region,
        "--access-key",
        params.access_key,
        "--secret-key",
        params.secret_key,
        "--size",
        str(params.size),
        "--duration",
        params.duration,
        "--warmup",
        params.warmup,
    ]
    if params.threads:
        args += ["--threads"]
    return args + verbose_flags()


def _seed_object(mcli_bin: str, alias: str, params: S3PerfParams) -> None:
    """Upload a size-byte object under the key so the read modes have something to fetch."""
    with tempfile.NamedTemporaryFile(delete=False) as handle:
        handle.write(b"x" * params.size)
        seed_path = handle.name
    try:
        run(
            mcli_bin,
            "--quiet",
            "cp",
            seed_path,
            f"{alias}/{params.bucket}/{params.key}",
        )
    finally:
        os.unlink(seed_path)


def cmd_s3_perf(preset: str, params: S3PerfParams) -> None:
    print()
    print("## s3-perf -- S3 object storage")
    print()
    print(
        f"endpoint={params.endpoint}, bucket={params.bucket}, size={params.size}, "
        f"duration={params.duration}, warmup={params.warmup}"
    )
    print()

    s3_perf = os.path.join(ROOT, f"build/{preset}/bin/s3-perf")
    endpoint = urlparse(params.endpoint)
    minio_host = endpoint.hostname or "127.0.0.1"
    minio_port = endpoint.port or 9000
    alias = "bb-s3-perf"
    executor = "threads" if params.threads else "fibers"
    configs = [
        (jobs, depth, mode)
        for mode in params.rw
        for jobs in params.numjobs
        for depth in params.iodepth
    ]

    minio_bin, mcli_bin = _ensure_minio()
    server_cpus, client_cpus = cpu_split(params.server_cpus, params.client_cpus)

    os.makedirs(params.data_dir, exist_ok=True)
    minio = start_server(
        "taskset",
        "-c",
        server_cpus,
        minio_bin,
        "server",
        params.data_dir,
        "--address",
        f"{minio_host}:{minio_port}",
        "--quiet",
        host=minio_host,
        port=minio_port,
    )

    try:
        run(
            mcli_bin,
            "--quiet",
            "alias",
            "set",
            alias,
            params.endpoint,
            params.access_key,
            params.secret_key,
        )
        run(mcli_bin, "--quiet", "mb", "--ignore-existing", f"{alias}/{params.bucket}")
        if any(mode in ("read", "readwrite") for mode in params.rw):
            _seed_object(mcli_bin, alias, params)

        if params.flamegraph:
            jobs, depth, mode = configs[0]
            run_under_profiler(
                preset,
                f"s3-perf-{mode}-{executor}",
                ["taskset", "-c", client_cpus]
                + _client_args(s3_perf, params, jobs, depth, mode),
            )
        else:
            print(perf_row(_HEADERS, _WIDTHS))
            print(perf_sep(_WIDTHS))

            for jobs, depth, mode in configs:
                args = _client_args(s3_perf, params, jobs, depth, mode)
                if params.print_counters:
                    args += ["--print-counters"]
                result = run_capture(
                    "taskset", "-c", client_cpus, *args, timeout=params.timeout or None
                )
                data = json.loads(result.stdout)
                cells = [
                    jobs,
                    depth,
                    mode,
                    executor,
                    str(round(data["rps"])),
                    *latency_cells(data),
                ]
                print(perf_row(cells, _WIDTHS))
                if params.print_counters:
                    print_counters(data)
    finally:
        minio.terminate()
        minio.wait()
