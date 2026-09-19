from dataclasses import replace

from ci.commands.cmake import cmd_build
from ci.commands.file_perf import FilePerfParams, cmd_file_perf, cmd_fio_perf
from ci.commands.http_perf import HttpPerfParams, cmd_http_perf
from ci.commands.net_perf import NetPerfEngine, NetPerfParams, cmd_net_perf
from ci.commands.s3_perf import S3PerfParams, cmd_s3_perf

# The perf targets by name, with the words the perf help lists them under.
PERF_TARGETS: dict[str, str] = {
    "file": "file-perf",
    "fio": "fio comparison",
    "net": "net-perf",
    "net-asio": "net-perf-asio",
    "net-epoll": "net-perf-epoll",
    "http": "http-perf (internal server, fibers)",
    "http-threads": "http-perf (internal server, thread client)",
    "http-nginx": "http-perf against nginx (fiber client)",
    "s3": "s3-perf (fibers)",
    "s3-threads": "s3-perf (threads)",
}

_NET_ENGINES = {
    "net": NetPerfEngine.FIBERS,
    "net-asio": NetPerfEngine.ASIO,
    "net-epoll": NetPerfEngine.EPOLL,
}


def cmd_perf(
    preset: str,
    targets: list[str],
    duration: str,
    warmup: str,
    timeout: int,
    fixed_buffers: bool,
) -> None:
    """Run the named perf targets one after another over the standard matrix of each; all runs every target."""
    selected = set(PERF_TARGETS) if "all" in targets else set(targets)

    file_params = FilePerfParams(
        numjobs=[1, 16],
        iodepth=[1, 16],
        rw=["randwrite", "randread"],
        duration=duration,
        warmup=warmup,
        timeout=timeout,
        fixed_buffers=fixed_buffers,
    )
    if "file" in selected:
        cmd_build(preset, ["file-perf"])
        cmd_file_perf(preset, file_params)
    if "fio" in selected:
        cmd_fio_perf(file_params)

    net_params = NetPerfParams(
        connections=[1, 256, 512, 1024],
        duration=duration,
        warmup=warmup,
        timeout=timeout,
    )
    for target, engine in _NET_ENGINES.items():
        if target in selected:
            cmd_build(preset, [engine.value])
            cmd_net_perf(preset, engine, net_params)

    http_params = HttpPerfParams(
        connections=[1, 256, 512, 1024],
        duration=duration,
        warmup=warmup,
        timeout=timeout,
    )
    if "http" in selected:
        cmd_build(preset, ["http-perf"])
        cmd_http_perf(preset, http_params)
    if "http-threads" in selected:
        cmd_build(preset, ["http-perf"])
        cmd_http_perf(preset, replace(http_params, threads=True))
    if "http-nginx" in selected:
        cmd_build(preset, ["http-perf"])
        cmd_http_perf(preset, replace(http_params, nginx=True))

    s3_params = S3PerfParams(
        numjobs=[1, 16],
        iodepth=[1, 64],
        rw=["read", "write"],
        duration=duration,
        warmup=warmup,
        timeout=timeout,
    )
    if "s3" in selected:
        cmd_build(preset, ["s3-perf"])
        cmd_s3_perf(preset, s3_params)
    if "s3-threads" in selected:
        cmd_build(preset, ["s3-perf"])
        cmd_s3_perf(preset, replace(s3_params, threads=True))
    print()
