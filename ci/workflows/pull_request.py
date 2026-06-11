from praktika import Job, Workflow
from ci.settings.settings import RunnerLabels


WORKFLOWS = [
    Workflow.Config(
        name="Pull Request CI",
        event=Workflow.Event.PULL_REQUEST,
        base_branches=["main"],
        jobs=[
            Job.Config(
                name="Smoke Test",
                runs_on=[RunnerLabels.SMALL_ARM],
                command='python3 -c \'print("hello from praktika")\'',
            ),
        ],
        enable_report=True,
        enable_gh_summary_comment=True,
        enable_exit_code_result=True,
    )
]
