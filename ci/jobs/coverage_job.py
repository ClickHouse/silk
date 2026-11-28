from praktika.result import Result

# bb builds the coverage tree only through test --coverage; a failed run leaves no report to package.
_STEPS = [
    ("Build and test", "./bb test --coverage"),
    (
        "Package coverage HTML",
        "mkdir -p ci/tmp && "
        "tar -C build/debug-coverage/html -czf ci/tmp/coverage-html.tar.gz .",
    ),
]


def main():
    results = []
    for name, command in _STEPS:
        result = Result.from_commands_run(name=name, command=[command])
        results.append(result)
        if not result.is_ok():
            break
    Result.create_from(results=results).complete_job()


if __name__ == "__main__":
    main()
