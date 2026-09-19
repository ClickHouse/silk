import argparse
import logging
import resource
import signal
import subprocess
import sys
from dataclasses import MISSING, Field, fields
from typing import Any, Literal, TypeVar, get_args, get_origin

from ci.commands.bench import cmd_bench
from ci.commands.cmake import (
    BuildParams,
    cmd_build,
    cmd_clean,
    cmd_configure,
    cmd_test,
)
from ci.commands.file_perf import (
    FilePerfParams,
    FioPerfParams,
    cmd_file_perf,
    cmd_fio_perf,
)
from ci.commands.format import cmd_fmt
from ci.commands.http_perf import HttpPerfParams, cmd_http_perf
from ci.commands.lint import cmd_lint
from ci.commands.net_perf import NetPerfEngine, NetPerfParams, cmd_net_perf
from ci.commands.perf import PERF_TARGETS, cmd_perf
from ci.commands.process import log
from ci.commands.s3_perf import S3PerfParams, cmd_s3_perf
from ci.commands.simulator import SimulatorParams, cmd_simulator

SANITIZERS: dict[str, str] = {
    "thread": "tsan",
    "address": "asan",
    "undefined": "ubsan",
    "memory": "msan",
}

# The help of the flags every perf command shares, by field.
_PERF_HELP = {
    "duration": "measurement duration (e.g. 30s, 1m)",
    "warmup": "warmup before measuring (e.g. 5s)",
    "timeout": "per-run timeout in seconds, 0 for none",
    "flamegraph": "run under silk's BPF profiler and render an on-CPU + off-CPU flamegraph",
    "print_counters": "print the scheduler-latency histograms and counters after each run",
    "server_cpus": "taskset CPU list for the server (default: the lower half of the machine)",
    "client_cpus": "taskset CPU list for the client (default: the upper half of the machine)",
    "numjobs": "parallel jobs; several values run one matrix row each",
    "iodepth": "IO queue depth per job; several values run one matrix row each",
    "connections": "connections to open; several values run one table row each",
    "host": "server host",
    "port": "server port",
}

FILE_PERF_HELP = {
    **_PERF_HELP,
    "file": "test file path, removed after the run",
    "bs": "block size (e.g. 4k, 64k)",
    "size": "file size (e.g. 1g)",
    "rw": "access modes; several values run one matrix row each",
    "fixed_buffers": "use registered buffers (IORING_OP_READ_FIXED / WRITE_FIXED)",
}

NET_PERF_HELP = {
    **_PERF_HELP,
    "host": "server host; a remote host skips starting the local server",
    "msg_size": "echo message size in bytes",
    "delay": "server-side delay per message (e.g. 1ms, 100us)",
    "stall_rate": "per-connection Poisson rate of stall messages in Hz, 0 disables",
    "stall_duration": "stall duration per stall event (e.g. 100us, 1ms)",
}

HTTP_PERF_HELP = {
    **_PERF_HELP,
    "delay": "server-side delay per request (e.g. 1ms, 100us)",
    "threads": "thread-per-connection client instead of fibers",
    "nginx": "run the client against nginx instead of the internal server",
}

S3_PERF_HELP = {
    **_PERF_HELP,
    "endpoint": "S3 endpoint URL; a local MinIO is started there",
    "bucket": "S3 bucket",
    "key": "S3 object key",
    "region": "S3 region",
    "access_key": "S3 access key",
    "secret_key": "S3 secret key",
    "size": "object size in bytes",
    "rw": "access modes; several values run one matrix row each",
    "threads": "thread executor instead of fibers",
    "data_dir": "MinIO data directory",
}

SIMULATOR_HELP = {
    **_PERF_HELP,
    "duration": "override the config's run duration",
    "warmup": "override the config's warmup",
    "cpus": "taskset CPU list for the run (default: no pinning)",
    "param": "override a config param; repeatable, comma-separated pairs allowed",
    "print_counters": "print the scheduler-latency histograms and counters after the run",
    "disable_cpu_adjust": "pin the scheduler CPU width at full (static-width baselines)",
}

# The list fields whose flag takes one value per occurrence, so a positional may follow it.
_ONE_VALUE_PER_FLAG = {"param"}

