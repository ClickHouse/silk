import re
import shlex

from praktika.info import Info
from praktika.result import Result
from praktika.utils import Shell

BRANCH = "clickhouse-public"
REPOSITORY = "ClickHouse/silk"
BOT_LOGINS = ("clickhouse-gh", "robot-clickhouse", "clickhouse-robot-gh")

# How many times a rebuild may lose the race for the branch to an overlapping Main run.
PUSH_ATTEMPTS = 3

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


def _authenticated_fetch():
    shallow = Shell.get_output("git rev-parse --is-shallow-repository") == "true"
    unshallow = "--unshallow " if shallow else ""
    fetch = (
        'token="$(gh auth token)" && git fetch '
        f"{unshallow}https://x-access-token:${{token}}@github.com/{REPOSITORY}.git "
        "+refs/heads/main:refs/remotes/origin/main "
        f"+refs/heads/{BRANCH}:refs/remotes/origin/{BRANCH}"
    )
    return_code, stdout, stderr = Shell.get_res_stdout_stderr(fetch, verbose=False)
    if return_code == 0:
        return True
    output = "\n".join(part for part in (stdout, stderr) if part)
    print(f"ERROR: failed to fetch main and {BRANCH}")
    print(TOKENIZED_GITHUB_URL.sub(r"\1***\2", output))
    return False


def _authenticated_push(refs, lease):
    push = (
        'token="$(gh auth token)" && git push '
        f"--force-with-lease=refs/heads/{BRANCH}:{lease} "
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


def _holds_main():
    """Whether the fetched clickhouse-public already holds the fetched main: another Main run published it, or a newer one."""
    if not Shell.check(f"git merge-base --is-ancestor origin/main origin/{BRANCH}"):
        return False
    print(f"{BRANCH} already holds origin/main, nothing to publish")
    return True


def rebuild():
    sha = Info().sha
    if not Shell.check(
        'git config user.name "clickhouse-robot-gh"'
        ' && git config user.email "clickhouse-robot-gh@users.noreply.github.com"',
        verbose=True,
    ):
        return False

    if not _authenticated_fetch():
        return False

    for _ in range(PUSH_ATTEMPTS):
        if _holds_main():
            return True

        published = Shell.get_output(f"git rev-parse origin/{BRANCH}").strip()
        if not Shell.check(f"git checkout -B {BRANCH} origin/{BRANCH}~1", verbose=True):
            return False

        if not Shell.check("git rebase origin/main", verbose=True):
            Shell.check("git rebase --abort")
            _report_conflict(sha)
            return False

        if not Shell.check(REMOVE_SUBMODULES, verbose=True):
            return False

        if _authenticated_push(BRANCH, published):
            return True

        # Two Main runs can overlap: the lease rejects the push when the other run moved the branch since the
        # fetch, and the rebuild starts again from its tip. A push that failed with the branch in place fails the job.
        if not _authenticated_fetch():
            return False
        if Shell.get_output(f"git rev-parse origin/{BRANCH}").strip() == published:
            return False

    # The last fetch may show that the other run published this main.
    if _holds_main():
        return True
    print(f"ERROR: {BRANCH} moved under each of {PUSH_ATTEMPTS} pushes")
    return False


if __name__ == "__main__":
    Result.from_commands_run(
        name="Rebuild clickhouse-public", command=rebuild
    ).complete_job()
