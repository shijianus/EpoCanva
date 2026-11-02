#!/usr/bin/env python3
"""Synthetic receipt fixtures for the fail-closed backend release gate.

These tests never create release evidence. They exercise the parser against an
explicitly marked temporary receipt and patch only the read-only current CTest
and source-identity probes. Every positive fixture is deliberately incomplete
for aggregate release readiness; it only proves the new receipt binding itself.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
GATE_PATH = ROOT / "tools/backend/check_backend_release.py"
spec = importlib.util.spec_from_file_location("epocanva_backend_release_gate_tests", GATE_PATH)
assert spec and spec.loader
_gate = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = _gate
spec.loader.exec_module(_gate)


class ReceiptFixture(unittest.TestCase):
    """Build a small current-shaped receipt entirely below a temporary build/."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / "build", prefix="backend-gate-")
        self.root = Path(self.temp.name)
        self.build = self.root / "build" / "backend-verification"
        self.build.mkdir(parents=True)
        self.tools = self.build / "tools"
        self.tools.mkdir()
        self.ctest = self.tools / "ctest.exe"
        self.ctest.write_bytes(b"synthetic ctest executable")
        self.cmake = self.tools / "cmake.exe"
        self.cmake.write_bytes(b"synthetic cmake executable")
        self.test_executable = self.build / "Release" / "backend-tests.exe"
        self.test_executable.parent.mkdir()
        self.test_executable.write_bytes(b"synthetic backend test executable")
        self.cache = self.build / "CMakeCache.txt"
        self.cache.write_text(
            "CMAKE_COMMAND:INTERNAL=" + str(self.cmake) + "\n" +
            "CMAKE_CTEST_COMMAND:INTERNAL=" + str(self.ctest) + "\n",
            encoding="utf-8")
        self.junit = self.build / "ctest.xml"
        self.names = ["backend.abi.synthetic", "backend.profiles.synthetic",
                      "prediction.abbreviation.synthetic"]
        self.command_rows = [
            {"name": name, "command": [str(self.test_executable), "--case", name],
             "config": "Release"}
            for name in self.names
        ]
        self.test_list = {"kind": "ctestTestfile", "version": {"major": 1, "minor": 0},
                          "tests": self.command_rows}
        self.test_list_path = self.build / "ctest-list.json"
        self.test_list_path.write_text(json.dumps(self.test_list, separators=(",", ":")) + "\n", encoding="utf-8")
        self.junit.write_text(
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<testsuite name="synthetic" tests="3" failures="0" errors="0" skipped="0">'
            + "".join(f'<testcase name="{name}" classname="{name}"/>' for name in self.names)
            + "</testsuite>\n", encoding="utf-8")
        self.source = {"head": "a" * 40, "commit": "a" * 40,
                       "source_sha256": "b" * 64, "dirty": False, "source_files": 3}
        self.receipt_path = self.build / "backend-verification-receipt.json"
        self.receipt = self._make_receipt()
        self.receipt_path.write_text(json.dumps(self.receipt, indent=2) + "\n", encoding="utf-8")
        self.current_document = self.test_list
        self.current_identity = self.source.copy()
        self.patches = mock.patch.multiple(
            _gate,
            _configured_ctest=mock.Mock(return_value=self.ctest),
            _run_current_ctest_list=mock.Mock(return_value=(self.current_document, self.names)),
        )
        self.patches.start()
        self.addCleanup(self.patches.stop)
        self.addCleanup(self.temp.cleanup)

    @staticmethod
    def _sha(path: Path) -> str:
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def _ref(self, path: Path) -> dict[str, object]:
        return {"path": _gate._repo_relative(self.root, path),
                "sha256": self._sha(path), "bytes": path.stat().st_size}

    def _make_receipt(self) -> dict[str, object]:
        build_relative = _gate._repo_relative(self.root, self.build)
        ctest_path = str(self.ctest)
        build_command = [str(self.cmake), "--build", str(self.build), "--config", "Release", "--parallel", "4"]
        list_command = [ctest_path, "--test-dir", str(self.build), "-C", "Release", "--show-only=json-v1"]
        test_command = [ctest_path, "--test-dir", str(self.build), "-C", "Release",
                        "--output-on-failure", "--no-tests=error", "--output-junit", str(self.junit)]
        junit_names, totals = _gate._junit_summary(self.junit)
        registered = self._ref(self.test_list_path)
        registered["names"] = list(self.names)
        junit = self._ref(self.junit)
        junit["names"] = junit_names
        junit["totals"] = totals
        return {
            "format": 1,
            "kind": "epocanva-backend-verification",
            "status": "verified",
            "source": self.source.copy(),
            "build_dir": build_relative,
            "configuration": "Release",
            "cache": self._ref(self.cache),
            "registered_tests": registered,
            "junit": junit,
            "commands": {
                "build": {"argv": build_command, "exit_code": 0},
                "list": {"argv": list_command, "exit_code": 0},
                "test": {"argv": test_command, "exit_code": 0},
            },
            "binaries": [{"name": "backend-tests", **self._ref(self.test_executable)}],
        }

    def validate(self):
        return _gate._validate_verification_receipt(
            self.root, self.build, self.junit, self.receipt_path, self.current_identity)


