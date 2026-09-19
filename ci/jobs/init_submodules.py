import subprocess

from praktika.info import Info


COMMON_SUBMODULES = [
    "contrib/benchmark",
    "contrib/bpftool",
    "contrib/cxxopts",
    "contrib/googletest",
    "contrib/libbacktrace",
    "contrib/libbpf",
    "contrib/librseq",
    "contrib/liburing",
]

# The submodules a build variant needs beyond the common set, restored from the
# submodule cache like them.
EXTRA_SUBMODULES_BY_BUILD = {
    "release": ["contrib/poco", "contrib/jemalloc"],
    "tsan": ["contrib/poco"],
    "asan": ["contrib/poco"],
    "ubsan": ["contrib/poco"],
}

# llvm-project is pinned update=none in .gitmodules and is absent from the
# submodule cache; --checkout overrides that and fetches it on demand.
UNCACHED_SUBMODULES_BY_BUILD = {
    "msan": ["contrib/llvm-project"],
}


def run(*args):
    print("+", " ".join(args), flush=True)
    subprocess.run(args, check=True)


def checkout_submodules(paths, force=False):
    fetch_flag = "--checkout" if force else "--no-fetch"
    run(
        "git",
        "submodule",
        "update",
        "--init",
        fetch_flag,
        "--depth=1",
        "--jobs",
        "8",
        *paths,
    )


if __name__ == "__main__":
    job_name = Info().job_name

    run("git", "submodule", "sync")
    checkout_submodules(COMMON_SUBMODULES)

    for build, paths in EXTRA_SUBMODULES_BY_BUILD.items():
        if f"({build})" in job_name:
            checkout_submodules(paths)

    for build, paths in UNCACHED_SUBMODULES_BY_BUILD.items():
        if f"({build})" in job_name:
            checkout_submodules(paths, force=True)
