from praktika import Workflow
from ci.workflows.jobs import (
    BUILD_VARIANTS,
    BUILD_VARIANTS_WITH_COVERAGE_ARTIFACT,
    COVERAGE_HTML_ARTIFACT,
    FMT_JOB,
    PUBLISH_COVERAGE_REPORT_JOB,
    TEST_AMD,
    TEST_ARM,
)

_TEST_JOBS = [
    FMT_JOB.copy(),
    *TEST_ARM.parametrize(*BUILD_VARIANTS),
    *TEST_AMD.parametrize(*BUILD_VARIANTS_WITH_COVERAGE_ARTIFACT),
]


WORKFLOWS = [
    Workflow.Config(
        name="Main",
        event=Workflow.Event.PUSH,
        branches=["main"],
        jobs=[
            *_TEST_JOBS,
            PUBLISH_COVERAGE_REPORT_JOB.set_run_after(_TEST_JOBS),
        ],
        artifacts=[COVERAGE_HTML_ARTIFACT],
        enable_cache=True,
        enable_report=True,
        enable_exit_code_result=True,
    )
]
