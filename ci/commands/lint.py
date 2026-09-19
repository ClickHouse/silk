from ci.commands.process import run


def cmd_lint() -> None:
    run(
        "mypy",
        "--strict",
        "--scripts-are-modules",
        "--explicit-package-bases",
        "--cache-dir",
        "build/mypy-cache",
        "bb",
        "ci/commands",
    )
