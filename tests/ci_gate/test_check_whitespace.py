# Tests for tools/ci/check_whitespace.py, the event-aware whitespace gate.
#
# The bare `git diff --check` this replaced ran on a clean CI checkout and so
# could not fail; every behaviour here is pinned on real fixture repositories
# under build/test-tmp: a committed trailing-whitespace line must fail the
# event range, clean commits must pass, the pull_request range must start at
# the merge-base (whitespace already on the base branch must NOT be reported),
# branch-creation pushes must fall back to the explicit baseline, and every
# underivable range must fail closed.
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import unittest
import uuid
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "ci"))

import check_whitespace as cw  # noqa: E402


def fixture_repo(name: str) -> Path:
    root = REPO_ROOT / "build" / "test-tmp" / f"{name}-{os.getpid()}-{uuid.uuid4().hex[:8]}"
    root.mkdir(parents=True)
    return root


class FixtureRepo:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.environment = dict(os.environ,
                                GIT_CONFIG_GLOBAL=str(path / "gitconfig"),
                                GIT_CONFIG_SYSTEM=str(path / "gitconfig"))

    def git(self, *arguments: str) -> str:
        completed = subprocess.run(
            ["git", "-C", str(self.path), *arguments], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace",
            timeout=120, check=False, env=self.environment)
        if completed.returncode != 0:
            raise AssertionError(f"fixture git {' '.join(arguments)} failed: {completed.stderr}")
        return completed.stdout.strip()

    def commit_file(self, name: str, content: bytes, message: str) -> str:
        (self.path / name).write_bytes(content)
        self.git("add", name)
        self.git("-c", "user.name=fixture", "-c", "user.email=fixture@example.invalid",
                 "commit", "-m", message)
        return self.git("rev-parse", "HEAD")


class WhitespaceGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.path = fixture_repo("whitespace")
        self.addCleanup(shutil.rmtree, self.path, ignore_errors=True)
        self.repo = FixtureRepo(self.path)
        self.repo.git("init")

    def test_committed_trailing_whitespace_fails_the_range(self) -> None:
        base = self.repo.commit_file("clean.txt", b"clean line\n", "clean base")
        head = self.repo.commit_file("dirty.txt", b"trailing space   \n", "dirty commit")
        problems = cw.check_range(self.path, base, head)
        self.assertTrue(problems, "trailing whitespace in the committed range must be reported")
        self.assertIn("dirty.txt", problems[0])

    def test_clean_commits_pass_the_range(self) -> None:
        base = self.repo.commit_file("clean.txt", b"clean line\n", "clean base")
        head = self.repo.commit_file("more.txt", b"more clean\n", "clean head")
        self.assertEqual(cw.check_range(self.path, base, head), [])

    def test_whitespace_on_the_base_branch_is_not_reported(self) -> None:
        base = self.repo.commit_file("base.txt", b"trailing on base   \n", "whitespace on base")
        head = self.repo.commit_file("feature.txt", b"clean feature\n", "clean feature")
        self.assertEqual(cw.check_range(self.path, base, head), [])

    def test_pull_request_range_starts_at_the_merge_base(self) -> None:
        # The base branch gets a whitespace commit AFTER the feature diverges;
        # the PR range must not report it, only the feature's own commits.
        default_branch = self.repo.git("symbolic-ref", "--short", "HEAD")
        self.repo.commit_file("trunk.txt", b"trunk\n", "trunk")
        self.repo.git("checkout", "-b", "feature")
        feature_head = self.repo.commit_file("feature.txt", b"feature\n", "feature work")
        self.repo.git("checkout", default_branch)
        base_tip = self.repo.commit_file("base.txt", b"trailing   \n", "whitespace on base")
        start, end = cw.resolve_range(
            event="pull_request", base_sha=base_tip, head_sha=feature_head,
            before_sha="", after_sha="", baseline_ref="", repo_root=self.path)
        self.assertEqual(start, self.repo.git("merge-base", base_tip, feature_head))
        self.assertEqual(end, feature_head)
        self.assertEqual(cw.check_range(self.path, start, end), [])

    def test_push_event_range_is_before_after(self) -> None:
        base = self.repo.commit_file("a.txt", b"a\n", "a")
        head = self.repo.commit_file("b.txt", b"trailing   \n", "b with whitespace")
        start, end = cw.resolve_range(
            event="push", base_sha="", head_sha="", before_sha=base, after_sha=head,
            baseline_ref="", repo_root=self.path)
        self.assertEqual((start, end), (base, head))
        self.assertTrue(cw.check_range(self.path, start, end))

    def test_branch_creation_falls_back_to_the_baseline(self) -> None:
        # Simulate the hosted layout: the fixture is its own "origin"; a new
        # branch grows from the baseline tip and adds whitespace.
        baseline = self.repo.commit_file("main.txt", b"main\n", "main")
        self.repo.git("update-ref", "refs/remotes/origin/main", baseline)
        head = self.repo.commit_file("new.txt", b"trailing   \n", "new branch work")
        start, end = cw.resolve_range(
            event="push", base_sha="", head_sha="", before_sha=cw.ZERO_SHA,
            after_sha=head, baseline_ref="origin/main", repo_root=self.path)
        self.assertEqual((start, end), (baseline, head))
        self.assertTrue(cw.check_range(self.path, start, end))

    def test_undivergable_push_fails_closed(self) -> None:
        baseline = self.repo.commit_file("main.txt", b"main\n", "main")
        head = self.repo.commit_file("new.txt", b"more\n", "new work")
        with self.assertRaises(cw.GateError):
            cw.resolve_range(event="push", base_sha="", head_sha="", before_sha=cw.ZERO_SHA,
                             after_sha=head, baseline_ref="origin/absent", repo_root=self.path)
        del baseline

    def test_unknown_event_fails_closed(self) -> None:
        head = self.repo.commit_file("a.txt", b"a\n", "a")
        with self.assertRaises(cw.GateError):
            cw.resolve_range(event="workflow_dispatch", base_sha="", head_sha="",
                             before_sha="", after_sha=head, baseline_ref="origin/main",
                             repo_root=self.path)

    def test_pull_request_with_missing_objects_fails_closed(self) -> None:
        head = self.repo.commit_file("a.txt", b"a\n", "a")
        absent = "1" * 40
        with self.assertRaises(cw.GateError):
            cw.resolve_range(event="pull_request", base_sha=absent, head_sha=head,
                             before_sha="", after_sha="", baseline_ref="",
                             repo_root=self.path)


if __name__ == "__main__":
    unittest.main()
