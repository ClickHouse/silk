#!/usr/bin/env python3

import argparse
import glob
import json
import logging
import os
import platform
import re
import resource
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from collections.abc import Sequence
from dataclasses import dataclass, field, replace
from enum import Enum
from typing import Any, TypedDict
from urllib.parse import urlparse

ROOT = os.path.dirname(os.path.abspath(__file__))
SCRIPTS_DIR = os.path.join(ROOT, "contrib", "FlameGraph")
TOOLS_DIR = os.path.join(ROOT, ".tools")
TMP_DIR = os.path.join(ROOT, "build/tmp")

logging.addLevelName(logging.DEBUG, "DEBUG")
logging.addLevelName(logging.INFO, "INFO ")
logging.addLevelName(logging.WARNING, "WARN ")
logging.addLevelName(logging.ERROR, "ERROR")
logging.basicConfig(
    format="%(asctime)s.%(msecs)03d [%(levelname)s] %(filename)s:%(lineno)d: %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
    level=logging.INFO,
)
log = logging.getLogger("bb")


def run(*args: str, **kwargs: Any) -> None:
    log.debug("run command: %s", " ".join(args))
    subprocess.run(args, cwd=ROOT, check=True, **kwargs)


def run_capture(*args: str, **kwargs: Any) -> subprocess.CompletedProcess[str]:
    """Run a command, capture its stdout/stderr, and return the result."""
    log.debug("run command: %s", " ".join(args))
    result = subprocess.run(
        args, cwd=ROOT, capture_output=True, text=True, check=False, **kwargs
    )
    if result.returncode:
        log.error("command failed: %s\n\n%s", " ".join(args), result.stderr)
    result.check_returncode()
    return result


def start_process(*args: str, **kwargs: Any) -> subprocess.Popen[str]:
    proc = subprocess.Popen(args, cwd=ROOT, text=True, **kwargs)
    log.debug("run command: %s", " ".join(args))
    deadline = time.monotonic() + 0.1
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(
                f"process exited prematurely (code {proc.returncode}): {' '.join(args)}"
            )
        time.sleep(0.01)
    return proc


def wait_for_tcp_port(host: str, port: int, timeout: float = 5.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.05)
    raise TimeoutError(f"{host}:{port} not ready within {timeout}s")


def cmd_clean() -> None:
    build_dir = os.path.join(ROOT, "build")
    if os.path.exists(build_dir):
        shutil.rmtree(build_dir)
        log.info("removed %s", build_dir)
    else:
        log.info("nothing to clean")


_CLANG_FORMAT_REQUIRED_MAJOR = 21


def _find_clang_format() -> str:
    """Locate a clang-format binary matching the required major version."""
    for cmd in (f"clang-format-{_CLANG_FORMAT_REQUIRED_MAJOR}", "clang-format"):
        path = shutil.which(cmd)
        if not path:
            continue
        try:
            result = subprocess.run(
                [path, "--version"], capture_output=True, text=True, check=True
            )
        except subprocess.CalledProcessError:
            continue
        m = re.search(r"clang-format version (\d+)", result.stdout)
        if m and int(m.group(1)) == _CLANG_FORMAT_REQUIRED_MAJOR:
            return path
    raise RuntimeError(
        f"clang-format {_CLANG_FORMAT_REQUIRED_MAJOR} not found; "
        f"install clang-format-{_CLANG_FORMAT_REQUIRED_MAJOR}"
    )


def cmd_fmt(check: bool = False) -> None:
    sources = glob.glob(
        os.path.join(ROOT, "src/**/*.[ch]"), recursive=True
    ) + glob.glob(os.path.join(ROOT, "src/**/*.cpp"), recursive=True)
    args = [_find_clang_format()]
    args += ["--dry-run", "--Werror"] if check else ["-i"]
    run(*args, *sources)


@dataclass
class BuildParams:
    poco: bool = False
    aws: bool = False
    jemalloc: bool = False


def cmd_configure(preset: str, params: BuildParams) -> None:
    build_dir = os.path.join(ROOT, f"build/{preset}")
    if os.path.exists(build_dir):
        shutil.rmtree(build_dir)
    cmake_args = ["cmake", "--preset", preset]
    if params.poco:
        cmake_args += ["-DBUILD_POCO=ON"]
    if params.aws:
        cmake_args += ["-DBUILD_AWS=ON"]
    if params.jemalloc:
        cmake_args += ["-DBUILD_JEMALLOC=ON"]
    run(*cmake_args)


def cmd_build(preset: str, targets: list[str] = []) -> None:
    if not os.path.isdir(os.path.join(ROOT, f"build/{preset}")):
        cmd_configure(preset, BuildParams())
    args = ["cmake", "--build", "--preset", preset]
    for target in targets:
        args += ["--target", target]
    run(*args)


def cmd_test(
    preset: str,
    tests_regex: str | None = None,
    show_only: bool = False,
    timeout: int = 0,
    coverage: bool = False,
    extra: list[str] | None = None,
) -> None:
    profiles_dir: str | None = None
    env: dict[str, str] | None = None
    if coverage:
        profiles_dir = os.path.join(ROOT, f"build/{preset}/profiles")
        if os.path.exists(profiles_dir):
            shutil.rmtree(profiles_dir)
        os.makedirs(profiles_dir)
        env = {
            **os.environ,
            "LLVM_PROFILE_FILE": os.path.join(profiles_dir, "%p.profraw"),
        }

    args = ["ctest", "--preset", preset, "--parallel", str(os.cpu_count())]
    if tests_regex:
        args += ["--tests-regex", tests_regex]
    if show_only:
        args += ["--show-only"]
    if timeout:
        args += ["--timeout", str(timeout)]
    if extra:
        args += extra
    run(*args, env=env)

    if coverage:
        assert profiles_dir is not None
        _gen_coverage_report(preset, profiles_dir)


class _FileInfo(TypedDict):
    lines: dict[int, int]
    branches: dict[int, list[int]]


