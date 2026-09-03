import re
import shlex

from praktika.info import Info
from praktika.result import Result
from praktika.utils import Shell

BRANCH = "clickhouse-public"
REPOSITORY = "ClickHouse/silk"
BOT_LOGINS = ("clickhouse-gh", "robot-clickhouse", "clickhouse-robot-gh")

TOKENIZED_GITHUB_URL = re.compile(r"(https://x-access-token:)[^@\s]+(@github\.com/)")

REMOVE_SUBMODULES = (
    "git rm -r -q $(git config -f .gitmodules --get-regexp '\\.path$' | cut -d' ' -f2)"
    " && git rm -q -f .gitmodules"
    ' && git commit -q -m "Remove all submodules from contrib"'
)

CONFLICT_COMMENT = """\
`{branch}` could not be rebuilt on `{sha}`: rebasing its reverts onto this commit \
conflicts.

ClickHouse pins a commit of `{branch}` as `contrib/silk`, so it stays on the previous \
commit until this is resolved by hand:

```
git fetch origin main {branch}
git checkout -B {branch} origin/{branch}~1
git rebase origin/main
# keep this branch's boost::context and unbundled cxxopts, take main's change around them
git rebase --continue
{remove_submodules}
git push -f origin {branch}
```

The next rebuild starts from whatever this branch holds, so nothing else needs updating.
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
        remove_submodules=REMOVE_SUBMODULES.replace(" && ", "\n"),
        mentions=_mentions(pull_request),
    )
    Shell.check(
        f"gh pr comment {pull_request} --repo {REPOSITORY} --body {shlex.quote(body)}",
        verbose=True,
    )


def rebuild():
    sha = Info().sha
    shallow = Shell.get_output("git rev-parse --is-shallow-repository") == "true"
    unshallow = "--unshallow " if shallow else ""
    if not Shell.check(
        'git config user.name "clickhouse-robot-gh"'
        ' && git config user.email "clickhouse-robot-gh@users.noreply.github.com"'
        f" && git fetch {unshallow}origin main {BRANCH}"
        f" && git checkout -B {BRANCH} origin/{BRANCH}~1",
        verbose=True,
    ):
        return False

    if not Shell.check("git rebase origin/main", verbose=True):
        Shell.check("git rebase --abort")
        _report_conflict(sha)
        return False

    if not Shell.check(REMOVE_SUBMODULES, verbose=True):
        return False

    return _authenticated_push(BRANCH)


if __name__ == "__main__":
    Result.from_commands_run(
        name="Rebuild clickhouse-public", command=rebuild
    ).complete_job()