# The metavar of a flag, by field; the upper-cased field name otherwise.
_METAVARS = {
    "file": "PATH",
    "data_dir": "PATH",
    "bs": "SIZE",
    "size": "SIZE",
    "duration": "DURATION",
    "warmup": "DURATION",
    "delay": "DURATION",
    "stall_duration": "DURATION",
    "numjobs": "N",
    "iodepth": "N",
    "connections": "N",
    "msg_size": "BYTES",
    "server_cpus": "CPUS",
    "client_cpus": "CPUS",
    "cpus": "CPUS",
    "timeout": "SECONDS",
    "stall_rate": "HZ",
    "param": "NAME=VALUE",
    "endpoint": "URL",
    "bucket": "NAME",
    "key": "NAME",
    "region": "NAME",
    "access_key": "KEY",
    "secret_key": "KEY",
}


def _configure_logging() -> None:
    logging.addLevelName(logging.DEBUG, "DEBUG")
    logging.addLevelName(logging.INFO, "INFO ")
    logging.addLevelName(logging.WARNING, "WARN ")
    logging.addLevelName(logging.ERROR, "ERROR")
    logging.basicConfig(
        format="%(asctime)s.%(msecs)03d [%(levelname)s] %(filename)s:%(lineno)d: %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
        level=logging.INFO,
    )


def _check_no_extra(extra: list[str]) -> None:
    if extra:
        log.error("unexpected arguments: %s", " ".join(extra))
        sys.exit(1)


Params = TypeVar("Params")


def _params(args: argparse.Namespace, cls: type[Params]) -> Params:
    """The params the parser stored under params_; a flag not given keeps the dataclass default."""
    values = {
        key[len("params_") :]: value
        for key, value in vars(args).items()
        if key.startswith("params_") and value is not None
    }
    return cls(**values)


def _help_text(field: Field[Any], text: str) -> str:
    """The field's help with its dataclass default appended, unless the default is empty or off."""
    if field.default_factory is not MISSING:
        default = field.default_factory()
    else:
        default = field.default
    if isinstance(default, list):
        default = " ".join(str(item) for item in default)
    if not default:
        return text
    return f"{text} (default: {default})"


