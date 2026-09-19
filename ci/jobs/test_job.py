import platform
import sys

from praktika.result import Result

# Build variant to the bb flags that select preset and sanitizer.
_BUILD_FLAGS = {
    "coverage": "-b debug",
    "release": "-b release",
    "tsan": "-b release -s thread",
    "asan": "-b release -s address",
    "ubsan": "-b release -s undefined",
    "msan": "-b release -s memory",
}

# The optional components each variant configures; the sanitizer builds skip
# jemalloc, and the MSan build skips Poco too.
_COMPONENTS = {
    "coverage": [],
    "release": ["poco", "jemalloc"],
    "tsan": ["poco"],
    "asan": ["poco"],
    "ubsan": ["poco"],
    "msan": [],
}


def _arch():
    machine = platform.machine()
    return "arm64" if machine in ("aarch64", "arm64") else "amd64"


def _commands(build, arch):
    """The (step name, shell command) pairs the variant runs on the arch, in order."""
    bb = f"./bb {_BUILD_FLAGS[build]}"
    components = _COMPONENTS[build]
    commands = []

    if components:
        flags = " ".join(f"--build-{component}" for component in components)
        commands.append(("Configure", f"{bb} configure {flags}"))

    if build == "coverage":
        commands.append(("Build and test", f"{bb} test --coverage"))
        # Only the amd64 job publishes the report.
        if arch == "amd64":
            commands.append(
                (
                    "Package coverage HTML",
                    "mkdir -p ci/tmp && "
                    "tar -C build/debug-coverage/html -czf ci/tmp/coverage-html.tar.gz .",
                )
            )
        return commands

    commands.append(("Build and test", f"{bb} test"))
    commands.append(("Bench", f"{bb} bench"))
    # http-perf is built only with Poco.
    perf_targets = "file net http" if "poco" in components else "file net"
    commands.append(("Perf", f"{bb} perf {perf_targets}"))
    return commands


def main():
    build = sys.argv[1]
    results = [
        Result.from_commands_run(name=name, command=[command])
        for name, command in _commands(build, _arch())
    ]
    Result.create_from(results=results).complete_job()


if __name__ == "__main__":
    main()
