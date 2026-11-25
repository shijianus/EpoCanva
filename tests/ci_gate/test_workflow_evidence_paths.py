"""Falsifiable regression for CTest evidence paths inside the CI workflow.

GitHub run 43570509740 (windows-ci #14, commit 80cb0bb) failed in
``root-unit-tests`` although CTest itself exited 0: the job passed a
*relative* ``--output-junit build/vs2022-x64-debug/ci/ctest.xml`` together
with ``--test-dir build/vs2022-x64-debug``. CTest resolves a relative
``--output-junit`` against its own test directory, so the report landed in
``build/vs2022-x64-debug/build/vs2022-x64-debug/ci/ctest.xml`` while
``verify_root_junit.py`` looked for the literal path the workflow named.
The verifier then failed closed with "No such file or directory", which is
correct behaviour for a missing file but was misread as a test failure.

The rule this test enforces: every evidence path handed to CTest in the
workflow must be an absolute path derived with ``[IO.Path]::GetFullPath``,
and the file must be proven present at that absolute path before the
verifier reads it. A missing file then means the suite truly produced no
report, never that the workflow wrote it somewhere else.

The checker is exercised against the real workflow (must pass) and against
mutated copies that restore each defect (must fail). A checker that cannot
reject the historical form would be worthless, so the negative controls are
part of the contract.
"""
from __future__ import annotations

import re
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "windows-ci.yml"
HOSTED = REPO_ROOT / "tools/ci/record_hosted_tests.ps1"

ABSOLUTE_PREFIX = "[IO.Path]::GetFullPath"
JUNIT_FLAG = "--output-junit"


def job_block(text: str, job: str) -> str:
    """Return the YAML block for one job, from its key to the next peer key."""
    pattern = re.compile(rf"^  {re.escape(job)}:\s*$", re.MULTILINE)
    match = pattern.search(text)
    if match is None:
        raise AssertionError(f"job {job!r} is not defined in the workflow")
    rest = text[match.end():]
    end = re.compile(r"^  [A-Za-z0-9_-]+:\s*$", re.MULTILINE).search(rest)
    return rest[: end.start()] if end else rest


def ctest_invocations(block: str) -> list[str]:
    """The lines that actually run CTest, including continued arguments."""
    return [line for line in block.splitlines() if JUNIT_FLAG in line and not line.lstrip().startswith('#')]


def check_evidence_paths(block: str) -> list[str]:
    """Violations of the absolute-evidence-path contract for one job."""
    violations: list[str] = []
    if not re.search(rf"\[IO\.Path\]::GetFullPath", block):
        violations.append("no [IO.Path]::GetFullPath anchor for evidence paths")

    for line in ctest_invocations(block):
        if JUNIT_FLAG not in line:
            continue
        # Everything after the flag up to the next option or redirect.
        tail = line.split(JUNIT_FLAG, 1)[1].lstrip("'\", ")
        argument = tail.split()[0].rstrip(")") if tail.split() else ""
        if not argument:
            violations.append(f"empty --output-junit argument: {line.strip()}")
        elif not argument.startswith("$"):
            violations.append(
                "relative --output-junit argument (CTest resolves it against "
                f"--test-dir): {argument}")
        else:
            # The variable must be built from the absolute anchor, not from a
            # bare relative literal.
            name = re.escape(argument)
            assignment = re.search(
                rf"^\s*{name}\s*=\s*Join-Path\s+(\S+)\s+['\"][^'\"]+['\"]\s*$",
                block, re.MULTILINE)
            if assignment is None:
                violations.append(
                    f"no Join-Path assignment for the --output-junit variable {argument}")
            else:
                anchor = re.escape(assignment.group(1))
                if not re.search(
                    rf"^\s*{anchor}\s*=\s*\[IO\.Path\]::GetFullPath\(",
                    block, re.MULTILINE):
                    violations.append("JUnit parent is not the absolute anchor")
    return violations


class WorkflowEvidencePathTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = WORKFLOW.read_text(encoding="utf-8")
        self.block = HOSTED.read_text(encoding="utf-8")

    def test_both_hosted_layers_call_the_checked_producer(self) -> None:
        for layer in ("contract-gate", "root-unit-tests"):
            block = job_block(self.text, layer)
            self.assertIn(f'record_hosted_tests.ps1 -Layer {layer}', block)
            self.assertIn('if: always()', block)
            self.assertIn(f'collect_layer_artifact.ps1 -Layer {layer}', block)

    def test_real_workflow_uses_absolute_evidence_paths(self) -> None:
        self.assertEqual(check_evidence_paths(self.block), [])

    def test_real_workflow_proves_the_junit_exists_before_verifying(self) -> None:
        self.assertIn("Test-Path -LiteralPath $junit", self.block)
        self.assertIn("verify_root_junit.py", self.block)
        # The existence check must precede the verifier call in the script.
        self.assertLess(
            self.block.index("Test-Path -LiteralPath $junit"),
            self.block.index("verify_root_junit.py"))

    def test_historical_relative_form_is_rejected(self) -> None:
        """The exact form that failed CI #14 must be caught by the checker."""
        mutated = self.block.replace(
            "'--output-junit', $junit",
            "--output-junit build/vs2022-x64-debug/ci/ctest.xml")
        self.assertNotEqual(mutated, self.block, "mutation did not apply")
        violations = check_evidence_paths(mutated)
        self.assertTrue(violations, "the checker accepted the failing form")
        self.assertTrue(any("relative --output-junit" in v for v in violations))

    def test_missing_absolute_anchor_is_rejected(self) -> None:
        mutated = self.block.replace(ABSOLUTE_PREFIX, "Join-Path")
        self.assertNotEqual(mutated, self.block, "mutation did not apply")
        violations = check_evidence_paths(mutated)
        self.assertTrue(any("GetFullPath" in v for v in violations))

    def test_unbound_variable_is_rejected(self) -> None:
        mutated = self.block.replace(
            "$junit = Join-Path $ci 'ctest.xml'", "$junit = 'ctest.xml'")
        self.assertNotEqual(mutated, self.block, "mutation did not apply")
        self.assertTrue(check_evidence_paths(mutated))

    def test_relative_parent_with_unrelated_absolute_anchor_is_rejected(self) -> None:
        mutated = self.block.replace("$junit = Join-Path $ci 'ctest.xml'",
                                     "$junit = Join-Path $relative 'ctest.xml'")
        self.assertNotEqual(mutated, self.block)
        self.assertTrue(check_evidence_paths(mutated))

    def test_removing_the_existence_guard_is_visible(self) -> None:
        self.assertIn("Hosted JUnit was not written at the absolute evidence path",
                      self.block)


class CTestEvidencePathIntegrationTests(unittest.TestCase):
    """Exercise actual CTest and the actual verifier, without compiling a target."""

    def test_relative_path_reproduces_failure_and_absolute_path_repairs_it(self) -> None:
        ctest = os.environ.get("EPOCANVA_CTEST")
        if not ctest or not Path(ctest).is_file():
            self.fail("EPOCANVA_CTEST must name the actual CTest executable")
        work = REPO_ROOT / "build" / "ci-path-tests"
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work, prefix="evidence path ") as temporary:
            caller = Path(temporary)
            test_dir = caller / "build" / "fixture"
            test_dir.mkdir(parents=True)
            python = Path(sys.executable).as_posix()
            (test_dir / "CTestTestfile.cmake").write_text(
                f'add_test(path.fixture "{python}" "-c" "assert 2 + 2 == 4")\n',
                encoding="utf-8")
            registered = caller / "registered.json"
            inventory = subprocess.run([ctest, "--test-dir", str(test_dir), "--show-only=json-v1"],
                                       capture_output=True, check=True, timeout=30)
            registered.write_bytes(inventory.stdout)
            verifier = REPO_ROOT / "tools" / "ci" / "verify_root_junit.py"
            relative = Path("build/fixture/ci/ctest.xml")
            expected = caller / relative
            misplaced = test_dir / relative

            def run(arguments: list[str]) -> subprocess.CompletedProcess[str]:
                return subprocess.run(arguments, cwd=caller, capture_output=True,
                                      text=True, timeout=30, check=False)

            old = run([ctest, "--test-dir", str(test_dir), "--no-tests=error",
                       "--output-junit", relative.as_posix()])
            self.assertEqual(old.returncode, 0, old.stdout + old.stderr)
            self.assertFalse(expected.exists())
            self.assertTrue(misplaced.is_file())
            rejected = run([sys.executable, "-B", str(verifier), "--registered",
                            str(registered), "--junit", str(expected)])
            self.assertEqual(rejected.returncode, 1, rejected.stdout + rejected.stderr)
            self.assertIn("ROOT JUNIT BLOCKED", rejected.stdout)

            expected.parent.mkdir(parents=True)
            fixed = run([ctest, "--test-dir", str(test_dir), "--no-tests=error",
                         "--output-junit", str(expected.resolve())])
            self.assertEqual(fixed.returncode, 0, fixed.stdout + fixed.stderr)
            self.assertGreater(expected.stat().st_size, 0)
            accepted = run([sys.executable, "-B", str(verifier), "--registered",
                            str(registered), "--junit", str(expected)])
            self.assertEqual(accepted.returncode, 0, accepted.stdout + accepted.stderr)
            self.assertIn("1 exact registered/executed tests", accepted.stdout)


if __name__ == "__main__":
    unittest.main()