def _add_params_args(
    parser: argparse.ArgumentParser, cls: type[Any], help_texts: dict[str, str]
) -> None:
    """One --flag per field of cls, stored under params_: a bool is a switch, a list takes several values (or
    one per occurrence) and repeats, a Literal restricts the choices; a flag not given leaves None so the
    dataclass default applies.
    """
    for field in fields(cls):
        flag = "--" + field.name.replace("_", "-")
        dest = "params_" + field.name
        help_text = _help_text(field, help_texts[field.name])
        if field.type is bool:
            parser.add_argument(flag, dest=dest, action="store_true", help=help_text)
            continue

        kwargs: dict[str, Any] = {"dest": dest, "help": help_text, "default": None}
        value_type = field.type
        if get_origin(value_type) is list:
            (value_type,) = get_args(value_type)
            if field.name in _ONE_VALUE_PER_FLAG:
                kwargs["action"] = "append"
            else:
                kwargs.update(nargs="+", action="extend")
        if get_origin(value_type) is Literal:
            kwargs["choices"] = get_args(value_type)
            value_type = str
        if "choices" not in kwargs:
            kwargs["metavar"] = _METAVARS.get(field.name, field.name.upper())
        parser.add_argument(flag, type=value_type, **kwargs)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="bb", allow_abbrev=False)
    parser.add_argument(
        "-b",
        "--build",
        default="debug",
        choices=["debug", "release"],
        help="build type (default: debug)",
    )
    parser.add_argument(
        "-s",
        "--sanitizer",
        choices=sorted(SANITIZERS.keys()),
        help="enable a sanitizer build",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="print every command before running it and pass --verbose to the perf binaries",
    )

    sub = parser.add_subparsers(dest="command")

    sub.add_parser("clean", help="remove build/ directory")

    fmt_parser = sub.add_parser(
        "fmt", help="format the C++ sources with clang-format and the Python with black"
    )
    fmt_parser.add_argument(
        "--check", action="store_true", help="check formatting without modifying files"
    )

    sub.add_parser("lint", help="type-check the Python sources with mypy")

    configure_parser = sub.add_parser("configure", help="run CMake configure step only")
    configure_parser.add_argument(
        "--build-poco",
        dest="poco",
        action="store_true",
        help="enable Poco library (used by http-perf)",
    )
    configure_parser.add_argument(
        "--build-aws",
        dest="aws",
        action="store_true",
        help="enable AWS SDK (used by s3-perf)",
    )
    configure_parser.add_argument(
        "--build-jemalloc",
        dest="jemalloc",
        action="store_true",
        help="enable jemalloc library",
    )

    build_parser = sub.add_parser("build", help="build the project (default command)")
    build_parser.add_argument("targets", nargs="*", help="CMake targets to build")

    test_parser = sub.add_parser("test", help="build then run tests with ctest")
    test_parser.add_argument(
        "-R", "--tests-regex", help="run only tests whose name matches the regex"
    )
    test_parser.add_argument(
        "-N",
        "--show-only",
        action="store_true",
        help="list matching tests without running them",
    )
    test_parser.add_argument(
        "--timeout",
        default=180,
        type=int,
        metavar="SECONDS",
        help="per-test timeout in seconds (default: 180, 0=none)",
    )
    test_parser.add_argument(
        "--coverage",
        action="store_true",
        help="instrument with coverage, run tests, and generate an HTML report",
    )

    bench_parser = sub.add_parser("bench", help="build then run benchmarks")
    bench_parser.add_argument(
        "-R", "--tests-regex", help="run only benchmarks whose name matches the regex"
    )
    bench_parser.add_argument(
        "-N",
        "--show-only",
        action="store_true",
        help="list matching benchmarks without running them",
    )
    bench_parser.add_argument(
        "--timeout",
        default=180,
        type=int,
        metavar="SECONDS",
        help="per-binary timeout in seconds (default: 180, 0=none)",
    )

    perf_parser = sub.add_parser(
        "perf",
        help="build release then run a set of perf benchmarks",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=(
            "Run one or more perf benchmarks in a single invocation.\n\n"
            "Targets (positional, repeatable):\n"
            + "".join(f"  {name:<13} {text}\n" for name, text in PERF_TARGETS.items())
            + "  all           run every target above\n\n"
            "Examples:\n"
            "  ./bb -b release perf --duration 60s --warmup 10s file net net-asio\n"
            "  ./bb -b release perf all --duration 60s --warmup 10s\n"
        ),
    )
    perf_parser.add_argument(
        "--timeout",
        default=180,
        type=int,
        metavar="SECONDS",
        help="per-run timeout in seconds (default: 180, 0=none)",
    )
    perf_parser.add_argument(
        "--duration",
        default="10s",
        metavar="DURATION",
        help="measurement duration applied to every benchmark (default: 10s)",
    )
    perf_parser.add_argument(
        "--warmup",
        default="2s",
        metavar="DURATION",
        help="warmup applied to every benchmark (default: 2s)",
    )
    perf_parser.add_argument(
        "--fixed-buffers",
        action="store_true",
        help="run file-perf with registered buffers (IORING_OP_READ_FIXED / WRITE_FIXED)",
    )
    perf_parser.add_argument(
        "targets",
        nargs="+",
        metavar="TARGET",
        choices=[*PERF_TARGETS, "all"],
        help="benchmarks to run (see list above; use 'all' for every target)",
    )

    file_perf_parser = sub.add_parser("file-perf", help="build then run file-perf")
    _add_params_args(file_perf_parser, FilePerfParams, FILE_PERF_HELP)

    fio_perf_parser = sub.add_parser(
        "fio-perf", help="run fio comparison (no build needed)"
    )
    _add_params_args(fio_perf_parser, FioPerfParams, FILE_PERF_HELP)

    net_perf_parser = sub.add_parser("net-perf", help="build then run net-perf")
    _add_params_args(net_perf_parser, NetPerfParams, NET_PERF_HELP)
    net_perf_asio_parser = sub.add_parser(
        "net-perf-asio",
        help="build then run net-perf-asio (Boost.Asio C++20 coroutines)",
    )
    _add_params_args(net_perf_asio_parser, NetPerfParams, NET_PERF_HELP)
    net_perf_epoll_parser = sub.add_parser(
        "net-perf-epoll", help="build then run net-perf-epoll (raw epoll)"
    )
    _add_params_args(net_perf_epoll_parser, NetPerfParams, NET_PERF_HELP)

    simulator_parser = sub.add_parser(
        "simulator",
        help="build then run fibers-simulator (fiber workload pipelines)",
    )
    simulator_parser.add_argument(
        "config",
        metavar="CONFIG",
        help="config file path or a bundled config name (e.g. chain, net-baseline)",
    )
    _add_params_args(simulator_parser, SimulatorParams, SIMULATOR_HELP)

    http_perf_parser = sub.add_parser("http-perf", help="build then run http-perf")
    _add_params_args(http_perf_parser, HttpPerfParams, HTTP_PERF_HELP)

    s3_perf_parser = sub.add_parser("s3-perf", help="build then run s3-perf")
    _add_params_args(s3_perf_parser, S3PerfParams, S3_PERF_HELP)

    return parser


