from praktika import Job, Workflow
from ci.settings.settings import RunnerLabels


WORKFLOWS = [
    Workflow.Config(
        name="Main CI",
        event=Workflow.Event.PUSH,
        branches=["main"],
        jobs=[
            Job.Config(
                name="Smoke Test",
                runs_on=[RunnerLabels.SMALL_ARM],
                command='python3 -c \'print("hello from main ci")\'',
            ),
        ],
        enable_report=True,
        enable_exit_code_result=True,
    )
]