class ReceiptBindingTests(ReceiptFixture):
    def test_current_receipt_binds_exact_source_registration_junit_commands_and_binary(self):
        passed, detail, receipt = self.validate()
        self.assertTrue(passed, detail)
        self.assertIn("exact registered tests", detail)
        self.assertEqual(receipt["source"]["source_sha256"], self.source["source_sha256"])

    def test_dependency_order_may_differ_from_registered_order(self):
        names = list(reversed(self.names))
        self.junit.write_text(
            '<testsuite tests="3" failures="0" errors="0" skipped="0">' +
            ''.join(f'<testcase name="{name}"/>' for name in names) + '</testsuite>',
            encoding='utf-8')
        self.receipt['junit'] = self._ref(self.junit) | {
            'names': names, 'totals': {'tests': 3, 'failures': 0, 'errors': 0, 'skipped': 0}}
        self.receipt_path.write_text(json.dumps(self.receipt), encoding='utf-8')
        passed, detail, _ = self.validate()
        self.assertTrue(passed, detail)

    def test_missing_receipt_is_a_blocked_cli_gate_not_a_historical_junit_pass(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build", prefix="backend-gate-report-") as directory:
            output = Path(directory) / "gate.json"
            result = subprocess.run([
                sys.executable, "-B", str(GATE_PATH),
                "--build-dir", str(ROOT / "build/backend-service"),
                "--models", str(ROOT / "build/no-such-models"),
                "--junit", str(ROOT / "build/backend-service/backend-final.xml"),
                "--output", str(output),
            ], capture_output=True, text=True, encoding="utf-8")
            self.assertEqual(result.returncode, 1)
            report = json.loads(output.read_text(encoding="utf-8"))
            self.assertFalse(report["stable_ready"])
            self.assertEqual(next(row for row in report["checks"] if row["id"] == "verification-receipt")["status"], "blocked")
            self.assertEqual(next(row for row in report["checks"] if row["id"] == "executed-backend-tests")["status"], "blocked")

    def test_stale_source_identity_blocks_even_when_every_file_and_junit_is_unchanged(self):
        self.receipt["source"]["source_sha256"] = "c" * 64
        self.receipt_path.write_text(json.dumps(self.receipt) + "\n", encoding="utf-8")
        passed, detail, _ = self.validate()
        self.assertFalse(passed)
        self.assertIn("source.source_sha256 is stale", detail)

    def test_duplicate_registered_test_names_block_before_junit_name_prefixes_can_help(self):
        duplicate = dict(self.test_list)
        duplicate["tests"] = list(self.command_rows) + [dict(self.command_rows[0])]
        self.test_list_path.write_text(json.dumps(duplicate) + "\n", encoding="utf-8")
        self.receipt["registered_tests"] = self._ref(self.test_list_path) | {"names": self.names + [self.names[0]]}
        self.receipt_path.write_text(json.dumps(self.receipt) + "\n", encoding="utf-8")
        passed, detail, _ = self.validate()
        self.assertFalse(passed)
        self.assertIn("duplicate test names", detail)

    def test_current_registration_change_blocks_old_receipt_exact_set(self):
        changed = dict(self.current_document)
        changed["tests"] = list(self.command_rows)
        changed["tests"][2] = dict(changed["tests"][2], name="prediction.abbreviation.changed")
        _gate._run_current_ctest_list.return_value = (changed, [
            "backend.abi.synthetic", "backend.profiles.synthetic", "prediction.abbreviation.changed"])
        passed, detail, _ = self.validate()
        self.assertFalse(passed)
        self.assertIn("registered CTest names or commands differ", detail)

    def test_modified_binary_blocks_even_if_receipt_names_and_junit_are_valid(self):
        self.test_executable.write_bytes(b"modified after test run")
        passed, detail, _ = self.validate()
        self.assertFalse(passed)
        self.assertIn("binary[0] changed", detail)

    def test_nonzero_recorded_build_list_or_test_exit_blocks(self):
        for stage_name in ("build", "list", "test"):
            with self.subTest(stage=stage_name):
                receipt = json.loads(json.dumps(self.receipt))
                receipt["commands"][stage_name]["exit_code"] = 7
                self.receipt_path.write_text(json.dumps(receipt) + "\n", encoding="utf-8")
                passed, detail, _ = self.validate()
                self.assertFalse(passed)
                self.assertIn("exit code is not zero", detail)

    def test_missing_configured_cmake_blocks_receipt(self):
        self.cmake.unlink()
        passed, detail, _ = self.validate()
        self.assertFalse(passed)
        self.assertIn("configured cmake executable is missing", detail)

    def test_build_command_mismatch_blocks_receipt(self):
        self.receipt["commands"]["build"]["argv"][1] = "--fresh"
        self.receipt_path.write_text(json.dumps(self.receipt) + "\n", encoding="utf-8")
        passed, detail, _ = self.validate()
        self.assertFalse(passed)
        self.assertIn("configured cmake --build", detail)

    def test_malformed_or_misdirected_commands_fail_closed(self):
        cases = [('build', ['--config']), ('list', ['--test-dir']),
                 ('test', ['--output-junit'])]
        for stage, suffix in cases:
            with self.subTest(stage=stage):
                receipt = json.loads(json.dumps(self.receipt))
                receipt['commands'][stage]['argv'] += suffix
                self.receipt_path.write_text(json.dumps(receipt), encoding='utf-8')
                passed, _, _ = self.validate()
                self.assertFalse(passed)
        for option in ('--test-dir', '--output-junit', '-C'):
            with self.subTest(option=option):
                receipt = json.loads(json.dumps(self.receipt))
                argv = receipt['commands']['test']['argv']
                argv[argv.index(option) + 1] = 'Debug' if option == '-C' else str(self.build / 'wrong')
                self.receipt_path.write_text(json.dumps(receipt), encoding='utf-8')
                passed, _, _ = self.validate()
                self.assertFalse(passed)

    def test_junit_skip_failure_dtd_and_name_mismatch_each_block(self):
        base = self.junit.read_text(encoding="utf-8")
        testcase = f'<testcase name="{self.names[0]}" classname="{self.names[0]}"/>'
        cases = {
            "skip": (base.replace('skipped="0"', 'skipped="1"').replace(
                testcase, testcase[:-2] + "><skipped/></testcase>", 1),
                      list(self.names), {"tests": 3, "failures": 0, "errors": 0, "skipped": 1}),
            "failure": (base.replace('failures="0"', 'failures="1"').replace(
                testcase, testcase[:-2] + "><failure/></testcase>", 1),
                        list(self.names), {"tests": 3, "failures": 1, "errors": 0, "skipped": 0}),
            "dtd": ('<!DOCTYPE testsuite [<!ENTITY x "x">]>\n' + base,
                    list(self.names), {"tests": 3, "failures": 0, "errors": 0, "skipped": 0}),
            "name": (base.replace(self.names[1], "old.historical.name", 1),
                     [self.names[0], "old.historical.name", self.names[2]],
                     {"tests": 3, "failures": 0, "errors": 0, "skipped": 0}),
        }
        for label, (content, names, totals) in cases.items():
            with self.subTest(case=label):
                self.junit.write_text(content, encoding="utf-8")
                self.receipt["junit"] = self._ref(self.junit) | {"names": names, "totals": totals}
                self.receipt_path.write_text(json.dumps(self.receipt) + "\n", encoding="utf-8")
                passed, detail, _ = self.validate()
                self.assertFalse(passed)
                self.assertTrue("JUnit" in detail or "registered" in detail)
                self.setUpReceiptFiles()

    def setUpReceiptFiles(self):
        # Recreate only the mutable JUnit and its receipt hash; this helper is
        # explicit so the loop above never silently converts a failure to pass.
        self.junit.write_text(
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<testsuite name="synthetic" tests="3" failures="0" errors="0" skipped="0">'
            + "".join(f'<testcase name="{name}" classname="{name}"/>' for name in self.names)
            + "</testsuite>\n", encoding="utf-8")
        self.receipt["junit"] = self._ref(self.junit) | {"names": list(self.names), "totals": {"tests": 3, "failures": 0, "errors": 0, "skipped": 0}}
        self.receipt_path.write_text(json.dumps(self.receipt) + "\n", encoding="utf-8")

    def test_path_boundary_rejects_absolute_parent_and_non_build_receipt_references(self):
        for value in ("../outside.json", "/absolute/file.json", "C:/outside.json",
                      "build/backend/../outside.json", "build//preset/file.json",
                      "build/preset/./file.json", "build/preset/file:stream.json"):
            with self.subTest(path=value), self.assertRaises(_gate.GateError):
                _gate._relative_build_path(self.root, value, "fixture path")

    def test_receipt_junit_path_must_match_cli_junit_path(self):
        other = self.build / "other.xml"
        other.write_bytes(self.junit.read_bytes())
        self.receipt["junit"]["path"] = _gate._repo_relative(self.root, other)
        self.receipt["junit"]["sha256"] = self._sha(other)
        self.receipt["junit"]["bytes"] = other.stat().st_size
        self.receipt_path.write_text(json.dumps(self.receipt) + "\n", encoding="utf-8")
        passed, detail, _ = self.validate()
        self.assertFalse(passed)
        self.assertIn("--junit", detail)


class BoundaryTests(unittest.TestCase):
    def test_output_outside_build_is_refused_and_no_report_is_created(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build", prefix="backend-gate-output-") as directory:
            outside = Path(directory).parent.parent / (Path(directory).name + '-must-not-be-a-report.json')
            self.assertFalse(outside.exists())
            result = subprocess.run([
                sys.executable, "-B", str(GATE_PATH),
                "--build-dir", str(ROOT / "build/backend-service"),
                "--models", str(ROOT / "build/no-such-models"),
                "--junit", str(ROOT / "build/no.xml"),
                "--output", str(outside),
            ], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(outside.exists())


if __name__ == "__main__":
    unittest.main()
