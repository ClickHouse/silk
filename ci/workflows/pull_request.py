from praktika import Job, Workflow
from ci.settings.settings import RunnerLabels

FMT_JOB = Job.Config(
    name="Formatting",
    runs_on=[RunnerLabels.SMALL_ARM],
    command="python3 ./ci/jobs/fmt_job.py",
    digest_config=Job.CacheDigestConfig(
        include_paths=["src", "include"],
    ),
)

_TEST_DIGEST = Job.CacheDigestConfig(
    include_paths=["src", "include", "CMakeLists.txt", "CMakePresets.json", "bb"],
    with_git_submodules=True,
)

_TEST_ARM = Job.Config(
    name="Test ARM",
    runs_on=[RunnerLabels.SMALL_ARM],
    command="python3 ./ci/jobs/test_job.py {PARAMETER}",
    needs_submodules=True,
    timeout=2 * 3600,
    digest_config=_TEST_DIGEST,
)

_TEST_AMD = Job.Config(
    name="Test AMD",
    runs_on=[RunnerLabels.SMALL_AMD],
    command="python3 ./ci/jobs/test_job.py {PARAMETER}",
    needs_submodules=True,
    timeout=2 * 3600,
    digest_config=_TEST_DIGEST,
)

_BUILD_VARIANTS = [
    Job.ParamSet(parameter="coverage"),
    Job.ParamSet(parameter="release"),
    Job.ParamSet(parameter="tsan"),
    Job.ParamSet(parameter="asan"),
    Job.ParamSet(parameter="ubsan"),
    Job.ParamSet(parameter="msan"),
]

WORKFLOWS = [
    Workflow.Config(
        name="Pull Request CI",
        event=Workflow.Event.PULL_REQUEST,
        base_branches=["main"],
        jobs=[
            FMT_JOB,
            *_TEST_ARM.parametrize(*_BUILD_VARIANTS),
            *_TEST_AMD.parametrize(*_BUILD_VARIANTS),
        ],
        enable_cache=True,
        enable_report=True,
        enable_gh_summary_comment=True,
        enable_exit_code_result=True,
    )
]
