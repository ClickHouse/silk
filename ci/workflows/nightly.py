from praktika import Workflow
from ci.settings.settings import MAIN_BRANCH
from ci.workflows.jobs import (
    BUILD_VARIANTS,
    TEST_AMD,
    TEST_ARM,
)

# GitHub owns the cron timing and the manual "Run workflow" button; the thin
# ignition workflow signs a trigger and the real test matrix runs on the native
# Praktika engine. Pinned to the default branch so the nightly run tracks main.
WORKFLOWS = [
    Workflow.Config(
        name="Nightly",
        event=Workflow.Event.SCHEDULE,
        engine=Workflow.Engine.GH_IGNITION,
        branches=[MAIN_BRANCH],
        cron_schedules=["0 2 * * *"],
        jobs=[
            *TEST_ARM.parametrize(*BUILD_VARIANTS),
            *TEST_AMD.parametrize(*BUILD_VARIANTS),
        ],
        enable_cache=True,
        enable_report=True,
        enable_exit_code_result=True,
    )
]
