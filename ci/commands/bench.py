import glob
import os
import re
import sys

from ci.commands.process import ROOT, log, run, run_capture


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

    listed = {bench: list_tests(bench) for bench in benches}
    benches = [bench for bench in benches if listed[bench]]

    if tests_regex:
        pattern = re.compile(tests_regex)
        benches = [
            bench
            for bench in benches
            if any(pattern.search(name) for name in listed[bench])
        ]
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
