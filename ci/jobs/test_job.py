import sys

from praktika.result import Result

_CONFIGS = {
    "coverage": {
        "test": "./bb -b debug test --coverage",
        "package_coverage": (
            "mkdir -p ci/tmp && "
            "tar -C build/debug-coverage/html -czf ci/tmp/coverage-html.tar.gz ."
        ),
    },
    "release": {
        "configure": "./bb -b release configure --build-poco --build-jemalloc",
        "test": "./bb -b release test",
        "bench": "./bb -b release bench",
        "perf": "./bb -v -b release perf file net http",
    },
    "tsan": {
        "configure": "./bb -b release -s thread configure --build-poco",
        "test": "./bb -b release -s thread test",
        "bench": "./bb -b release -s thread bench",
        "perf": "./bb -v -b release -s thread perf file net http",
    },
    "asan": {
        "test": "./bb -b release -s address test",
    },
    "ubsan": {
        "test": "./bb -b release -s undefined test",
    },
    "msan": {
        "test": "./bb -b release -s memory test",
    },
}

if __name__ == "__main__":
    build = sys.argv[1]
    config = _CONFIGS[build]
    results = []

    if "configure" in config:
        results.append(
            Result.from_commands_run(
                name="Configure",
                command=[config["configure"]],
            )
        )

    results.append(
        Result.from_commands_run(
            name="Build and test",
            command=[config["test"]],
        )
    )

    if "package_coverage" in config:
        results.append(
            Result.from_commands_run(
                name="Package coverage HTML",
                command=[config["package_coverage"]],
            )
        )

    if "bench" in config:
        results.append(
            Result.from_commands_run(
                name="Bench",
                command=[config["bench"]],
            )
        )

    if "perf" in config:
        results.append(
            Result.from_commands_run(
                name="Perf",
                command=[config["perf"]],
            )
        )

    Result.create_from(results=results).complete_job()
