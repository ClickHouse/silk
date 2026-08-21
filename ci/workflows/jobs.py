from praktika import Artifact, Job
from ci.settings.settings import RunnerLabels

COVERAGE_HTML_ARTIFACT = Artifact.Config(
    type=Artifact.Type.S3,
    name="coverage-html",
    path="ci/tmp/coverage-html.tar.gz",
)

FMT_JOB = Job.Config(
    name="Formatting",
    runs_on=[RunnerLabels.SMALL_ARM],
    command="python3 ./ci/jobs/fmt_job.py",
    digest_config=Job.CacheDigestConfig(
        include_paths=["src", "include"],
    ),
)

CODE_REVIEW_JOB = Job.Config(
    name="Code Review",
    runs_on=[RunnerLabels.SMALL_ARM_BEDROCK],
    command=(
        "python3 -I -m praktika review --provider bedrock-openai "
        "--model global.openai.gpt-5.6-sol --reasoning-effort high "
        "--prompt ./ci/prompts/code_review.md"
    ),
    allow_failure=True,
    enable_gh_auth=True,
)

TEST_DIGEST = Job.CacheDigestConfig(
    include_paths=[
        "src",
        "include",
        "CMakeLists.txt",
        "CMakePresets.json",
        "bb",
        "ci/jobs/init_submodules.py",
    ],
    with_git_submodules=True,
)

CHECKOUT_TEST_SUBMODULES = "python3 ./ci/jobs/init_submodules.py"

TEST_ARM = Job.Config(
    name="Test ARM",
    runs_on=[RunnerLabels.MEDIUM_ARM],
    command="python3 ./ci/jobs/test_job.py {PARAMETER}",
    needs_submodules=True,
    pre_hooks=[CHECKOUT_TEST_SUBMODULES],
    timeout=2 * 3600,
    digest_config=TEST_DIGEST,
)

TEST_AMD = Job.Config(
    name="Test AMD",
    runs_on=[RunnerLabels.MEDIUM_AMD],
    command="python3 ./ci/jobs/test_job.py {PARAMETER}",
    needs_submodules=True,
    pre_hooks=[CHECKOUT_TEST_SUBMODULES],
    timeout=2 * 3600,
    digest_config=TEST_DIGEST,
)

BUILD_VARIANTS = [
    Job.ParamSet(parameter="coverage"),
    Job.ParamSet(parameter="release"),
    Job.ParamSet(parameter="tsan"),
    Job.ParamSet(parameter="asan"),
    Job.ParamSet(parameter="ubsan"),
    Job.ParamSet(parameter="msan"),
]

BUILD_VARIANTS_WITH_COVERAGE_ARTIFACT = [
    Job.ParamSet(parameter="coverage", provides=[COVERAGE_HTML_ARTIFACT.name]),
    Job.ParamSet(parameter="release"),
    Job.ParamSet(parameter="tsan"),
    Job.ParamSet(parameter="asan"),
    Job.ParamSet(parameter="ubsan"),
    Job.ParamSet(parameter="msan"),
]

PUBLISH_COVERAGE_REPORT_JOB = Job.Config(
    name="Publish Coverage Report",
    runs_on=[RunnerLabels.SMALL_AMD],
    command="python3 ./ci/jobs/deploy_pages.py",
    requires=[COVERAGE_HTML_ARTIFACT.name],
    timeout=30 * 60,
    enable_gh_auth=True,
)

REBUILD_CLICKHOUSE_PUBLIC_JOB = Job.Config(
    name="Rebuild clickhouse-public",
    runs_on=[RunnerLabels.SMALL_ARM],
    command="python3 ./ci/jobs/rebuild_clickhouse_public.py",
    timeout=15 * 60,
    enable_gh_auth=True,
)
