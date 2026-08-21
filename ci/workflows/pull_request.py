from praktika import Workflow
from ci.workflows.jobs import (
    BUILD_VARIANTS,
    CODE_REVIEW_JOB,
    FMT_JOB,
    TEST_AMD,
    TEST_ARM,
)

WORKFLOWS = [
    Workflow.Config(
        name="PR",
        event=Workflow.Event.PULL_REQUEST,
        base_branches=["main"],
        jobs=[
            FMT_JOB.copy(),
            CODE_REVIEW_JOB.copy(),
            *TEST_ARM.parametrize(*BUILD_VARIANTS),
            *TEST_AMD.parametrize(*BUILD_VARIANTS),
        ],
        enable_cache=True,
        enable_report=True,
        enable_gh_summary_comment=True,
        enable_exit_code_result=True,
    )
]