def _lcov_to_cobertura(lcov_path: str, xml_path: str) -> None:
    files: dict[str, _FileInfo] = {}
    current: str | None = None

    with open(lcov_path) as f:
        for raw in f:
            line = raw.rstrip()
            if line.startswith("SF:"):
                current = line[3:]
                files[current] = {"lines": {}, "branches": {}}
            elif line.startswith("DA:") and current:
                parts = line[3:].split(",")
                files[current]["lines"][int(parts[0])] = int(parts[1])
            elif line.startswith("BRDA:") and current:
                parts = line[5:].split(",")
                lineno, taken = int(parts[0]), parts[3]
                count = 0 if taken == "-" else int(taken)
                files[current]["branches"].setdefault(lineno, []).append(count)
            elif line == "end_of_record":
                current = None

    total_lines_valid = total_lines_hit = total_branches_valid = total_branches_hit = 0

    root = ET.Element("coverage")
    ET.SubElement(ET.SubElement(root, "sources"), "source").text = ROOT
    pkgs_el = ET.SubElement(root, "packages")

    by_pkg: dict[str, list[str]] = {}
    for fname in sorted(files):
        by_pkg.setdefault(os.path.dirname(fname) or ".", []).append(fname)

    for pkg_name, fnames in sorted(by_pkg.items()):
        pkg_el = ET.SubElement(
            pkgs_el, "package", name=pkg_name.replace("/", "."), complexity="0"
        )
        classes_el = ET.SubElement(pkg_el, "classes")
        pkg_lines_valid = pkg_lines_hit = pkg_branches_valid = pkg_branches_hit = 0

        for fname in sorted(fnames):
            data = files[fname]
            class_el = ET.SubElement(
                classes_el,
                "class",
                name=os.path.basename(fname),
                filename=fname,
                complexity="0",
            )
            ET.SubElement(class_el, "methods")
            lines_el = ET.SubElement(class_el, "lines")
            file_lines_valid = file_lines_hit = 0
            file_branches_valid = file_branches_hit = 0

            for lineno in sorted(data["lines"]):
                hits = data["lines"][lineno]
                attrs: dict[str, str] = {"number": str(lineno), "hits": str(hits)}
                branches = data["branches"].get(lineno, [])
                if branches:
                    branches_hit = sum(1 for b in branches if b > 0)
                    branches_total = len(branches)
                    pct = (
                        round(100 * branches_hit / branches_total)
                        if branches_total
                        else 0
                    )
                    attrs["branch"] = "true"
                    attrs["condition-coverage"] = (
                        f"{pct}% ({branches_hit}/{branches_total})"
                    )
                    file_branches_valid += branches_total
                    file_branches_hit += branches_hit
                else:
                    attrs["branch"] = "false"
                ET.SubElement(lines_el, "line", attrs)
                file_lines_valid += 1
                if hits > 0:
                    file_lines_hit += 1

            line_rate = file_lines_hit / file_lines_valid if file_lines_valid else 0.0
            branch_rate = (
                file_branches_hit / file_branches_valid if file_branches_valid else 0.0
            )
            class_el.set("line-rate", f"{line_rate:.4f}")
            class_el.set("branch-rate", f"{branch_rate:.4f}")
            pkg_lines_valid += file_lines_valid
            pkg_lines_hit += file_lines_hit
            pkg_branches_valid += file_branches_valid
            pkg_branches_hit += file_branches_hit

        pkg_line_rate = pkg_lines_hit / pkg_lines_valid if pkg_lines_valid else 0.0
        pkg_branch_rate = (
            pkg_branches_hit / pkg_branches_valid if pkg_branches_valid else 0.0
        )
        pkg_el.set("line-rate", f"{pkg_line_rate:.4f}")
        pkg_el.set("branch-rate", f"{pkg_branch_rate:.4f}")
        total_lines_valid += pkg_lines_valid
        total_lines_hit += pkg_lines_hit
        total_branches_valid += pkg_branches_valid
        total_branches_hit += pkg_branches_hit

    root.set("version", "5.0")
    root.set("timestamp", str(int(time.time())))
    root.set("lines-valid", str(total_lines_valid))
    root.set("lines-covered", str(total_lines_hit))
    root.set(
        "line-rate",
        f"{total_lines_hit / total_lines_valid:.4f}" if total_lines_valid else "0.0000",
    )
    root.set("branches-valid", str(total_branches_valid))
    root.set("branches-covered", str(total_branches_hit))
    root.set(
        "branch-rate",
        (
            f"{total_branches_hit / total_branches_valid:.4f}"
            if total_branches_valid
            else "0.0000"
        ),
    )
    root.set("complexity", "0")

    tree = ET.ElementTree(root)
    ET.indent(tree, space="  ")
    with open(xml_path, "w") as f:
        f.write('<?xml version="1.0" ?>\n')
        tree.write(f, encoding="unicode", xml_declaration=False)


def _gen_coverage_report(preset: str, profiles_dir: str) -> None:
    build_dir = os.path.join(ROOT, f"build/{preset}")
    profdata_path = os.path.join(build_dir, "coverage.profdata")
    report_dir = os.path.join(build_dir, "html")
    bin_dir = os.path.join(build_dir, "bin")

    profraw_files = glob.glob(os.path.join(profiles_dir, "*.profraw"))
    if not profraw_files:
        log.error("no .profraw files found in %s", profiles_dir)
        sys.exit(1)

    run("llvm-profdata-21", "merge", "-sparse", *profraw_files, "-o", profdata_path)

    test_bins = sorted(glob.glob(os.path.join(bin_dir, "*-test")))
    if not test_bins:
        log.error("no *-test binaries found in %s", bin_dir)
        sys.exit(1)

    common_cov_args = [test_bins[0]]
    for binary in test_bins[1:]:
        common_cov_args += ["-object", binary]
    common_cov_args += [
        f"-instr-profile={profdata_path}",
        "-ignore-filename-regex=/usr/|/_deps/|/libbacktrace-generated/|contrib/|-test\\.cpp",
        "-Xdemangler=c++filt",
    ]

    run(
        "llvm-cov-21",
        "show",
        *common_cov_args,
        "-format=html",
        f"-output-dir={report_dir}",
        "-show-line-counts-or-regions",
    )

    lcov_path = os.path.join(build_dir, "coverage.lcov")
    with open(lcov_path, "w") as lcov_file:
        run(
            "llvm-cov-21",
            "export",
            *common_cov_args,
            "--format=lcov",
            stdout=lcov_file,
        )

    xml_path = os.path.join(build_dir, "coverage.xml")
    _lcov_to_cobertura(lcov_path, xml_path)

    log.info("coverage report: %s/index.html", report_dir)
    log.info("coverage lcov:   %s", lcov_path)
    log.info("coverage xml:    %s", xml_path)


def cmd_bench(
    preset: str,
    tests_regex: str | None = None,
    show_only: bool = False,
    timeout: int = 0,
    extra: list[str] | None = None,
) -> None:
    bin_dir = os.path.join(ROOT, f"build/{preset}/bin")
    benches = sorted(glob.glob(os.path.join(bin_dir, "*-bench")))
    if not benches:
        log.error("no *-bench binaries found in %s", bin_dir)
        sys.exit(1)

    def list_tests(bench: str) -> list[str]:
        return run_capture(bench, "--benchmark_list_tests").stdout.splitlines()

    benches = [b for b in benches if list_tests(b)]

    if tests_regex:
        benches = [b for b in benches if any(tests_regex in t for t in list_tests(b))]
        if not benches:
            log.error("no benchmarks matched %r", tests_regex)
            sys.exit(1)

    for bench in benches:
        args = [bench]
        if show_only:
            args += ["--benchmark_list_tests"]
        if tests_regex:
            args += [f"--benchmark_filter={tests_regex}"]
        if extra:
            args += extra
        run(*args, timeout=timeout or None)


