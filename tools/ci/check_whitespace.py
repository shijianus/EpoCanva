#!/usr/bin/env python3
"""Event-aware whitespace gate for committed changes.

The bare ``git diff --check`` this replaces checked whatever happened to be
in the runner's working tree -- usually nothing, since CI checkouts are
clean -- so it could not fail. This checker derives the committed range the
CI event actually introduced and runs ``git diff --check`` over exactly
that range:

- ``pull_request``: the merge-base of the PR base SHA and the head SHA,
  compared against the head SHA, so the gate covers exactly the commits the
  PR adds and never re-reports whitespace already on the base branch.
- ``push``: ``before..after`` from the event payload. When ``before`` is the
  zero SHA (branch creation) or is not an ancestor of ``after`` (rewritten
  history), the range falls back to an explicit baseline ref
  (``--baseline-ref``, default ``origin/main``) via its merge-base with
  ``after``.
- any other event, or a range that cannot be resolved: fail closed, never
  silently pass.

Exit codes: 0 no whitespace problems in the event range; 1 problems found
or the range could not be determined (fail closed); 2 invalid arguments.
Only file names and line numbers are printed, never line contents.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ZERO_SHA = "0" * 40
SUPPORTED_EVENTS = ("pull_request", "push")


class GateError(RuntimeError):
    """The comparison range could not be determined; fail closed."""


def git(repo_root: Path, *arguments: str) -> str:
    try:
        completed = subprocess.run(
            ["git", "-C", str(repo_root), *arguments], stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            encoding="utf-8", errors="replace", timeout=300, shell=False, check=False)
    except OSError as exc:
        raise GateError(f"cannot run git: {exc}") from exc
    if completed.returncode != 0:
        raise GateError(f"git {arguments[0]} failed with status {completed.returncode}")
    return completed.stdout


def is_commit(repo_root: Path, revision: str) -> bool:
    try:
        git(repo_root, "rev-parse", "--verify", "--quiet", revision + "^{commit}")
        return True
    except GateError:
        return False


def resolve_range(*, event: str, base_sha: str, head_sha: str, before_sha: str,
                  after_sha: str, baseline_ref: str, repo_root: Path) -> tuple[str, str]:
    """Return the (start, end) commit range this CI event introduced."""
    if event == "pull_request":
        if not base_sha or not head_sha:
            raise GateError("pull_request event without base/head SHAs; failing closed")
        if not is_commit(repo_root, base_sha) or not is_commit(repo_root, head_sha):
            raise GateError("pull_request base/head objects are unavailable locally; "
                            "the checkout depth is insufficient; failing closed")
        merge_base = git(repo_root, "merge-base", base_sha, head_sha).strip()
        if not merge_base:
            raise GateError("pull_request merge-base is empty; failing closed")
        return merge_base, head_sha
    if event == "push":
        if not after_sha or not is_commit(repo_root, after_sha):
            raise GateError("push event without a resolvable after SHA; failing closed")
        if before_sha and before_sha != ZERO_SHA and is_commit(repo_root, before_sha):
            try:
                git(repo_root, "merge-base", "--is-ancestor", before_sha, after_sha)
                return before_sha, after_sha
            except GateError:
                pass  # before is not an ancestor of after: use the baseline fallback
        # Branch creation (before == zero SHA) or a non-ancestor before SHA:
        # fall back to the explicit baseline's merge-base, which the caller
        # must have fetched; otherwise fail closed.
        if baseline_ref and is_commit(repo_root, baseline_ref):
            merge_base = git(repo_root, "merge-base", baseline_ref, after_sha).strip()
            if merge_base:
                return merge_base, after_sha
        raise GateError("push event did not provide a usable before SHA and no baseline "
                        "ref resolves; failing closed")
    raise GateError(f"event {event!r} has no defined comparison range; failing closed")


def check_range(repo_root: Path, start: str, end: str) -> list[str]:
    """Run git diff --check over the committed range; return offending lines.

    Output lines carry file names and line numbers only (git's own format),
    never the offending content, so the log stays free of committed text.
    """
    try:
        completed = subprocess.run(
            ["git", "-C", str(repo_root), "diff", "--check", f"{start}..{end}"],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace", timeout=300, shell=False,
            check=False)
    except OSError as exc:
        raise GateError(f"cannot run git diff --check: {exc}") from exc
    # git diff --check exits 2 when problems are found and 0 on a clean diff;
    # anything else (e.g. missing objects) is a range failure, not a pass.
    if completed.returncode not in (0, 2):
        raise GateError(f"git diff --check failed with status {completed.returncode}")
    return [line for line in completed.stdout.splitlines() if line.strip()]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--event", required=True,
                        help="CI event name (pull_request or push)")
    parser.add_argument("--base-sha", default="", help="pull_request base SHA")
    parser.add_argument("--head-sha", default="", help="pull_request head SHA")
    parser.add_argument("--before-sha", default="", help="push event before SHA")
    parser.add_argument("--after-sha", default="", help="push event after SHA")
    parser.add_argument("--baseline-ref", default="origin/main",
                        help="fallback baseline for branch-creation pushes")
    parser.add_argument("--repo-root", default=".",
                        help="repository working tree the SHAs belong to")
    args = parser.parse_args(argv)
    repo_root = Path(args.repo_root).resolve()
    try:
        start, end = resolve_range(
            event=args.event, base_sha=args.base_sha, head_sha=args.head_sha,
            before_sha=args.before_sha, after_sha=args.after_sha,
            baseline_ref=args.baseline_ref, repo_root=repo_root)
        problems = check_range(repo_root, start, end)
    except GateError as exc:
        print(f"whitespace gate: {exc}", file=sys.stderr)
        return 1
    if problems:
        for line in problems:
            print(line)
        print(f"whitespace gate: {len(problems)} problem(s) in {start}..{end}")
        return 1
    print(f"whitespace gate: clean over {start}..{end}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
