from praktika.result import Result

if __name__ == "__main__":
    Result.from_commands_run(
        name="Check formatting",
        command=["./bb fmt --check"],
    ).complete_job()
