from praktika import Workflow
from ci.workflows.jobs import (
    BUILD_VARIANTS,
    COVERAGE_HTML_ARTIFACT,
    COVERAGE_JOB,
    FMT_JOB,
    PUBLISH_COVERAGE_REPORT_JOB,
    REBUILD_CLICKHOUSE_PUBLIC_JOB,
    TEST_AMD,
    TEST_ARM,
)

_TEST_JOBS = [
    FMT_JOB.copy(),
    # The Main run publishes the coverage report, so its coverage job provides the HTML.
    COVERAGE_JOB.set_provides(COVERAGE_HTML_ARTIFACT.name),
    *TEST_ARM.parametrize(*BUILD_VARIANTS),
    *TEST_AMD.parametrize(*BUILD_VARIANTS),
]


WORKFLOWS = [
    Workflow.Config(
        name="Main",
        event=Workflow.Event.PUSH,
        branches=["main"],
        jobs=[
            *_TEST_JOBS,
            PUBLISH_COVERAGE_REPORT_JOB.set_run_after(_TEST_JOBS),
            REBUILD_CLICKHOUSE_PUBLIC_JOB.set_run_after(_TEST_JOBS),
        ],
        artifacts=[COVERAGE_HTML_ARTIFACT],
        enable_cache=True,
        enable_report=True,
        enable_exit_code_result=True,
    )
]