def _render_flamegraph(folded_file: str, out_svg: str, title: str) -> None:
    combined_lines: list[str] = []

    with open(folded_file) as f:
        for line in f:
            parts = line.strip().split(" ")
            if len(parts) != 3:
                continue
            stack, on_ns, off_ns = parts
            total = int(on_ns) + int(off_ns)
            if total > 0:
                combined_lines.append(f"{stack} {total}\n")

    flamegraph_pl = os.path.join(SCRIPTS_DIR, "flamegraph.pl")
    with open(out_svg, "w") as outfile:
        subprocess.run(
            [flamegraph_pl, "--title", title, "--countname=ns", "--hash"],
            input="".join(combined_lines).encode(),
            stdout=outfile,
            cwd=ROOT,
            check=True,
        )


def _run_flamegraph(preset: str, name: str, client_args: list[str]) -> None:
    cmd_build(preset, ["profiler"])

    profiler_bin = os.path.join(ROOT, f"build/{preset}/bin/profiler")
    folded_stacks = os.path.join(ROOT, f"build/{preset}/{name}.flamegraph.folded")
    out_svg = os.path.join(ROOT, f"build/{preset}/{name}.flamegraph.svg")
    verbose_flag = ["--verbose"] if log.isEnabledFor(logging.DEBUG) else []

    log.info("profiling %s -> %s", name, out_svg)

    client = start_process(*client_args, stdout=subprocess.DEVNULL)

    try:
        with open(folded_stacks, "w") as f:
            profiler = start_process(
                profiler_bin,
                "--pid",
                str(client.pid),
                "--on-cpu",
                "--off-cpu",
                "--kernel-stacks",
                *verbose_flag,
                stdout=f,
            )
        try:
            client.wait()
            if client.returncode != 0:
                raise RuntimeError(f"client exited with code {client.returncode}")
        finally:
            profiler.send_signal(signal.SIGTERM)
            profiler.wait()
            if profiler.returncode != 0:
                raise RuntimeError(f"profiler exited with code {profiler.returncode}")
    except Exception:
        client.kill()
        raise

    _render_flamegraph(folded_stacks, out_svg, f"{name} on-CPU + off-CPU")

    log.info("folded stacks: %s", folded_stacks)
    log.info("flamegraph: %s", out_svg)


def _fmt_ns(ns: int) -> str:
    if ns >= 1_000_000:
        return f"{ns / 1_000_000:.1f} ms"
    if ns >= 1_000:
        return f"{ns / 1_000:.1f} us"
    return f"{ns} ns"


def _print_counters(data: dict[str, Any]) -> None:
    sched = data.get("scheduler_latency", {})
    if sched:
        print()
        for kind_name, cats in sched.items():
            print(f"  {kind_name}")
            for cat_name, r in cats.items():
                print(
                    f"    {cat_name}  count: {r['count']:>10,}"
                    f"  p50: {_fmt_ns(r['p50_ns']):>10}"
                    f"  p90: {_fmt_ns(r['p90_ns']):>10}"
                    f"  p99: {_fmt_ns(r['p99_ns']):>10}"
                    f"  p999: {_fmt_ns(r['p999_ns']):>10}"
                )

    counters = data.get("counters", {})
    if not counters:
        return
    width = max(len(k) for k in counters)
    print()
    for name, value in counters.items():
        if name.endswith("Time"):
            print(f"  {name:<{width}}  {value // 1_000_000:>15,} ms")
        else:
            print(f"  {name:<{width}}  {value:>15,}")
    print()


def _parse_duration_s(s: str) -> float:
    """Parse a duration string (e.g. '10ms', '100us', '1s') to seconds. No suffix = seconds."""
    for suffix, factor in [
        ("ns", 1e-9),
        ("us", 1e-6),
        ("ms", 1e-3),
        ("m", 60.0),
        ("s", 1.0),
    ]:
        if s.endswith(suffix):
            return float(s[: -len(suffix)]) * factor
    return float(s)


def _parse_perf(data: dict[str, Any]) -> dict[str, Any]:
    lat = data["latency_us"]
    result: dict[str, Any] = {
        "rps": f"{round(data['rps'] / 1000)}k",
        "avg": lat["avg"],
        "p50": lat["p50"],
        "p95": lat["p95"],
        "p99": lat["p99"],
        "p999": lat["p99_9"],
    }
    if "bw_bytes" in data:
        result["bw"] = f"{round(data['bw_bytes'] / (1024 * 1024)):.1f} MiB/s"
    return result


def _us(v: Any) -> str:
    return f"{v} µs" if v != "?" else "?"


def _perf_row(cells: Sequence[object], widths: list[int]) -> str:
    return "".join(f"| {str(c):<{w}} " for c, w in zip(cells, widths)) + "|"


def _perf_sep(widths: list[int]) -> str:
    return "|" + "|".join("-" * (w + 2) for w in widths) + "|"


