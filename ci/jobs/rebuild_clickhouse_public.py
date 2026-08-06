import re
import shlex

from praktika.info import Info
from praktika.result import Result
from praktika.utils import Shell

BRANCH = "clickhouse-public"
CXXOPTS_TAG = "cxxopts-revert"
FCONTEXT_TAG = "fcontext-revert"
REPOSITORY = "ClickHouse/silk"
BOT_LOGINS = ("clickhouse-gh", "robot-clickhouse", "clickhouse-robot-gh")

TOKENIZED_GITHUB_URL = re.compile(r"(https://x-access-token:)[^@\s]+(@github\.com/)")

REMOVE_SUBMODULES = (
    "git rm -r -q $(git config -f .gitmodules --get-regexp '\\.path$' | cut -d' ' -f2)"
    " && git rm -q -f .gitmodules"
    ' && git commit -q -m "Remove all submodules from contrib"'
)

CONFLICT_COMMENT = """\
`{branch}` could not be rebuilt on `{sha}`: replaying `{cxxopts}` and `{fcontext}` \
onto this commit conflicts.

ClickHouse pins a commit of `{branch}` as `contrib/silk`, so it stays on the previous \
commit until this is resolved by hand:

```
git fetch --force origin main "refs/tags/{cxxopts}:refs/tags/{cxxopts}" "refs/tags/{fcontext}:refs/tags/{fcontext}"
git checkout -B {branch} origin/main
git cherry-pick {cxxopts} {fcontext}
# keep this branch's boost::context and unbundled cxxopts, take main's change around them
git cherry-pick --continue
git tag -f {cxxopts} HEAD~1 && git tag -f {fcontext} HEAD
{remove_submodules}
git push -f origin {branch} {cxxopts} {fcontext}
```

Moving the tags is what keeps the next rebuild conflict-free, so do not skip it.
{mentions}"""


def _authenticated_push(refs):
    push = (
        'token="$(gh auth token)" && git push -f '
        f"https://x-access-token:${{token}}@github.com/{REPOSITORY}.git {refs}"
    )
    return_code, stdout, stderr = Shell.get_res_stdout_stderr(push, verbose=False)
    if return_code == 0:
        return True
    output = "\n".join(part for part in (stdout, stderr) if part)
    print(f"ERROR: failed to push {refs}")
    print(TOKENIZED_GITHUB_URL.sub(r"\1***\2", output))
    return False


def _pull_request_number(sha):
    return Shell.get_output(
        f"gh api repos/{REPOSITORY}/commits/{sha}/pulls --jq '.[0].number // empty'"
    ).strip()


def _mentions(pull_request):
    logins = Shell.get_output(
        f"gh api repos/{REPOSITORY}/pulls/{pull_request} --jq '.user.login // empty';"
        f" gh api repos/{REPOSITORY}/pulls/{pull_request}/reviews --paginate"
        " --jq '.[].user.login // empty'"
    ).split()
    named = sorted(
        {
            login
            for login in logins
            if login not in BOT_LOGINS and not login.endswith("[bot]")
        }
    )
    if not named:
        return ""
    return "\ncc " + " ".join(f"@{login}" for login in named)


def _report_conflict(sha):
    pull_request = _pull_request_number(sha)
    if not pull_request:
        print(f"WARNING: no pull request found for {sha}, cannot report the conflict")
        return
    body = CONFLICT_COMMENT.format(
        branch=BRANCH,
        sha=sha[:12],
        cxxopts=CXXOPTS_TAG,
        fcontext=FCONTEXT_TAG,
        remove_submodules=REMOVE_SUBMODULES.replace(" && ", "\n"),
        mentions=_mentions(pull_request),
    )
    Shell.check(
        f"gh pr comment {pull_request} --repo {REPOSITORY} --body {shlex.quote(body)}",
        verbose=True,
    )


def rebuild():
    sha = Info().sha
    if not Shell.check(
        'git config user.name "clickhouse-robot-gh"'
        ' && git config user.email "clickhouse-robot-gh@users.noreply.github.com"'
        " && git fetch --force origin main"
        f' "refs/tags/{CXXOPTS_TAG}:refs/tags/{CXXOPTS_TAG}"'
        f' "refs/tags/{FCONTEXT_TAG}:refs/tags/{FCONTEXT_TAG}"'
        f" && git checkout -B {BRANCH} origin/main",
        verbose=True,
    ):
        return False

    if not Shell.check(f"git cherry-pick {CXXOPTS_TAG} {FCONTEXT_TAG}", verbose=True):
        Shell.check("git cherry-pick --abort")
        _report_conflict(sha)
        return False

    if not Shell.check(
        f"git tag -f {CXXOPTS_TAG} HEAD~1"
        f" && git tag -f {FCONTEXT_TAG} HEAD"
        f" && {REMOVE_SUBMODULES}",
        verbose=True,
    ):
        return False

    return _authenticated_push(f"{BRANCH} {CXXOPTS_TAG} {FCONTEXT_TAG}")


if __name__ == "__main__":
    Result.from_commands_run(
        name="Rebuild clickhouse-public", command=rebuild
    ).complete_job()
