import os
import shutil
from dataclasses import dataclass

from ci.commands.coverage import gen_coverage_report
from ci.commands.process import ROOT, log, run


@dataclass
class BuildParams:
    """The optional components configure turns on."""

    poco: bool = False
    aws: bool = False
    jemalloc: bool = False
    lto: bool = False


def cmd_clean() -> None:
    build_dir = os.path.join(ROOT, "build")
    if os.path.exists(build_dir):
        shutil.rmtree(build_dir)
        log.info("removed %s", build_dir)
    else:
        log.info("nothing to clean")


def cmd_configure(preset: str, params: BuildParams) -> None:
    build_dir = os.path.join(ROOT, f"build/{preset}")
    if os.path.exists(build_dir):
        shutil.rmtree(build_dir)
    args = ["cmake", "--preset", preset]
    if params.poco:
        args += ["-DBUILD_POCO=ON"]
    if params.aws:
        args += ["-DBUILD_AWS=ON"]
    if params.jemalloc:
        args += ["-DBUILD_JEMALLOC=ON"]
    if params.lto:
        args += ["-DLTO=ON"]
    run(*args)


def cmd_build(preset: str, targets: list[str] | None = None) -> None:
    # A cancelled job leaves a tree without its cache; the build configures it again.
    if not os.path.isfile(os.path.join(ROOT, f"build/{preset}/CMakeCache.txt")):
        log.info(
            "build/%s has no CMake cache: configuring it with no optional components",
            preset,
        )
        cmd_configure(preset, BuildParams())
    args = ["cmake", "--build", "--preset", preset]
    for target in targets or []:
        args += ["--target", target]
    run(*args)


def cmd_test(
    preset: str,
    tests_regex: str | None = None,
    exclude_regex: str | None = None,
    show_only: bool = False,
    timeout: int = 0,
    repeat: str | None = None,
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

    # A filter that matches nothing fails rather than reporting a green run of zero tests.
    args = [
        "ctest",
        "--preset",
        preset,
        "--parallel",
        str(os.cpu_count()),
        "--no-tests=error",
    ]
    if tests_regex:
        args += ["--tests-regex", tests_regex]
    if exclude_regex:
        args += ["--exclude-regex", exclude_regex]
    if show_only:
        args += ["--show-only"]
    if timeout:
        args += ["--timeout", str(timeout)]
    if repeat:
        args += ["--repeat", repeat]
    if extra:
        args += extra
    run(*args, env=env)

    if coverage:
        assert profiles_dir is not None
        gen_coverage_report(preset, profiles_dir)
