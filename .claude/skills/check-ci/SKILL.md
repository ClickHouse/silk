---
name: check-ci
description: Fetch and report CI results for a silk PR. Use when the user asks to investigate, address, or fix CI failures, or refers to a PR without specifying what's broken.
---

Always fetch the CI result JSON before reading code or proposing fixes.

## Determining PR, sha, and workflow name

**From a Praktika report URL** (e.g. `https://silk-artifacts-eu-north-1.s3.amazonaws.com/json.html?PR=12&sha=abc123&name_0=Pull%20Request%20CI`):
- Extract `PR`, `sha`, `name_0` from query params
- Normalize `name_0`: lowercase + spaces → underscores (e.g. `"Pull Request CI"` → `"pull_request_ci"`)

**From a bare PR number:**
- Run `gh pr view {PR} --json headRefOid --jq '.headRefOid'` to get the latest sha
- Use `pull_request_ci` as the normalized workflow name

## Building the JSON URL

```
https://silk-artifacts-eu-north-1.s3.amazonaws.com/PRs/{PR}/{sha}/{normalized}/result_{normalized}.json
```

Fetch this URL with WebFetch.

## Result structure

The JSON is a serialized `praktika.Result`:

```
{
  "name": str,
  "status": str,       # OK | FAIL | ERROR | SKIPPED | UNKNOWN | XFAIL | XPASS | PENDING | RUNNING | DROPPED
  "start_time": float?,
  "duration": float?,
  "info": str,
  "results": [...],    # nested Result objects, same shape, recursive
  "files": [...],
  "links": [...],
  "ext": {
    "labels": [{"name": str, "link": str?, "hint": str?}, ...],
    "warnings": [...],
    "errors": [...],
    "report_url": str?,
    ...
  }
}
```

Walk the `results` tree recursively to find all failing jobs and sub-jobs. Use the `info` field of failing nodes as the primary signal for what went wrong before touching any code.
