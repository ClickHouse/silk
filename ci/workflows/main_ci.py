from praktika import Job, Workflow
from ci.settings.settings import RunnerLabels


WORKFLOWS = [
    Workflow.Config(
        name="Main CI",
        event=Workflow.Event.PUSH,
        branches=["main", "add-praktika-ci-config"],
        jobs=[
            Job.Config(
                name="Hello World Test",
                runs_on=[RunnerLabels.SMALL_ARM],
                command='python3 -c \'print("hello world")\'',
            ),
        ],
        enable_report=True,
        enable_exit_code_result=True,
    )
]
