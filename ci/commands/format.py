import glob
import os
import re
import shutil
import subprocess
import sys

from ci.commands.process import ROOT, log, run

_CLANG_FORMAT_MAJOR = 21


def _find_clang_format() -> str:
    """The clang-format binary of the required major version, the versioned name first."""
    for command in (f"clang-format-{_CLANG_FORMAT_MAJOR}", "clang-format"):
        path = shutil.which(command)
        if not path:
            continue
        result = subprocess.run(
            [path, "--version"], capture_output=True, text=True, check=False
        )
        match = re.search(r"clang-format version (\d+)", result.stdout)
        if match and int(match.group(1)) == _CLANG_FORMAT_MAJOR:
            return path
    log.error("clang-format %d not found", _CLANG_FORMAT_MAJOR)
    sys.exit(1)


def cmd_fmt(check: bool = False) -> None:
    sources = glob.glob(os.path.join(ROOT, "src/**/*.[ch]"), recursive=True)
    sources += glob.glob(os.path.join(ROOT, "src/**/*.cpp"), recursive=True)
    args = [_find_clang_format()]
    args += ["--dry-run", "--Werror"] if check else ["-i"]
    run(*args, *sources)

    # ci/tmp is praktika's work directory, not source.
    work_dir = os.path.join(ROOT, "ci/tmp/")
    python_sources = [os.path.join(ROOT, "bb")]
    python_sources += [
        path
        for path in glob.glob(os.path.join(ROOT, "ci/**/*.py"), recursive=True)
        if not path.startswith(work_dir)
    ]
    args = ["black", "--quiet"]
    if check:
        args += ["--check", "--diff"]
    run(*args, *python_sources)