def _cpu_split() -> tuple[str, str]:
    ncpus = os.cpu_count() or 2
    half = max(1, ncpus // 2)
    return f"0-{half - 1}", f"{half}-{ncpus - 1}"


class NetPerfEngine(Enum):
    FIBERS = "net-perf"  # silk fibers + io_uring
    ASIO = "net-perf-asio"  # boost.asio C++20 coroutines
    EPOLL = "net-perf-epoll"  # raw epoll


@dataclass
class NetPerfParams:
    engine: NetPerfEngine = NetPerfEngine.FIBERS
    host: str = "127.0.0.1"
    port: int = 17777
    msg_size: int = 64
    duration: str = "10s"
    warmup: str = "2s"
    connections: list[int] = field(default_factory=lambda: [1000])
    delay: str = "0"
    stall_rate: float = 0.0
    stall_duration: str = "0"
    flamegraph: bool = False
    print_counters: bool = False
    timeout: int = 180


_NP_HEADERS: list[str] = [
    "connections",
    "RPS",
    "BW",
    "avg",
    "p50",
    "p95",
    "p99",
    "p99.9",
]
_NP_WIDTH: list[int] = [11, 8, 10, 8, 8, 8, 8, 8]


def cmd_net_perf(preset: str, params: NetPerfParams) -> None:
    binary = params.engine.value
    print()
    print(f"## {binary} -- async network I/O")
    print()
    print(
        f"{params.host}:{params.port}, msg_size={params.msg_size}, duration={params.duration}, warmup={params.warmup}, delay={params.delay}"
    )
    print()

    net_perf = os.path.join(ROOT, f"build/{preset}/bin/{binary}")
    server_cpus, client_cpus = _cpu_split()
    local = params.host in ("127.0.0.1", "localhost")
    verbose_flag = ["--verbose"] if log.isEnabledFor(logging.DEBUG) else []
    print_counters_flag = ["--print-counters"] if params.print_counters else []
    stall_flags: list[str] = []
    if params.stall_rate > 0:
        stall_flags = [
            "--stall-rate",
            str(params.stall_rate),
            "--stall-duration",
            str(params.stall_duration),
        ]

    server = None
    if local:
        server_kwargs: dict[str, Any] = {}
        if params.print_counters:
            server_kwargs["stdout"] = subprocess.PIPE
        server = start_process(
            "taskset",
            "-c",
            server_cpus,
            net_perf,
            "server",
            "--host",
            params.host,
            "--port",
            str(params.port),
            "--delay",
            str(params.delay),
            *print_counters_flag,
            *verbose_flag,
            **server_kwargs,
        )
        wait_for_tcp_port(params.host, params.port)

    try:
        if params.flamegraph:
            _run_flamegraph(
                preset,
                binary,
                [
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
                    str(params.connections[0]),
                    "--msg-size",
                    str(params.msg_size),
                    "--duration",
                    str(params.duration),
                    "--warmup",
                    str(params.warmup),
                    *stall_flags,
                    *verbose_flag,
                ],
            )
        else:
            print(_perf_row(_NP_HEADERS, _NP_WIDTH))
            print(_perf_sep(_NP_WIDTH))

            for conns in params.connections:
                result = run_capture(
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
                    str(conns),
                    "--msg-size",
                    str(params.msg_size),
                    "--duration",
                    str(params.duration),
                    "--warmup",
                    str(params.warmup),
                    *stall_flags,
                    *print_counters_flag,
                    *verbose_flag,
                    timeout=params.timeout or None,
                )
                data = json.loads(result.stdout)
                p = _parse_perf(data)

                cells: list[str | int] = [
                    conns,
                    p.get("rps", "?"),
                    p.get("bw", "?"),
                    _us(p.get("avg", "?")),
                    _us(p.get("p50", "?")),
                    _us(p.get("p95", "?")),
                    _us(p.get("p99", "?")),
                    _us(p.get("p999", "?")),
                ]
                print(_perf_row(cells, _NP_WIDTH))
                if params.print_counters:
                    print()
                    print("### client counters")
                    _print_counters(data)
    finally:
        if server:
            server.terminate()
            server_stdout, _ = server.communicate()
            if params.print_counters and server_stdout:
                try:
                    server_data = json.loads(server_stdout)
                    print("### server counters")
                    _print_counters(server_data)
                except json.JSONDecodeError as e:
                    log.warning("could not parse server counters: %s", e)


@dataclass
class FilePerfParams:
    file: str = "/dev/shm/file-perf.bin"
    bs: str = "4k"
    size: str = "1g"
    duration: str = "10s"
    warmup: str = "2s"
    numjobs: list[int] = field(default_factory=lambda: [1])
    iodepth: list[int] = field(default_factory=lambda: [16])
    rw: list[str] = field(default_factory=lambda: ["randread"])
    flamegraph: bool = False
    print_counters: bool = False
    timeout: int = 180


_FP_HEADERS: list[str] = [
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
_FP_WIDTHS: list[int] = [8, 8, 10, 8, 10, 8, 8, 8, 8, 8]


def cmd_file_perf(preset: str, params: FilePerfParams) -> None:
    configs = [
        (j, d, m) for m in params.rw for j in params.numjobs for d in params.iodepth
    ]
    if not configs:
        return

    print()
    print("## file-perf -- async file I/O")
    print()
    print(
        f"file={params.file}, bs={params.bs}, size={params.size}, duration={params.duration}, warmup={params.warmup}"
    )
    print()

    file_perf = os.path.join(ROOT, f"build/{preset}/bin/file-perf")
    verbose_flag = ["--verbose"] if log.isEnabledFor(logging.DEBUG) else []
    print_counters_flag = ["--print-counters"] if params.print_counters else []

    try:
        if params.flamegraph:
            jobs, depth, mode = configs[0]
            _run_flamegraph(
                preset,
                "file-perf",
                [
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
                    str(params.duration),
                    "--warmup",
                    str(params.warmup),
                    "--filename",
                    params.file,
                    *verbose_flag,
                ],
            )
        else:
            print(_perf_row(_FP_HEADERS, _FP_WIDTHS))
            print(_perf_sep(_FP_WIDTHS))

            for jobs, depth, mode in configs:
                result = run_capture(
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
                    str(params.duration),
                    "--warmup",
                    str(params.warmup),
                    "--filename",
                    params.file,
                    *print_counters_flag,
                    *verbose_flag,
                    timeout=params.timeout or None,
                )
                data = json.loads(result.stdout)
                p = _parse_perf(data)

                cells: list[str | int] = [
                    jobs,
                    depth,
                    mode,
                    p.get("rps", "?"),
                    p.get("bw", "?"),
                    _us(p.get("avg", "?")),
                    _us(p.get("p50", "?")),
                    _us(p.get("p95", "?")),
                    _us(p.get("p99", "?")),
                    _us(p.get("p999", "?")),
                ]
                print(_perf_row(cells, _FP_WIDTHS))
                if params.print_counters:
                    _print_counters(data)
    finally:
        if os.path.exists(params.file):
            os.unlink(params.file)


def _parse_fio(data: dict[str, Any], mode: str) -> dict[str, Any]:
    field = "write" if "write" in mode else "read"
    job = data["jobs"][0][field]
    ns = job["clat_ns"]
    iops_k = round(job["iops"] / 1000)
    bw_mib = round(job["bw_bytes"] / (1024 * 1024))
    avg_us = round(ns["mean"] / 1000, 2)
    pcts: dict[str, float] = ns.get("percentile", {})

    def pct(p: str) -> float:
        return round(pcts.get(p, 0) / 1000, 2)

    return {
        "iops": f"{iops_k}k",
        "bw": f"{bw_mib} MiB/s",
        "avg": avg_us,
        "p50": pct("50.000000"),
        "p95": pct("95.000000"),
        "p99": pct("99.000000"),
        "p999": pct("99.900000"),
    }


def cmd_fio_perf(params: FilePerfParams) -> None:
    configs = [
        (j, d, m) for m in params.rw for j in params.numjobs for d in params.iodepth
    ]
    if not configs:
        return

    print()
    print("## fio comparison (io_uring)")
    print()
    print(
        f"file={params.file}, bs={params.bs}, size={params.size}, duration={params.duration}, warmup={params.warmup}"
    )
    print()

    try:
        print(_perf_row(_FP_HEADERS, _FP_WIDTHS))
        print(_perf_sep(_FP_WIDTHS))

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
                f"--runtime={int(_parse_duration_s(params.duration))}",
                f"--ramp_time={int(_parse_duration_s(params.warmup))}",
                "--time_based",
                "--fallocate=native",
                f"--filename={params.file}",
                "--group_reporting",
                "--output-format=json",
                timeout=params.timeout or None,
            )
            p = _parse_fio(json.loads(result.stdout), mode)

            cells: list[str | int] = [
                jobs,
                depth,
                mode,
                p.get("iops", "?"),
                p.get("bw", "?"),
                _us(p.get("avg", "?")),
                _us(p.get("p50", "?")),
                _us(p.get("p95", "?")),
                _us(p.get("p99", "?")),
                _us(p.get("p999", "?")),
            ]
            print(_perf_row(cells, _FP_WIDTHS))
    finally:
        if os.path.exists(params.file):
            os.unlink(params.file)


@dataclass
class HttpPerfParams:
    host: str = "127.0.0.1"
    port: int = 18080
    duration: str = "10s"
    warmup: str = "2s"
    connections: list[int] = field(default_factory=lambda: [1000])
    delay: str = "0"
    threads: bool = False
    flamegraph: bool = False
    print_counters: bool = False
    timeout: int = 180
    nginx: bool = False


_HP_HEADERS: list[str] = [
    "connections",
    "mode",
    "RPS",
    "avg",
    "p50",
    "p95",
    "p99",
    "p99.9",
]
_HP_WIDTHS: list[int] = [11, 8, 8, 8, 8, 8, 8, 8]


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
    params: HttpPerfParams, server_cpus: str, workers: int
) -> subprocess.Popen[str]:
    delay_s = _parse_duration_s(params.delay)
    if delay_s > 0:
        load_modules = "load_module modules/ndk_http_module.so;\nload_module modules/ngx_http_lua_module.so;\n"
        handler = (
            f"content_by_lua_block {{ ngx.sleep({delay_s}); ngx.exit(ngx.HTTP_OK); }}"
        )
    else:
        load_modules = ""
        handler = "return 200;"

    os.makedirs(TMP_DIR, exist_ok=True)
    conf_path = os.path.join(TMP_DIR, "http-perf-nginx.conf")
    with open(conf_path, "w") as f:
        f.write(
            _NGINX_CONF.format(
                port=params.port,
                workers=workers,
                handler=handler,
                load_modules=load_modules,
                pid_file=os.path.join(TMP_DIR, "http-perf-nginx.pid"),
            )
        )

    return start_process(
        "taskset",
        "-c",
        server_cpus,
        "nginx",
        "-c",
        conf_path,
        "-g",
        "daemon off;",
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
    if _parse_duration_s(params.delay) > 0:
        args += ["--delay", params.delay]
    if params.print_counters:
        args += ["--print-counters"]
    if log.isEnabledFor(logging.DEBUG):
        args += ["--verbose"]
    server_kwargs: dict[str, Any] = {}
    if params.print_counters:
        server_kwargs["stdout"] = subprocess.PIPE
    return start_process(*args, **server_kwargs)


def cmd_http_perf(preset: str, params: HttpPerfParams) -> None:
    mode = "threads" if params.threads else "fibers"
    server_kind = "nginx" if params.nginx else "internal"
    print()
    print(f"## http-perf (server={server_kind}, client={mode}) -- HTTP/1.1 GET")
    print()
    print(f"duration={params.duration}, warmup={params.warmup}, delay={params.delay}")
    print()

    server_cpus, client_cpus = _cpu_split()
    workers = (os.cpu_count() or 2) // 2

    if params.nginx:
        server = _start_nginx_server(params, server_cpus, workers)
    else:
        server = _start_internal_server(preset, params, server_cpus)

    wait_for_tcp_port(params.host, params.port)

    http_perf = os.path.join(ROOT, f"build/{preset}/bin/http-perf")
    threads_flag = ["--threads"] if params.threads else []
    verbose_flag = ["--verbose"] if log.isEnabledFor(logging.DEBUG) else []
    print_counters_flag = ["--print-counters"] if params.print_counters else []

    try:
        if params.flamegraph:
            _run_flamegraph(
                preset,
                "http-perf-" + mode,
                [
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
                    str(params.connections[0]),
                    "--duration",
                    str(params.duration),
                    "--warmup",
                    str(params.warmup),
                    *threads_flag,
                    *verbose_flag,
                ],
            )
        else:
            print(_perf_row(_HP_HEADERS, _HP_WIDTHS))
            print(_perf_sep(_HP_WIDTHS))

            for conns in params.connections:
                result = run_capture(
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
                    str(conns),
                    "--duration",
                    str(params.duration),
                    "--warmup",
                    str(params.warmup),
                    *threads_flag,
                    *print_counters_flag,
                    *verbose_flag,
                    timeout=params.timeout or None,
                )
                data = json.loads(result.stdout)
                p = _parse_perf(data)

                cells: list[str | int] = [
                    conns,
                    mode,
                    p.get("rps", "?"),
                    _us(p.get("avg", "?")),
                    _us(p.get("p50", "?")),
                    _us(p.get("p95", "?")),
                    _us(p.get("p99", "?")),
                    _us(p.get("p999", "?")),
                ]
                print(_perf_row(cells, _HP_WIDTHS))
                if params.print_counters:
                    print()
                    print("### client counters")
                    _print_counters(data)
    finally:
        server.terminate()
        if params.print_counters and not params.nginx:
            server_stdout, _ = server.communicate()
            if server_stdout:
                try:
                    server_data = json.loads(server_stdout)
                    print("### server counters")
                    _print_counters(server_data)
                except json.JSONDecodeError as e:
                    log.warning("could not parse server counters: %s", e)
        else:
            server.wait()


def _ensure_minio() -> tuple[str, str]:
    """Return (minio, mcli) paths, downloading to .tools/ if not in PATH."""
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
        "minio",
        f"https://dl.min.io/server/minio/release/linux-{arch}/minio",
    )
    mcli = ensure(
        "mcli",
        f"https://dl.min.io/client/mc/release/linux-{arch}/mc",
    )
    return minio, mcli


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
    rw: list[str] = field(default_factory=lambda: ["read"])
    threads: bool = False
    flamegraph: bool = False
    data_dir: str = "/dev/shm/minio-data"
    print_counters: bool = False
    timeout: int = 180