def _run_command(args: argparse.Namespace, extra: list[str], preset: str) -> None:
    if args.command == "clean":
        _check_no_extra(extra)
        cmd_clean()
    elif args.command == "fmt":
        _check_no_extra(extra)
        cmd_fmt(args.check)
    elif args.command == "lint":
        _check_no_extra(extra)
        cmd_lint()
    elif args.command == "configure":
        _check_no_extra(extra)
        cmd_configure(preset, BuildParams(args.poco, args.aws, args.jemalloc))
    elif args.command == "build":
        _check_no_extra(extra)
        cmd_build(preset, args.targets)
    elif args.command == "test":
        test_preset = "debug-coverage" if args.coverage else preset
        cmd_build(test_preset)
        cmd_test(
            test_preset,
            args.tests_regex,
            args.show_only,
            args.timeout,
            args.coverage,
            extra,
        )
    elif args.command == "bench":
        cmd_build(preset)
        cmd_bench(preset, args.tests_regex, args.show_only, args.timeout, extra)
    elif args.command == "perf":
        _check_no_extra(extra)
        cmd_perf(
            preset,
            args.targets,
            args.duration,
            args.warmup,
            args.timeout,
            args.fixed_buffers,
        )
    elif args.command == "file-perf":
        _check_no_extra(extra)
        cmd_build(preset, ["file-perf"])
        cmd_file_perf(preset, _params(args, FilePerfParams))
    elif args.command == "fio-perf":
        _check_no_extra(extra)
        cmd_fio_perf(_params(args, FioPerfParams))
    elif args.command in ("net-perf", "net-perf-asio", "net-perf-epoll"):
        _check_no_extra(extra)
        engine = NetPerfEngine(args.command)
        cmd_build(preset, [engine.value])
        cmd_net_perf(preset, engine, _params(args, NetPerfParams))
    elif args.command == "simulator":
        _check_no_extra(extra)
        cmd_build(preset, ["fibers-simulator"])
        cmd_simulator(preset, args.config, _params(args, SimulatorParams))
    elif args.command == "http-perf":
        _check_no_extra(extra)
        cmd_build(preset, ["http-perf"])
        cmd_http_perf(preset, _params(args, HttpPerfParams))
    elif args.command == "s3-perf":
        _check_no_extra(extra)
        cmd_build(preset, ["s3-perf"])
        cmd_s3_perf(preset, _params(args, S3PerfParams))


def main() -> None:
    _configure_logging()
    args, extra = _build_parser().parse_known_args()

    if args.verbose:
        log.setLevel(logging.DEBUG)

    preset = args.build
    if args.sanitizer:
        preset = f"{preset}-{SANITIZERS[args.sanitizer]}"

    if args.command is None:
        args.command = "build"
        args.targets = []

    log.info("command=%s preset=%s", args.command, preset)

    _, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (hard, hard))
    # A SIGTERM unwinds like Ctrl-C, so every finally that stops a server or profiler runs before bb exits.
    signal.signal(signal.SIGTERM, signal.default_int_handler)

    try:
        _run_command(args, extra, preset)
    except subprocess.CalledProcessError as failure:
        log.error("command failed: r=%d %s", failure.returncode, failure.cmd)
        sys.exit(1)
    except KeyboardInterrupt:
        log.error("interrupted")
        sys.exit(130)
