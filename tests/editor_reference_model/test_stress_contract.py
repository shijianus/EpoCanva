"""Mutation tests for the editor evidence reader, using synthetic fixtures."""

import hashlib
import json
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/product"))
import verify_stress_reference_model as verifier


class StressContractTests(unittest.TestCase):
    def setUp(self):
        patcher = patch.object(verifier, "SEED_OPERATIONS", 10_000)
        patcher.start()
        self.addCleanup(patcher.stop)
        temporary = tempfile.TemporaryDirectory(prefix="editor-contract-", dir=Path.cwd())
        self.addCleanup(temporary.cleanup)
        self.repo = Path(temporary.name)
        self.build = self.repo / "build" / "preset"
        self.out = self.build / "stress-reference-model" / "unit-test"
        self.out.mkdir(parents=True)
        self.runner = self.build / "epocanva_editor_reference_tests.exe"
        self.runner.write_bytes(b"synthetic receipt executable")
        self.paths = {name: self.out / filename for name, filename in verifier.RAW_FILES.items()}
        self.during = "2026-01-01T00:00:0{}+00:00"
        self.ledger = {
            "format": "epocanva-editor-stress-execution-v2", "scope": verifier.SCOPE,
            "runner_identity": {"kind": "local", "machine_sha256": "b" * 64, "os_version": "10.0.0"},
            "installed_host_acceptance": False, "working_directory": str(self.out),
            "harness": self.ref(self.runner), "started_utc": self.during.format(0), "completed_utc": self.during.format(4),
            "source": {"commit": "a" * 40, "head": "a" * 40, "dirty": False, "source_sha256": "c" * 64,
                       "repository_sha256": "e" * 64, "source_files": 42},
        }
        self.log = self.out / "commands.json"
        self.replays = {"format": "epocanva-editor-stress-replay-v1", "seeds": []}
        self.rows = []
        lines = []
        for ordinal, seed in enumerate(verifier.SEEDS, 1):
            replay = self.out / f"measurements.json.seed-{ordinal}.replay"
            payload, counts = verifier.seeded_replay(seed, verifier.SEED_OPERATIONS)
            replay.write_bytes(payload)
            self.replays["seeds"].append(self.ref(replay))
            row = self.row(seed, 10_000, counts)
            row["resource_samples"] = [
                {"completed_operations": operations, "elapsed_ms": elapsed, "handles": handles,
                 "working_set_bytes": 5000, "private_bytes": 4000, "peak_working_set_bytes": 5100}
                for operations, elapsed, handles in ((0, 0, 30), (10_000, 50, 45), (10_000, 51, 30))
            ]
            self.rows.append(row)
            lines.append(f"seed={seed} operations=10000 mismatch=none ms=51 handles=30->30\n")
        self.save_measurements()
        prefix, _ = verifier.seeded_replay(verifier.SEEDS[0], 12)
        original = self.out / "negative-control.json.original.replay"
        original.write_bytes(prefix + b"2 0 0 0\n6 0 0 0\n")
        minimal = self.out / "negative-control.json.replay"
        minimal.write_bytes(b"6 0 0 0\n")
        self.replays.update(original=self.ref(original), minimal=self.ref(minimal))
        self.ledger["replay_log"] = self.write(self.paths["replay_log"], self.replays)
        counts = dict.fromkeys(verifier.OPERATIONS, 0)
        counts["insert"] = 1
        for key, mode, mismatch in (("negative_control", "self-test", ""), ("mutant_replay_report", "replay", "document")):
            payload = self.measure(mode, [self.row(verifier.SEEDS[0], 1, counts, mismatch)], 1, bool(mismatch))
            self.ledger[key] = self.write(self.paths[key], payload)
        self.ledger["executions"] = []
        outputs = ("".join(lines) + "RESULT PASS\n", "RESULT PASS\n", "RESULT FAIL document\n",
                   "RESULT FAIL omitted-write mutation detected\n")
        for ordinal, ((mode, code, arguments), output) in enumerate(zip(verifier.MODES, outputs)):
            log = self.out / f"{mode}.log"
            log.write_text(output, encoding="utf-8")
            self.ledger["executions"].append({
                "mode": mode, "exit_code": code, "timed_out": False, "command": [str(self.runner), *arguments],
                "expected_exit_code": code,
                "started_utc": self.during.format(ordinal), "completed_utc": self.during.format(ordinal + 1),
                "log": self.ref(log),
            })
        self.native_junit = self.out / "native.junit.xml"
        native = ET.Element("testsuite", name="editor-native-execution", tests="4", failures="0", errors="0", skipped="0")
        for execution in self.ledger["executions"]:
            case = ET.SubElement(native, "testcase", name=execution["mode"], status="run", time="1")
            properties = ET.SubElement(case, "properties")
            for key, value in {"exit_code": str(execution["exit_code"]), "started_utc": execution["started_utc"],
                               "completed_utc": execution["completed_utc"], "executable_sha256": self.ref(self.runner)["sha256"]}.items():
                ET.SubElement(properties, "property", name=key, value=value)
        ET.ElementTree(native).write(self.native_junit, encoding="utf-8")
        self.ledger["native_junit"] = self.ref(self.native_junit)
        self.policy_path = self.repo / verifier.RESOURCE_POLICY
        self.policy_path.parent.mkdir(parents=True)
        self.policy = {"format": "epocanva-editor-resource-policy-v1", "scope": verifier.SCOPE,
                       "decision": "pending_product_owner", "approval": None,
                       "limits": dict.fromkeys(verifier.RESOURCE_METRICS)}
        self.write(self.policy_path, self.policy)
        self.write(self.log, self.ledger)

    def ref(self, path):
        data = path.read_bytes()
        return {"path": path.relative_to(self.repo).as_posix(), "bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest()}

    def write(self, path, data):
        path.write_text(json.dumps(data), encoding="utf-8")
        return self.ref(path)

    @staticmethod
    def row(seed, count, operations, mismatch=""):
        return {"seed": seed, "operations": count, "mismatches": int(bool(mismatch)), "mismatch": mismatch,
                "operation_counts": operations, "duration_ms": 51,
                "stores_alive_after_shutdown": 0, "store_sinks_alive_after_shutdown": 0}

    @staticmethod
    def measure(mode, rows, total, failed=False):
        return {"format": "epocanva-editor-reference-measurements-v2", "mode": mode, "oracle": verifier.ORACLE,
                "scope": verifier.SCOPE, "failure_reduction": verifier.REDUCTION,
                "host_undo": "controlled-application-edit-session", "installed_host_acceptance": False,
                "selection_replacement": "unsupported-rejected", "composition_observation": "session-range-utf16",
                "result": "failed" if failed else "passed", "total_ops": total, "seeds": rows,
                "malformed_replays_rejected": 6 if mode == "self-test" else 0,
                "replay_minimization_negative_control": mode == "self-test"}

    def save_measurements(self):
        self.ledger["measurements"] = self.write(self.paths["measurements"], self.measure("stress", self.rows, 50_000))
        self.ledger["resource_samples"] = self.write(self.paths["resource_samples"], {
            "format": "epocanva-editor-stress-resources-v1",
            "seeds": [{"seed": row["seed"], "samples": row["resource_samples"]} for row in self.rows],
        })

    def raw(self):
        self.write(self.log, self.ledger)
        return verifier.verify_raw(self.repo, self.runner, self.log)

    def formal_fixture(self):
        self.stage = self.build / "stage"
        self.stage.mkdir()
        self.manifest = self.stage / "package.manifest"
        manifest_lines = ["epocanva-package-manifest 2", "version=test-only", "commit=" + "a" * 40,
                          "acceptance-profile=internal-unsigned", "distribution-policy=internal-only", "path-exception=none"]
        binary_records = []
        for name, filename in (("exe", "EpoCanva.exe"), ("tsf", "epocanva_tsf.dll"), ("rime", "rime.dll")):
            path = self.stage / filename
            path.write_bytes(b"SYNTHETIC PACKAGE FIXTURE ONLY " + filename.encode("ascii"))
            item = self.ref(path)
            manifest_lines.append(f"file\t{filename}\t{item['sha256']}\t{item['bytes']}")
            binary_records.append({**item, "name": name})
        self.manifest.write_text("\n".join(manifest_lines) + "\n", encoding="utf-8")
        self.receipt_path = self.build / "product-build-receipt.json"
        self.ctest_junit = self.build / "ctest.xml"
        ctest = ET.Element("testsuite", tests=str(len(verifier.REGISTERED)), failures="0", skipped="0")
        registered = []
        for case in verifier.REGISTERED:
            name = f"editor.reference_model.{case}"
            ET.SubElement(ctest, "testcase", name=name, status="run")
            registered.append({"name": name, "command": [str(self.runner)]})
        ET.ElementTree(ctest).write(self.ctest_junit, encoding="utf-8")
        inv = self.write(self.build / "ctest-list.json", {"tests": registered})
        self.receipt = {"format": 2, "status": "verified", "configuration": "Release", "trace": False,
                        "build_exit": 0, "ctest_exit": 0, "evidence": str(self.build),
                        "source": self.ledger["source"].copy(),
                        "tests": {"total": len(verifier.REGISTERED), "passed": len(verifier.REGISTERED), "failed": 0, "skipped": 0},
                        "binaries": binary_records,
                        "ctest_list_sha256": inv["sha256"], "ctest_xml_sha256": self.ref(self.ctest_junit)["sha256"],
                        "test_binaries": [{**self.ref(self.runner), "name": self.runner.name, "path": str(self.runner)}]}
        self.report_path = self.out / "stress-reference-model.json"
        self.junit = self.out / "stress-reference-model.junit.xml"
        self.report = {"format": "epocanva-stress-reference-model-v1", "result": "passed",
                       "oracle": verifier.ORACLE, "failure_reduction": verifier.REDUCTION,
                       "installed_host_acceptance": False, "commit": "a" * 40, "source_sha256": "c" * 64,
                       "language_policy_sha256": "d" * 64, "version": "test-only", "acceptance_profile": "internal-unsigned",
                       "package_manifest": self.ref(self.manifest), "package_manifest_sha256": self.ref(self.manifest)["sha256"],
                       "total_ops": 50_000, "seeds": self.rows}
        self.approve_synthetic_policy()

    def approve_synthetic_policy(self):
        # Test-only source record: not a real owner record or product approval.
        record = self.repo / "docs" / "synthetic-policy.txt"
        record.parent.mkdir(exist_ok=True)
        record.write_text("SYNTHETIC TEST FIXTURE ONLY", encoding="utf-8")
        self.policy.update(decision="approved", approval={"owner": "synthetic test", "record_id": "unit-only",
                                                        "record": self.ref(record)},
                           limits=dict.fromkeys(verifier.RESOURCE_METRICS, 100_000))
        self.write(self.policy_path, self.policy)

    def formal(self, mutate=None):
        ledger, rows = self.raw()
        self.report.update({key: ledger[key] for key in (*verifier.RAW_FILES, "source", "native_junit", "started_utc", "completed_utc", "runner_identity")})
        self.report["executable"] = self.ledger["harness"]
        self.report["execution_log"] = self.ref(self.log)
        self.report["build_receipt"] = self.write(self.receipt_path, self.receipt)
        self.report["build_receipt_sha256"] = self.report["build_receipt"]["sha256"]
        root = ET.Element("testsuite", name="stress-reference-model", tests="8", failures="0", errors="0", skipped="0")
        properties = ET.SubElement(root, "properties")
        for key in verifier.BINDINGS:
            ET.SubElement(properties, "property", name=f"epocanva.{key}", value=self.report[key])
        for case in verifier.CASES:
            ET.SubElement(root, "testcase", name=f"stress-reference-model.{case}")
        ET.ElementTree(root).write(self.junit, encoding="utf-8")
        self.report["junit"] = self.ref(self.junit)
        self.report["resource_assessment"] = self.write(self.out / "resource-assessment.json",
                                                       verifier.resource_assessment(self.repo, rows))
        if mutate:
            mutate()
        self.write(self.report_path, self.report)
        verifier.verify_formal(self.repo, self.build, self.stage, self.runner, self.log, self.report_path,
                               self.junit, ledger, rows)

    def test_valid_synthetic_structural_chain(self):
        self.formal_fixture()
        self.formal()

    def test_current_identity_rejects_missing_product_receipt(self):
        # Exercise the actual read-only identity helper, without a fake receipt,
        # shell mock, approval or installation. A structural fixture is not a
        # live product build and must never acquire EDITOR_STRESS_VERIFIED.
        repo = Path(__file__).resolve().parents[2]
        with self.assertRaisesRegex(verifier.ContractError, "current candidate identity rejected"):
            verifier.verify_current_candidate(repo, self.build, self.build / "stage", self.out / "report.json")

    def test_missing_seed_and_shortened_operation_count(self):
        original = self.rows.pop()
        self.save_measurements()
        with self.assertRaisesRegex(verifier.ContractError, "missing or reordered seed"):
            self.raw()
        self.rows.append(original)
        self.rows[0]["operations"] -= 1
        self.save_measurements()
        with self.assertRaisesRegex(verifier.ContractError, "operation inventory"):
            self.raw()

    def test_rehashed_operand_despite_identical_operation_categories(self):
        replay = self.out / "measurements.json.seed-1.replay"
        first, remaining = replay.read_bytes().split(b"\n", 1)
        replay.write_bytes(first.rsplit(b" ", 1)[0] + b" 0\n" + remaining)
        self.replays["seeds"][0] = self.ref(replay)
        self.ledger["replay_log"] = self.write(self.paths["replay_log"], self.replays)
        with self.assertRaisesRegex(verifier.ContractError, "replay differs from fixed seed"):
            self.raw()

    def test_removed_replay(self):
        (self.out / "measurements.json.seed-1.replay").unlink()
        with self.assertRaises(FileNotFoundError):
            self.raw()

    def test_rehashed_resource_growth(self):
        self.rows[0]["resource_samples"][-1]["handles"] += 1
        self.save_measurements()
        with self.assertRaisesRegex(verifier.ContractError, "handles grew"):
            self.raw()

    def test_rehashed_resource_samples_hide_cleanup_failure(self):
        self.rows[0]["store_sinks_alive_after_shutdown"] = 1
        self.save_measurements()
        with self.assertRaisesRegex(verifier.ContractError, "TSF cleanup failed"):
            self.raw()

    def test_rehashed_forged_negative_log(self):
        log = self.out / "replay-mutant.log"
        log.write_text("RESULT PASS\n", encoding="utf-8")
        self.ledger["executions"][2]["log"] = self.ref(log)
        with self.assertRaisesRegex(verifier.ContractError, "wrong negative process output"):
            self.raw()

    def test_negative_exit_type_and_timeout(self):
        row = self.ledger["executions"][2]
        row["exit_code"] = True
        with self.assertRaisesRegex(verifier.ContractError, "wrong native result"):
            self.raw()
        row["timed_out"], row["exit_code"] = True, 1
        with self.assertRaisesRegex(verifier.ContractError, "wrong native result"):
            self.raw()

    def test_candidate_sha_mismatch(self):
        self.formal_fixture()
        self.report["commit"] = "f" * 40
        with self.assertRaisesRegex(verifier.ContractError, "clean Release build"):
            self.formal()

    def test_rehashed_binary_replacement(self):
        self.formal_fixture()
        self.runner.write_bytes(b"synthetic replacement")
        self.ledger["harness"] = self.ref(self.runner)
        tree = ET.parse(self.native_junit)
        for item in tree.findall("testcase/properties/property[@name='executable_sha256']"):
            item.set("value", self.ledger["harness"]["sha256"])
        tree.write(self.native_junit, encoding="utf-8")
        self.ledger["native_junit"] = self.ref(self.native_junit)
        with self.assertRaisesRegex(verifier.ContractError, "receipt did not test this executable"):
            self.formal()

    def test_rehashed_skipped_ctest(self):
        self.formal_fixture()
        tree = ET.parse(self.ctest_junit)
        ET.SubElement(tree.find("testcase"), "skipped")
        tree.write(self.ctest_junit, encoding="utf-8")
        self.receipt["ctest_xml_sha256"] = self.ref(self.ctest_junit)["sha256"]
        with self.assertRaisesRegex(verifier.ContractError, "nonpassing JUnit"):
            self.formal()

    def test_rehashed_invalid_json_and_traversal(self):
        path = self.paths["measurements"]
        for invalid in ('{"version":0,"version":0}', '{"number":1e10000}', '{"number":NaN}'):
            path.write_text(invalid, encoding="utf-8")
            self.ledger["measurements"] = self.ref(path)
            with self.assertRaises(verifier.ContractError):
                self.raw()
        self.ledger["measurements"]["path"] = self.ledger["measurements"]["path"].replace("unit-test/", "unit-test/../unit-test/")
        with self.assertRaisesRegex(verifier.ContractError, "unsafe evidence path"):
            self.raw()

    def test_missing_native_exit(self):
        del self.ledger["executions"][0]["exit_code"]
        with self.assertRaises(KeyError):
            self.raw()

    def test_failed_stress_execution(self):
        self.ledger["executions"][0]["exit_code"] = 1
        with self.assertRaisesRegex(verifier.ContractError, "wrong native result"):
            self.raw()

    def test_rehashed_native_junit_exit(self):
        tree = ET.parse(self.native_junit)
        tree.find("testcase/properties/property[@name='exit_code']").set("value", "1")
        tree.write(self.native_junit, encoding="utf-8")
        self.ledger["native_junit"] = self.ref(self.native_junit)
        with self.assertRaisesRegex(verifier.ContractError, "native JUnit result differs"):
            self.raw()

    def test_resource_time_exceeds_native_process(self):
        for sample in self.rows[0]["resource_samples"]:
            sample["elapsed_ms"] *= 1000
        self.rows[0]["duration_ms"] *= 1000
        self.save_measurements()
        with self.assertRaisesRegex(verifier.ContractError, "durations exceed process"):
            self.raw()

    def test_pending_resource_approval_is_blocked(self):
        observed = verifier.resource_assessment(self.repo, self.rows)
        self.assertEqual(observed["status"], "blocked")
        self.assertEqual(observed["blockers"], ["STRESS_RESOURCE_APPROVAL_REQUIRED"])

    def test_rehashed_resource_limit_violation(self):
        self.formal_fixture()
        self.policy["limits"]["peak_private_bytes"] = 3999
        self.write(self.policy_path, self.policy)
        with self.assertRaisesRegex(verifier.ContractError, "STRESS_RESOURCE_LIMIT_PEAK_PRIVATE_BYTES"):
            self.formal()

    def test_memory_growth_in_middle_seed_is_not_hidden(self):
        self.approve_synthetic_policy()
        self.policy["limits"]["private_growth_bytes"] = 100
        self.write(self.policy_path, self.policy)
        self.rows[2]["resource_samples"][-1]["private_bytes"] += 101
        observed = verifier.resource_assessment(self.repo, self.rows)
        self.assertEqual(observed["metrics"]["private_growth_bytes"], 101)
        self.assertIn("STRESS_RESOURCE_LIMIT_PRIVATE_GROWTH_BYTES", observed["blockers"])

    def test_boolean_and_negative_resource_limits(self):
        self.approve_synthetic_policy()
        for value in (True, -1, "100", 100.0, None):
            self.policy["limits"]["private_growth_bytes"] = value
            self.write(self.policy_path, self.policy)
            with self.assertRaisesRegex(verifier.ContractError, "invalid editor resource limit"):
                verifier.resource_assessment(self.repo, self.rows)

    def test_forged_resource_assessment(self):
        self.formal_fixture()
        def tamper():
            assessment = verifier.resource_assessment(self.repo, self.rows)
            assessment["metrics"]["peak_private_bytes"] = 1
            self.report["resource_assessment"] = self.write(self.out / "resource-assessment.json", assessment)
        with self.assertRaisesRegex(verifier.ContractError, "resource assessment differs"):
            self.formal(tamper)

    def test_removed_approval_record(self):
        self.approve_synthetic_policy()
        (self.repo / self.policy["approval"]["record"]["path"]).unlink()
        with self.assertRaises(FileNotFoundError):
            verifier.resource_assessment(self.repo, self.rows)

    def test_execution_source_differs_from_receipt(self):
        self.formal_fixture()
        self.ledger["source"]["repository_sha256"] = "f" * 64
        with self.assertRaisesRegex(verifier.ContractError, "execution source differs"):
            self.formal()

    def test_receipt_missing_registered_case(self):
        self.formal_fixture()
        inv = self.build / "ctest-list.json"
        document = json.loads(inv.read_text())
        document["tests"].append({"name": "product.required.additional", "command": [str(self.runner)]})
        self.receipt["ctest_list_sha256"] = self.write(inv, document)["sha256"]
        with self.assertRaisesRegex(verifier.ContractError, "registered/executed inventory differs"):
            self.formal()

    def test_receipt_summary_count_mismatch(self):
        self.formal_fixture()
        self.receipt["tests"]["passed"] -= 1
        with self.assertRaisesRegex(verifier.ContractError, "receipt CTest counts disagree"):
            self.formal()

    def test_rehashed_package_dll_replacement(self):
        self.formal_fixture()
        path = self.stage / "epocanva_tsf.dll"
        old = self.ref(path)
        path.write_bytes(b"REPLACED SYNTHETIC DLL")
        replacement = self.ref(path)
        text = self.manifest.read_text().replace(f"{old['sha256']}\t{old['bytes']}",
                                                 f"{replacement['sha256']}\t{replacement['bytes']}")
        self.manifest.write_text(text, encoding="utf-8")
        self.report["package_manifest"] = self.ref(self.manifest)
        self.report["package_manifest_sha256"] = self.ref(self.manifest)["sha256"]
        with self.assertRaisesRegex(verifier.ContractError, "package binary differs from build receipt"):
            self.formal()

    def test_unlisted_package_payload(self):
        self.formal_fixture()
        (self.stage / "unlisted.dll").write_bytes(b"SYNTHETIC EXTRA PAYLOAD")
        with self.assertRaisesRegex(verifier.ContractError, "package payload inventory differs"):
            self.formal()

    def test_rehashed_ctest_failure(self):
        self.formal_fixture()
        tree = ET.parse(self.ctest_junit)
        ET.SubElement(tree.find("testcase"), "failure")
        tree.write(self.ctest_junit, encoding="utf-8")
        self.receipt["ctest_xml_sha256"] = self.ref(self.ctest_junit)["sha256"]
        with self.assertRaisesRegex(verifier.ContractError, "nonpassing JUnit"):
            self.formal()

    def test_stale_raw_schema(self):
        self.ledger["format"] = "epocanva-editor-stress-execution-v1"
        with self.assertRaisesRegex(verifier.ContractError, "wrong execution scope"):
            self.raw()

    def test_formal_consumer_rechecks_pending_policy(self):
        self.formal_fixture()
        self.policy.update(decision="pending_product_owner", approval=None, limits=dict.fromkeys(verifier.RESOURCE_METRICS))
        self.write(self.policy_path, self.policy)
        with self.assertRaisesRegex(verifier.ContractError, "STRESS_RESOURCE_APPROVAL_REQUIRED"):
            self.formal()

    def test_rehashed_junit_summary_count(self):
        self.formal_fixture()
        tree = ET.parse(self.ctest_junit)
        tree.getroot().set("tests", "999")
        tree.write(self.ctest_junit, encoding="utf-8")
        self.receipt["ctest_xml_sha256"] = self.ref(self.ctest_junit)["sha256"]
        with self.assertRaisesRegex(verifier.ContractError, "inconsistent JUnit count"):
            self.formal()

    def test_in_progress_receipt_and_absent_exit(self):
        self.formal_fixture()
        self.receipt["status"] = "in_progress"
        with self.assertRaisesRegex(verifier.ContractError, "clean Release build"):
            self.formal()
        self.receipt["status"] = "verified"
        del self.receipt["ctest_exit"]
        with self.assertRaises(KeyError):
            self.formal()

    def test_reader_io_failure_is_not_success(self):
        with patch.object(Path, "read_text", side_effect=OSError("simulated read failure")):
            with self.assertRaises(OSError):
                verifier.read_json(self.log)

    def test_hash_read_failure_after_partial_bytes(self):
        with patch.object(Path, "open") as opened:
            opened.return_value.__enter__.return_value.read.side_effect = [b"partial bytes", OSError("mid-read failure")]
            with self.assertRaisesRegex(OSError, "mid-read failure"):
                verifier.digest(self.runner)

    def test_locked_evidence_file(self):
        # This suite is registered only in the Windows TSF target. A real
        # exclusive Win32 handle tests sharing violations, not an ACL guess.
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                      wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
        kernel.CloseHandle.restype = wintypes.BOOL
        handle = kernel.CreateFileW(str(self.log), 0x80000000, 0, None, 3, 0x80, None)
        self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
        try:
            with self.assertRaises(PermissionError):
                verifier.verify_raw(self.repo, self.runner, self.log)
        finally:
            self.assertTrue(kernel.CloseHandle(handle))

    def test_reparse_point_rejected(self):
        original = Path.lstat
        def reparse(path, *args, **kwargs):
            result = original(path, *args, **kwargs)
            if path == self.log:
                from types import SimpleNamespace
                return SimpleNamespace(st_mode=result.st_mode, st_file_attributes=verifier.stat.FILE_ATTRIBUTE_REPARSE_POINT)
            return result
        with patch.object(Path, "lstat", reparse):
            with self.assertRaisesRegex(verifier.ContractError, "link or reparse point"):
                verifier.plain_path(self.log, self.repo)


if __name__ == "__main__":
    unittest.main()
