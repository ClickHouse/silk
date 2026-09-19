from praktika.result import Result

if __name__ == "__main__":
    Result.from_commands_run(
        name="Check formatting and types",
        command=["./bb fmt --check", "./bb lint"],
    ).complete_job()
