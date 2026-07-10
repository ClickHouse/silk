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
        "perf": "./bb -b release perf file net http",
    },
    "tsan": {
        "configure": "./bb -b release -s thread configure --build-poco",
        "test": "./bb -b release -s thread test",
        "bench": "./bb -b release -s thread bench",
        "perf": "./bb -b release -s thread perf file net http",
    },
    "asan": {
        "configure": "./bb -b release -s address configure --build-poco",
        "test": "./bb -b release -s address test",
        "bench": "./bb -b release -s address bench",
        "perf": "./bb -b release -s address perf file net http",
    },
    "ubsan": {
        "configure": "./bb -b release -s undefined configure --build-poco",
        "test": "./bb -b release -s undefined test",
        "bench": "./bb -b release -s undefined bench",
        "perf": "./bb -b release -s undefined perf file net http",
    },
    "msan": {
        "test": "./bb -b release -s memory test",
        "bench": "./bb -b release -s memory bench",
        "perf": "./bb -b release -s memory perf file net",
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
