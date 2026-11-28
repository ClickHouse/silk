import sys

from praktika.result import Result

# Build variant to the bb flags that select preset and sanitizer.
_BUILD_FLAGS = {
    "debug": "-b debug",
    "release": "-b release",
    "tsan": "-b release -s thread",
    "asan": "-b release -s address",
    "ubsan": "-b release -s undefined",
    "msan": "-b release -s memory",
}

# The optional components each variant configures; the sanitizer builds skip
# jemalloc, and the MSan build skips Poco too.
_COMPONENTS = {
    "debug": [],
    "release": ["poco", "jemalloc", "lto"],
    "tsan": ["poco"],
    "asan": ["poco"],
    "ubsan": ["poco"],
    "msan": [],
}


def _commands(build):
    """The (step name, shell command, needed) triples the variant runs, in order; a failed needed step leaves nothing
    for the steps after it to run.
    """
    bb = f"./bb {_BUILD_FLAGS[build]}"
    components = _COMPONENTS[build]
    commands = []

    if components:
        flags = " ".join(f"--build-{component}" for component in components)
        commands.append(("Configure", f"{bb} configure {flags}", True))

    commands.append(("Build", f"{bb} build", True))
    commands.append(("Test", f"{bb} test", False))
    commands.append(("Bench", f"{bb} bench", False))
    # http-perf is built only with Poco.
    perf_targets = "file net http" if "poco" in components else "file net"
    commands.append(("Perf", f"{bb} perf {perf_targets}", False))
    return commands


def main():
    build = sys.argv[1]
    results = []
    for name, command, needed in _commands(build):
        result = Result.from_commands_run(name=name, command=[command])
        results.append(result)
        if needed and not result.is_ok():
            break
    Result.create_from(results=results).complete_job()


if __name__ == "__main__":
    main()
