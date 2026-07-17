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

EXTRA_SUBMODULES_BY_BUILD = {
    "release": ["contrib/poco", "contrib/jemalloc"],
    "tsan": ["contrib/poco"],
    "asan": ["contrib/poco"],
    "ubsan": ["contrib/poco"],
    "msan": ["contrib/llvm-project"],
}


def run(*args):
    print("+", " ".join(args), flush=True)
    subprocess.run(args, check=True)


def checkout_submodules(paths):
    run(
        "git",
        "submodule",
        "update",
        "--init",
        "--no-fetch",
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
            break