_S3P_HEADERS: list[str] = [
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
_S3P_WIDTHS: list[int] = [8, 8, 12, 8, 8, 8, 8, 8, 8, 8]


def _parse_s3_perf(data: dict[str, Any]) -> dict[str, Any]:
    lat = data["latency_us"]
    return {
        "rps": str(round(data["rps"])),
        "avg": lat["avg"],
        "p50": lat["p50"],
        "p95": lat["p95"],
        "p99": lat["p99"],
        "p999": lat["p99_9"],
    }


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

    parsed = urlparse(params.endpoint)
    minio_addr = f"{parsed.hostname}:{parsed.port or 9000}"
    mc_alias = "bb-s3-perf"

    configs = [
        (j, d, m) for m in params.rw for j in params.numjobs for d in params.iodepth
    ]
    if not configs:
        return

    threads_flag = ["--threads"] if params.threads else []
    verbose_flag = ["--verbose"] if log.isEnabledFor(logging.DEBUG) else []
    print_counters_flag = ["--print-counters"] if params.print_counters else []

    def make_cmd(jobs: int, depth: int, mode: str) -> list[str]:
        return [
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
            str(params.duration),
            "--warmup",
            str(params.warmup),
            *threads_flag,
            *print_counters_flag,
            *verbose_flag,
        ]

    minio_bin, mcli_bin = _ensure_minio()
    server_cpus, client_cpus = _cpu_split()

    os.makedirs(params.data_dir, exist_ok=True)
    minio = start_process(
        "taskset",
        "-c",
        server_cpus,
        minio_bin,
        "server",
        params.data_dir,
        "--address",
        minio_addr,
        "--quiet",
    )

    try:
        wait_for_tcp_port(parsed.hostname or "127.0.0.1", parsed.port or 9000)

        run(
            mcli_bin,
            "--quiet",
            "alias",
            "set",
            mc_alias,
            params.endpoint,
            params.access_key,
            params.secret_key,
        )
        run(
            mcli_bin,
            "--quiet",
            "mb",
            "--ignore-existing",
            f"{mc_alias}/{params.bucket}",
        )

        # Seed the test object so read benchmarks have something to fetch.
        if any(m in ("read", "readwrite") for m in params.rw):
            with tempfile.NamedTemporaryFile(delete=False) as f:
                f.write(b"x" * params.size)
                seed_path = f.name
            try:
                run(
                    mcli_bin,
                    "--quiet",
                    "cp",
                    seed_path,
                    f"{mc_alias}/{params.bucket}/{params.key}",
                )
            finally:
                os.unlink(seed_path)

        executor = "threads" if params.threads else "fibers"

        if params.flamegraph:
            jobs, depth, mode = configs[0]
            _run_flamegraph(
                preset,
                f"s3-perf-{mode}-{executor}",
                ["taskset", "-c", client_cpus] + make_cmd(jobs, depth, mode),
            )
        else:
            print(_perf_row(_S3P_HEADERS, _S3P_WIDTHS))
            print(_perf_sep(_S3P_WIDTHS))

            for jobs, depth, mode in configs:
                result = run_capture(
                    "taskset",
                    "-c",
                    client_cpus,
                    *make_cmd(jobs, depth, mode),
                    timeout=params.timeout or None,
                )
                data = json.loads(result.stdout)
                p = _parse_s3_perf(data)

                cells: list[str | int] = [
                    jobs,
                    depth,
                    mode,
                    executor,
                    p.get("rps", "?"),
                    _us(p.get("avg", "?")),
                    _us(p.get("p50", "?")),
                    _us(p.get("p95", "?")),
                    _us(p.get("p99", "?")),
                    _us(p.get("p999", "?")),
                ]
                print(_perf_row(cells, _S3P_WIDTHS))
                if params.print_counters:
                    _print_counters(data)
    finally:
        minio.terminate()
        minio.wait()


SANITIZERS: dict[str, str] = {
    "thread": "tsan",
    "address": "asan",
    "undefined": "ubsan",
    "memory": "msan",
}


def _check_no_extra(extra: list[str]) -> None:
    if extra:
        log.error("unexpected arguments: %s", " ".join(extra))
        sys.exit(1)


def _params_from_args(args: argparse.Namespace, prefix: str, cls: type) -> Any:
    pfx = prefix + "_"
    kwargs = {
        key[len(pfx) :]: val for key, val in vars(args).items() if key.startswith(pfx)
    }
    return cls(**kwargs)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="bb")
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
        help="print every command before running it",
    )

    sub = parser.add_subparsers(dest="command")

    sub.add_parser("clean", help="remove build/ directory")

    fmt_parser = sub.add_parser("fmt", help="format all sources with clang-format")
    fmt_parser.add_argument(
        "--check", action="store_true", help="check formatting without modifying files"
    )

    configure_parser = sub.add_parser("configure", help="run CMake configure step only")
    configure_parser.add_argument(
        "--build-poco",
        action="store_true",
        help="enable Poco library (used by http-perf)",
    )
    configure_parser.add_argument(
        "--build-aws", action="store_true", help="enable AWS SDK (used by s3-perf)"
    )
    configure_parser.add_argument(
        "--build-jemalloc", action="store_true", help="enable jemalloc library"
    )

    build_parser = sub.add_parser("build", help="build the project (default command)")
    build_parser.add_argument("targets", nargs="*", help="CMake targets to build")

    #
    # test
    #

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

    #
    # bench
    #

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

    #
    # perf
    #

    perf_parser = sub.add_parser(
        "perf",
        help="build release then run a set of perf benchmarks",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=(
            "Run one or more perf benchmarks in a single invocation.\n\n"
            "Targets (positional, repeatable):\n"
            "  file          file-perf\n"
            "  fio           fio comparison\n"
            "  net           net-perf\n"
            "  net-asio      net-perf-asio\n"
            "  net-epoll     net-perf-epoll\n"
            "  http          http-perf (internal server, fibers)\n"
            "  http-threads  http-perf (internal server, thread client)\n"
            "  http-nginx    http-perf against nginx (fiber client)\n"
            "  s3            s3-perf (fibers)\n"
            "  s3-threads    s3-perf (threads)\n"
            "  all           run every target above\n\n"
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
        default=None,
        metavar="DURATION",
        help="measurement duration applied to every benchmark (e.g. 60s); per-binary defaults are used when omitted",
    )
    perf_parser.add_argument(
        "--warmup",
        default=None,
        metavar="DURATION",
        help="warmup duration applied to every benchmark (e.g. 10s); per-binary defaults are used when omitted",
    )
    perf_parser.add_argument(
        "targets",
        nargs="+",
        metavar="TARGET",
        choices=[
            "file",
            "fio",
            "net",
            "net-asio",
            "net-epoll",
            "http",
            "http-threads",
            "http-nginx",
            "s3",
            "s3-threads",
            "all",
        ],
        help="benchmarks to run (see list above; use 'all' for every target)",
    )

    #
    # file-perf
    #

    file_params = FilePerfParams()
    file_perf_parser = sub.add_parser("file-perf", help="build then run file-perf")
    file_perf_parser.add_argument(
        "--file", dest="file_file", default=file_params.file, metavar="PATH"
    )
    file_perf_parser.add_argument(
        "--bs", dest="file_bs", default=file_params.bs, metavar="SIZE"
    )
    file_perf_parser.add_argument(
        "--size", dest="file_size", default=file_params.size, metavar="SIZE"
    )
    file_perf_parser.add_argument(
        "--duration",
        dest="file_duration",
        default=file_params.duration,
        metavar="DURATION",
    )
    file_perf_parser.add_argument(
        "--warmup",
        dest="file_warmup",
        default=file_params.warmup,
        metavar="DURATION",
    )
    file_perf_parser.add_argument(
        "--numjobs",
        dest="file_numjobs",
        type=int,
        nargs="+",
        default=file_params.numjobs,
        metavar="N",
    )
    file_perf_parser.add_argument(
        "--iodepth",
        dest="file_iodepth",
        type=int,
        nargs="+",
        default=file_params.iodepth,
        metavar="N",
    )
    file_perf_parser.add_argument(
        "--rw",
        dest="file_rw",
        nargs="+",
        default=file_params.rw,
        choices=["randread", "randwrite", "seqread"],
    )
    file_perf_parser.add_argument(
        "--flamegraph",
        dest="file_flamegraph",
        action="store_true",
        help="profile process and generate flamegraph SVG",
    )
    file_perf_parser.add_argument(
        "--print-counters",
        dest="file_print_counters",
        action="store_true",
        help="print perf counters after each run",
    )
    file_perf_parser.add_argument(
        "--timeout",
        dest="file_timeout",
        default=180,
        type=int,
        metavar="SECONDS",
        help="per-run timeout in seconds (default: 180, 0=none)",
    )

    #
    # fio-perf
    #

    fio_perf_parser = sub.add_parser(
        "fio-perf", help="run fio comparison (no build needed)"
    )
    fio_perf_parser.add_argument(
        "--file", dest="fio_file", default=file_params.file, metavar="PATH"
    )
    fio_perf_parser.add_argument(
        "--bs", dest="fio_bs", default=file_params.bs, metavar="SIZE"
    )
    fio_perf_parser.add_argument(
        "--size", dest="fio_size", default=file_params.size, metavar="SIZE"
    )
    fio_perf_parser.add_argument(
        "--duration",
        dest="fio_duration",
        default=file_params.duration,
        metavar="DURATION",
    )
    fio_perf_parser.add_argument(
        "--warmup",
        dest="fio_warmup",
        default=file_params.warmup,
        metavar="DURATION",
    )
    fio_perf_parser.add_argument(
        "--numjobs",
        dest="fio_numjobs",
        type=int,
        nargs="+",
        default=file_params.numjobs,
        metavar="N",
    )
    fio_perf_parser.add_argument(
        "--iodepth",
        dest="fio_iodepth",
        type=int,
        nargs="+",
        default=file_params.iodepth,
        metavar="N",
    )
    fio_perf_parser.add_argument(
        "--rw",
        dest="fio_rw",
        nargs="+",
        default=file_params.rw,
        choices=["randread", "randwrite", "seqread"],
    )
    fio_perf_parser.add_argument(
        "--timeout",
        dest="fio_timeout",
        default=180,
        type=int,
        metavar="SECONDS",
        help="per-run timeout in seconds (default: 180, 0=none)",
    )

    #
    # net-perf / net-perf-asio
    #

    net_params = NetPerfParams()

    def _add_net_args(parser: argparse.ArgumentParser) -> None:
        parser.add_argument("--host", dest="net_host", default=net_params.host)
        parser.add_argument(
            "--port", dest="net_port", default=net_params.port, type=int
        )
        parser.add_argument(
            "--msg-size",
            dest="net_msg_size",
            default=net_params.msg_size,
            type=int,
            metavar="BYTES",
        )
        parser.add_argument(
            "--duration",
            dest="net_duration",
            default=net_params.duration,
            metavar="DURATION",
        )
        parser.add_argument(
            "--warmup", dest="net_warmup", default=net_params.warmup, metavar="DURATION"
        )
        parser.add_argument(
            "--connections",
            dest="net_connections",
            default=net_params.connections,
            type=int,
            nargs="+",
            metavar="N",
        )
        parser.add_argument(
            "--delay",
            dest="net_delay",
            default=net_params.delay,
            metavar="DURATION",
            help="server-side delay per message (e.g. 1ms, 100us)",
        )
        parser.add_argument(
            "--stall-rate",
            dest="net_stall_rate",
            default=net_params.stall_rate,
            type=float,
            metavar="HZ",
            help="per-connection Poisson rate of stall messages (Hz, 0 disables)",
        )
        parser.add_argument(
            "--stall-duration",
            dest="net_stall_duration",
            default=net_params.stall_duration,
            metavar="DURATION",
            help="stall duration per stall event (e.g. 100us, 1ms)",
        )
        parser.add_argument(
            "--print-counters",
            dest="net_print_counters",
            action="store_true",
            help="print perf counters after each run",
        )
        parser.add_argument(
            "--flamegraph",
            dest="net_flamegraph",
            action="store_true",
            help="profile client and generate flamegraph SVG",
        )
        parser.add_argument(
            "--timeout",
            dest="net_timeout",
            default=180,
            type=int,
            metavar="SECONDS",
            help="per-run timeout in seconds (default: 180, 0=none)",
        )

    net_perf_parser = sub.add_parser("net-perf", help="build then run net-perf")
    _add_net_args(net_perf_parser)

    net_perf_asio_parser = sub.add_parser(
        "net-perf-asio",
        help="build then run net-perf-asio (Boost.Asio C++20 coroutines)",
    )
    _add_net_args(net_perf_asio_parser)

    net_perf_epoll_parser = sub.add_parser(
        "net-perf-epoll",
        help="build then run net-perf-epoll (raw epoll)",
    )
    _add_net_args(net_perf_epoll_parser)

    #
    # http-perf
    #

    http_params = HttpPerfParams()
    http_perf_parser = sub.add_parser("http-perf", help="build then run http-perf")
    http_perf_parser.add_argument("--host", dest="http_host", default=http_params.host)
    http_perf_parser.add_argument(
        "--port", dest="http_port", default=http_params.port, type=int
    )
    http_perf_parser.add_argument(
        "--duration",
        dest="http_duration",
        default=http_params.duration,
        metavar="DURATION",
    )
    http_perf_parser.add_argument(
        "--warmup",
        dest="http_warmup",
        default=http_params.warmup,
        metavar="DURATION",
    )
    http_perf_parser.add_argument(
        "--connections",
        dest="http_connections",
        default=http_params.connections,
        type=int,
        nargs="+",
        metavar="N",
    )
    http_perf_parser.add_argument(
        "--delay",
        dest="http_delay",
        default=http_params.delay,
        metavar="DURATION",
        help="server-side response delay per request (e.g. 1ms, 100us)",
    )
    http_perf_parser.add_argument(
        "--nginx",
        dest="http_nginx",
        action="store_true",
        help="run client against nginx instead of the internal server",
    )
    http_perf_parser.add_argument(
        "--threads",
        dest="http_threads",
        action="store_true",
        help="use thread-per-connection mode",
    )
    http_perf_parser.add_argument(
        "--flamegraph",
        dest="http_flamegraph",
        action="store_true",
        help="profile client and generate flamegraph SVG",
    )
    http_perf_parser.add_argument(
        "--print-counters",
        dest="http_print_counters",
        action="store_true",
        help="print perf counters after each run",
    )
    http_perf_parser.add_argument(
        "--timeout",
        dest="http_timeout",
        default=180,
        type=int,
        metavar="SECONDS",
        help="per-run timeout in seconds (default: 180, 0=none)",
    )

    #
    # s3-perf
    #

    s3_params = S3PerfParams()
    s3_perf_parser = sub.add_parser("s3-perf", help="build then run s3-perf")
    s3_perf_parser.add_argument(
        "--endpoint", dest="s3_endpoint", default=s3_params.endpoint
    )
    s3_perf_parser.add_argument("--bucket", dest="s3_bucket", default=s3_params.bucket)
    s3_perf_parser.add_argument("--key", dest="s3_key", default=s3_params.key)
    s3_perf_parser.add_argument("--region", dest="s3_region", default=s3_params.region)
    s3_perf_parser.add_argument(
        "--access-key", dest="s3_access_key", default=s3_params.access_key
    )
    s3_perf_parser.add_argument(
        "--secret-key", dest="s3_secret_key", default=s3_params.secret_key
    )
    s3_perf_parser.add_argument(
        "--size", dest="s3_size", default=s3_params.size, type=int
    )
    s3_perf_parser.add_argument(
        "--duration",
        dest="s3_duration",
        default=s3_params.duration,
        metavar="DURATION",
    )
    s3_perf_parser.add_argument(
        "--warmup",
        dest="s3_warmup",
        default=s3_params.warmup,
        metavar="DURATION",
    )
    s3_perf_parser.add_argument(
        "--numjobs",
        dest="s3_numjobs",
        type=int,
        nargs="+",
        default=s3_params.numjobs,
        metavar="N",
    )
    s3_perf_parser.add_argument(
        "--iodepth",
        dest="s3_iodepth",
        type=int,
        nargs="+",
        default=s3_params.iodepth,
        metavar="N",
    )
    s3_perf_parser.add_argument(
        "--rw",
        dest="s3_rw",
        nargs="+",
        default=s3_params.rw,
        choices=["read", "write", "readwrite"],
    )
    s3_perf_parser.add_argument(
        "--threads",
        dest="s3_threads",
        action="store_true",
        help="also run with thread executor",
    )
    s3_perf_parser.add_argument(
        "--flamegraph",
        dest="s3_flamegraph",
        action="store_true",
        help="profile first config and generate flamegraph SVG",
    )
    s3_perf_parser.add_argument(
        "--data-dir",
        dest="s3_data_dir",
        default=s3_params.data_dir,
        metavar="PATH",
        help="MinIO data directory",
    )
    s3_perf_parser.add_argument(
        "--print-counters",
        dest="s3_print_counters",
        action="store_true",
        help="print perf counters after each run",
    )
    s3_perf_parser.add_argument(
        "--timeout",
        dest="s3_timeout",
        default=180,
        type=int,
        metavar="SECONDS",
        help="per-run timeout in seconds (default: 180, 0=none)",
    )

    return parser


def main() -> None:
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

    if args.command == "clean":
        _check_no_extra(extra)
        cmd_clean()
    elif args.command == "fmt":
        _check_no_extra(extra)
        cmd_fmt(args.check)
    elif args.command == "configure":
        _check_no_extra(extra)
        cmd_configure(preset, _params_from_args(args, "build", BuildParams))
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
    elif args.command == "file-perf":
        _check_no_extra(extra)
        cmd_build(preset, ["file-perf"])
        cmd_file_perf(preset, _params_from_args(args, "file", FilePerfParams))
    elif args.command == "fio-perf":
        _check_no_extra(extra)
        cmd_fio_perf(_params_from_args(args, "fio", FilePerfParams))
    elif args.command == "net-perf":
        _check_no_extra(extra)
        cmd_build(preset, ["net-perf"])
        params = _params_from_args(args, "net", NetPerfParams)
        cmd_net_perf(preset, replace(params, engine=NetPerfEngine.FIBERS))
    elif args.command == "net-perf-asio":
        _check_no_extra(extra)
        cmd_build(preset, ["net-perf-asio"])
        params = _params_from_args(args, "net", NetPerfParams)
        cmd_net_perf(preset, replace(params, engine=NetPerfEngine.ASIO))
    elif args.command == "net-perf-epoll":
        _check_no_extra(extra)
        cmd_build(preset, ["net-perf-epoll"])
        params = _params_from_args(args, "net", NetPerfParams)
        cmd_net_perf(preset, replace(params, engine=NetPerfEngine.EPOLL))
    elif args.command == "http-perf":
        _check_no_extra(extra)
        cmd_build(preset, ["http-perf"])
        cmd_http_perf(preset, _params_from_args(args, "http", HttpPerfParams))
    elif args.command == "s3-perf":
        _check_no_extra(extra)
        cmd_build(preset, ["s3-perf"])
        cmd_s3_perf(preset, _params_from_args(args, "s3", S3PerfParams))
    elif args.command == "perf":
        _check_no_extra(extra)
        timing_overrides: dict[str, str] = {}
        if args.duration is not None:
            timing_overrides["duration"] = args.duration
        if args.warmup is not None:
            timing_overrides["warmup"] = args.warmup
        targets = set(args.targets)
        if "all" in targets:
            targets = {
                "file",
                "fio",
                "net",
                "net-asio",
                "net-epoll",
                "http",
                "http-threads",
                "http-nginx",
                "s3",
                "s3-threads",
            }
        file_params = FilePerfParams(
            numjobs=[1, 16],
            iodepth=[1, 16],
            rw=["randwrite", "randread"],
            timeout=args.timeout,
            **timing_overrides,
        )
        if "file" in targets:
            cmd_build(preset, ["file-perf"])
            cmd_file_perf(preset, file_params)
        if "fio" in targets:
            cmd_fio_perf(file_params)
        net_params = NetPerfParams(
            connections=[1, 256, 512, 1024],
            timeout=args.timeout,
            **timing_overrides,
        )
        if "net" in targets:
            cmd_build(preset, ["net-perf"])
            cmd_net_perf(preset, replace(net_params, engine=NetPerfEngine.FIBERS))
        if "net-asio" in targets:
            cmd_build(preset, ["net-perf-asio"])
            cmd_net_perf(preset, replace(net_params, engine=NetPerfEngine.ASIO))
        if "net-epoll" in targets:
            cmd_build(preset, ["net-perf-epoll"])
            cmd_net_perf(preset, replace(net_params, engine=NetPerfEngine.EPOLL))
        http_params = HttpPerfParams(
            connections=[1, 256, 512, 1024],
            timeout=args.timeout,
            **timing_overrides,
        )
        if "http" in targets:
            cmd_build(preset, ["http-perf"])
            cmd_http_perf(preset, http_params)
        if "http-threads" in targets:
            cmd_build(preset, ["http-perf"])
            cmd_http_perf(preset, replace(http_params, threads=True))
        if "http-nginx" in targets:
            cmd_build(preset, ["http-perf"])
            cmd_http_perf(preset, replace(http_params, nginx=True))
        s3_params = S3PerfParams(
            numjobs=[1, 16],
            iodepth=[1, 64],
            rw=["read", "write"],
            timeout=args.timeout,
            **timing_overrides,
        )
        if "s3" in targets:
            cmd_build(preset, ["s3-perf"])
            cmd_s3_perf(preset, s3_params)
        if "s3-threads" in targets:
            cmd_build(preset, ["s3-perf"])
            cmd_s3_perf(preset, replace(s3_params, threads=True))
        print()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(1)
