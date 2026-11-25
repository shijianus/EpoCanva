"""Recheck the controlled editor's raw execution and candidate evidence."""

import argparse
import base64
import hashlib
import json
import math
import re
import shutil
import stat
import subprocess
import sys
import xml.etree.ElementTree as ET
from datetime import datetime
from pathlib import Path


SEEDS = tuple(f"0x5eed00000000000{ordinal}" for ordinal in range(1, 6))
OPERATIONS = ("insert", "delete", "caret-utf16", "selection-utf16", "undo", "invalid-state")
SEED_OPERATIONS = 100_000
SCOPE = "controlled-real-tsf-edit-layer"
ORACLE = "independent-editor-reference-model"
REDUCTION = "replay-and-minimize"
MASK64 = (1 << 64) - 1
MODES = (
    ("stress", 0, ("--report", "measurements.json")),
    ("self-test", 0, ("--self-test", "--report", "negative-control.json")),
    ("replay-mutant", 1, ("--replay", "negative-control.json.replay", "--report", "mutant-replay.json")),
    ("force-fail", 1, ("--force-fail",)),
)
RAW_FILES = {
    "measurements": "measurements.json", "replay_log": "replay-log.json", "resource_samples": "resource-samples.json",
    "negative_control": "negative-control.json", "mutant_replay_report": "mutant-replay.json",
}
CASES = tuple(f"seed-{ordinal}" for ordinal in range(1, 6)) + (
    "negative-control", "replay-minimization", "resource-growth",
)
BINDINGS = ("version", "commit", "source_sha256", "package_manifest_sha256", "language_policy_sha256", "acceptance_profile")
REGISTERED = ("stress", "replay_minimization", "can_fail", "report_unwritable", "evidence_contract")
RESOURCE_POLICY = "tests/editor_reference_model/resource-policy.json"
RESOURCE_METRICS = ("peak_working_set_bytes", "peak_private_bytes", "working_set_growth_bytes", "private_growth_bytes")
SOURCE_KEYS = ("head", "commit", "source_sha256", "repository_sha256", "dirty", "source_files")


class ContractError(ValueError):
    pass


def require(condition, detail):
    if not condition:
        raise ContractError(detail)


def integer(value, minimum=0):
    return type(value) is int and value >= minimum


def matches(value, pattern):
    return type(value) is str and re.fullmatch(pattern, value) is not None


def parse_json(text):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, "duplicate JSON property")
            result[key] = value
        return result

    def finite(raw):
        value = float(raw)
        require(math.isfinite(value), "nonfinite JSON number")
        return value

    return json.loads(text, object_pairs_hook=pairs, parse_constant=finite, parse_float=finite)


def read_json(path):
    return parse_json(path.read_text(encoding="utf-8-sig"))


def same_json(left, right):
    return json.dumps(left, sort_keys=True) == json.dumps(right, sort_keys=True)


def plain_path(path, boundary):
    root_entry = boundary.lstat()
    require(not stat.S_ISLNK(root_entry.st_mode) and
            not getattr(root_entry, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT,
            "evidence boundary is a link or reparse point")
    parts = path.relative_to(boundary).parts
    require(all(part != ".." for part in parts), "evidence path escapes its directory")
    resolved = boundary
    for part in parts:
        resolved /= part
        entry = resolved.lstat()
        require(not stat.S_ISLNK(entry.st_mode) and
                not getattr(entry, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT,
                "evidence path traverses a link or reparse point")
    return resolved


def digest(path):
    with path.open("rb") as input_file:
        checksum = hashlib.sha256()
        for block in iter(lambda: input_file.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def reference(repo, item, expected):
    require(type(item) is dict and set(item) == {"path", "sha256", "bytes"}, "invalid file reference")
    raw = item["path"]
    require(type(raw) is str and all(part and part not in (".", "..") and not part.endswith((".", " "))
            and not re.search(r'[\\:*?"<>|\x00-\x1f]', part) for part in raw.split("/")), "unsafe evidence path")
    path = plain_path(repo / raw, repo)
    require(path == expected and path.is_file(), f"unexpected file reference for {expected.name}")
    require(integer(item["bytes"], 1) and path.stat().st_size == item["bytes"] and
            matches(item["sha256"], "[0-9a-f]{64}") and digest(path) == item["sha256"],
            f"bad bytes or digest for {expected.name}")
    return path


def instant(value):
    require(matches(value, r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d{1,7})?(?:Z|\+00:00)"),
            "invalid UTC timestamp")
    return datetime.fromisoformat(value.replace("Z", "+00:00"))


def seeded_replay(seed, operations):
    state = int(seed, 16)

    def next_operand():
        nonlocal state
        state ^= state >> 12
        state ^= (state << 25) & MASK64
        state ^= state >> 27
        return ((state * 0x2545f4914f6cdd1d) & MASK64) >> 32

    counts = dict.fromkeys(OPERATIONS, 0)
    replay = bytearray()
    for ordinal in range(operations):
        kind = ordinal if ordinal < len(OPERATIONS) else next_operand() % len(OPERATIONS)
        operands = (next_operand(), next_operand(), next_operand())
        counts[OPERATIONS[kind]] += 1
        replay.extend(f"{kind} {operands[0]} {operands[1]} {operands[2]}\n".encode("ascii"))
    return replay, counts


def measurement(path, mode, total, negative_count, result):
    doc = read_json(path)
    require(doc["format"] == "epocanva-editor-reference-measurements-v2" and doc["mode"] == mode and
            doc["oracle"] == ORACLE and doc["scope"] == SCOPE and doc["failure_reduction"] == REDUCTION and
            doc["host_undo"] == "controlled-application-edit-session" and doc["installed_host_acceptance"] is False and
            doc["selection_replacement"] == "unsupported-rejected" and
            doc["composition_observation"] == "session-range-utf16" and
            doc["result"] == result and integer(doc["total_ops"]) and doc["total_ops"] == total and
            integer(doc["malformed_replays_rejected"]) and doc["malformed_replays_rejected"] == negative_count and
            doc["replay_minimization_negative_control"] is (mode == "self-test"), f"wrong {mode} measurements")
    return doc["seeds"]


def verify_row(row, operations, counts, mismatch):
    require(integer(row["operations"]) and row["operations"] == operations and integer(row["mismatches"]) and
            row["mismatches"] == int(bool(mismatch)) and row["mismatch"] == mismatch and
            same_json(row["operation_counts"], counts), "operation inventory or oracle result differs from replay")
    require(all(integer(row[key]) and row[key] == 0
                for key in ("stores_alive_after_shutdown", "store_sinks_alive_after_shutdown")), "TSF cleanup failed")


def verify_resources(rows, measured):
    require(same_json(measured, {"format": "epocanva-editor-stress-resources-v1", "seeds": [
        {"seed": row["seed"], "samples": row["resource_samples"]} for row in rows]}), "resource raw records disagree")
    teardown_counts, peak = [], 0
    schedule = [*range(0, SEED_OPERATIONS + 1, 10_000), SEED_OPERATIONS]
    for row in rows:
        samples = row["resource_samples"]
        require(type(samples) is list and len(samples) == len(schedule), "missing resource samples")
        prior_ms = 0
        for sample, completed in zip(samples, schedule):
            require(integer(sample["completed_operations"]) and sample["completed_operations"] == completed and
                    integer(sample["elapsed_ms"], prior_ms) and integer(sample["handles"], 1) and
                    integer(sample["working_set_bytes"], 1) and integer(sample["private_bytes"], 1) and
                    integer(sample["peak_working_set_bytes"], max(peak, sample["working_set_bytes"])),
                    "invalid or nonmonotonic resource sample")
            prior_ms, peak = sample["elapsed_ms"], sample["peak_working_set_bytes"]
        require(integer(row["duration_ms"], 1) and row["duration_ms"] == prior_ms, "resource interval or duration mismatch")
        require(samples[-1]["handles"] <= samples[0]["handles"], "handles grew across seed teardown")
        teardown_counts.append(samples[-1]["handles"])
    require(max(teardown_counts) <= rows[0]["resource_samples"][0]["handles"], "handles grew across seed runs")


def file_reference(repo, path):
    path = plain_path(path, repo)
    return {"path": path.relative_to(repo).as_posix(), "bytes": path.stat().st_size, "sha256": digest(path)}


def resource_assessment(repo, rows):
    """Apply only the current checked-in policy; report claims are never approvals.

    Owner/record identity is auditable provenance, not a cryptographic proof of
    authority. The clean-source release gate and human approval review remain
    required. Synthetic unit fixtures exercise this parser without granting approval.
    """
    policy_path = plain_path(repo / RESOURCE_POLICY, repo)
    policy = read_json(policy_path)
    require(policy["format"] == "epocanva-editor-resource-policy-v1" and policy["scope"] == SCOPE and
            set(policy["limits"]) == set(RESOURCE_METRICS), "invalid editor resource policy")
    samples = [sample for row in rows for sample in row["resource_samples"]]
    baseline = rows[0]["resource_samples"][0]
    # Retained memory is measured after each seed's TSF/undo teardown. Take the
    # greatest growth above the warmed process baseline; do not hide a middle peak.
    metrics = {
        "peak_working_set_bytes": max(sample["peak_working_set_bytes"] for sample in samples),
        "peak_private_bytes": max(sample["private_bytes"] for sample in samples),
        "working_set_growth_bytes": max(0, *(row["resource_samples"][-1]["working_set_bytes"] -
                                            baseline["working_set_bytes"] for row in rows)),
        "private_growth_bytes": max(0, *(row["resource_samples"][-1]["private_bytes"] -
                                        baseline["private_bytes"] for row in rows)),
    }
    result = {"format": "epocanva-editor-resource-assessment-v1", "policy": file_reference(repo, policy_path),
              "metrics": metrics, "status": "blocked", "blockers": []}
    if policy["decision"] == "pending_product_owner":
        require(policy["approval"] is None and all(value is None for value in policy["limits"].values()),
                "unapproved editor resource policy contains approval or limits")
        result["blockers"].append("STRESS_RESOURCE_APPROVAL_REQUIRED")
        return result
    require(policy["decision"] == "approved", "invalid editor resource decision")
    approval = policy["approval"]
    require(type(approval) is dict and all(type(approval.get(key)) is str and approval[key].strip()
                                        for key in ("owner", "record_id")), "missing editor resource approval record")
    record = approval["record"]
    # The record is reviewed with the policy in source, never taken from a report
    # or generated in a mutable build folder.
    require(type(record) is dict and type(record.get("path")) is str and
            record["path"].startswith("docs/") and record["path"] != RESOURCE_POLICY,
            "resource approval must reference a reviewed source record")
    reference(repo, record, repo / record["path"])
    for key, measured in metrics.items():
        limit = policy["limits"][key]
        require(integer(limit, 1 if key.startswith("peak_") else 0), "invalid editor resource limit")
        if measured > limit:
            result["blockers"].append("STRESS_RESOURCE_LIMIT_" + key.upper())
    if not result["blockers"]:
        result["status"] = "passed"
    return result


def verify_execution(repo, runner, log):
    out_dir = log.parent
    doc = read_json(log)
    require(doc["format"] == "epocanva-editor-stress-execution-v2" and doc["scope"] == SCOPE and
            doc["installed_host_acceptance"] is False and Path(doc["working_directory"]) == out_dir,
            "wrong execution scope or working directory")
    reference(repo, doc["harness"], runner)
    source = doc["source"]
    require(type(source) is dict and set(source) == set(SOURCE_KEYS) and
            matches(source["head"], "[0-9a-f]{40}") and type(source["dirty"]) is bool and
            all(matches(source[key], "[0-9a-f]{64}") for key in ("source_sha256", "repository_sha256")) and
            integer(source["source_files"], 1) and source["commit"] == source["head"] +
            ("+dirty." + source["source_sha256"][:12] if source["dirty"] else ""), "invalid execution source identity")
    identity = doc["runner_identity"]
    require(matches(identity["machine_sha256"], "[0-9a-f]{64}") and
            matches(identity["os_version"], r"\d+(\.\d+){1,3}") and
            identity["kind"] in ("local", "github-actions"), "invalid runner identity")
    if identity["kind"] == "github-actions":
        require(matches(identity["run_id"], "[1-9][0-9]*") and matches(identity["run_attempt"], "[1-9][0-9]*") and
                matches(identity["job"], "[A-Za-z0-9_-]+"), "missing GitHub run identity")
    executions = doc["executions"]
    require(type(executions) is list and len(executions) == len(MODES), "missing native executions")
    require(doc["started_utc"] == executions[0]["started_utc"] and doc["completed_utc"] == executions[-1]["completed_utc"],
            "execution interval differs from native runs")
    previous_end = instant(doc["started_utc"])
    output = {}
    for execution, (mode, code, arguments) in zip(executions, MODES):
        require(execution["mode"] == mode and execution["timed_out"] is False and
                type(execution["exit_code"]) is int and execution["exit_code"] == code and
                type(execution["expected_exit_code"]) is int and execution["expected_exit_code"] == code and
                execution["command"] == [str(runner), *arguments], f"wrong native result or command for {mode}")
        started, ended = instant(execution["started_utc"]), instant(execution["completed_utc"])
        require(previous_end <= started < ended, "native execution intervals overlap or are empty")
        previous_end = ended
        output[mode] = reference(repo, execution["log"], out_dir / f"{mode}.log").read_text(encoding="utf-8").splitlines()
    junit = reference(repo, doc["native_junit"], out_dir / "native.junit.xml")
    root = passing_xml(junit)
    cases = root.findall("testcase")
    require(root.get("name") == "editor-native-execution" and len(cases) == len(executions), "wrong native JUnit inventory")
    for case, execution in zip(cases, executions):
        require(case.get("name") == execution["mode"] and case.get("status") == "run", "native JUnit case differs")
        seconds = float(case.get("time", "nan"))
        elapsed = (instant(execution["completed_utc"]) - instant(execution["started_utc"])).total_seconds()
        require(math.isfinite(seconds) and abs(seconds - elapsed) <= 0.000001, "native JUnit duration differs")
        expected = {"exit_code": str(execution["exit_code"]), "started_utc": execution["started_utc"],
                    "completed_utc": execution["completed_utc"], "executable_sha256": doc["harness"]["sha256"]}
        properties = case.findall("properties/property")
        require(len(properties) == len(expected) and all(
            [item.get("value") for item in properties if item.get("name") == key] == [value]
            for key, value in expected.items()), "native JUnit result differs from execution")
    return doc, output


def verify_raw(repo, runner, log):
    doc, output = verify_execution(repo, runner, log)
    out_dir = log.parent
    paths = {key: reference(repo, doc[key], out_dir / name) for key, name in RAW_FILES.items()}
    rows = measurement(paths["measurements"], "stress", len(SEEDS) * SEED_OPERATIONS, 0, "passed")
    require(type(rows) is list and [row["seed"] for row in rows] == list(SEEDS), "missing or reordered seed")
    replay = read_json(paths["replay_log"])
    require(replay["format"] == "epocanva-editor-stress-replay-v1" and type(replay["seeds"]) is list and
            len(replay["seeds"]) == len(SEEDS), "invalid replay inventory")
    verify_resources(rows, read_json(paths["resource_samples"]))
    stress_execution = doc["executions"][0]
    elapsed_ms = (instant(stress_execution["completed_utc"]) - instant(stress_execution["started_utc"])).total_seconds() * 1000
    require(sum(row["duration_ms"] for row in rows) <= elapsed_ms, "resource durations exceed process execution interval")
    lines = []
    for ordinal, (row, seed, item) in enumerate(zip(rows, SEEDS, replay["seeds"]), 1):
        expected, counts = seeded_replay(seed, SEED_OPERATIONS)
        replay_path = reference(repo, item, out_dir / f"measurements.json.seed-{ordinal}.replay")
        require(replay_path.stat().st_size == len(expected) and replay_path.read_bytes() == expected,
                f"replay differs from fixed seed {seed}")
        verify_row(row, SEED_OPERATIONS, counts, "")
        before, after = row["resource_samples"][0]["handles"], row["resource_samples"][-1]["handles"]
        lines.append(f"seed={seed} operations={SEED_OPERATIONS} mismatch=none ms={row['duration_ms']} handles={before}->{after}")
    require(output["stress"] == [*lines, "RESULT PASS"], "stress output differs from measurements")
    original = reference(repo, replay["original"], out_dir / "negative-control.json.original.replay")
    minimal = reference(repo, replay["minimal"], out_dir / "negative-control.json.replay")
    prefix, _ = seeded_replay(SEEDS[0], 12)
    require(original.read_bytes() == prefix + b"2 0 0 0\n6 0 0 0\n" and minimal.read_bytes() == b"6 0 0 0\n",
            "wrong original or minimized mutation replay")
    one_insert = dict.fromkeys(OPERATIONS, 0)
    one_insert["insert"] = 1
    for key, mode, negatives, result, mismatch in (
            ("negative_control", "self-test", 6, "passed", ""),
            ("mutant_replay_report", "replay", 0, "failed", "document")):
        negative = measurement(paths[key], mode, 1, negatives, result)
        require(type(negative) is list and len(negative) == 1 and negative[0]["seed"] == SEEDS[0], "wrong mutation output")
        verify_row(negative[0], 1, one_insert, mismatch)
    require(output["self-test"] == ["RESULT PASS"] and output["replay-mutant"] == ["RESULT FAIL document"] and
            output["force-fail"] == ["RESULT FAIL omitted-write mutation detected"], "wrong negative process output")
    return doc, rows


def passing_xml(path):
    text = path.read_text(encoding="utf-8-sig")
    require("<!DOCTYPE" not in text.upper() and "<!ENTITY" not in text.upper(), "prohibited JUnit DTD/entity")
    root = ET.fromstring(text)
    require(root.tag == "testsuite" and all(root.get(key, "0") == "0" for key in ("failures", "errors", "skipped", "disabled"))
            and not any(element.tag in ("failure", "error", "skipped") for element in root.iter()), "nonpassing JUnit")
    cases = root.findall("testcase")
    names = [case.get("name") for case in cases]
    require(root.get("tests") == str(len(cases)) and len(cases) > 0 and len(set(names)) == len(names) and
            all(type(name) is str and name for name in names) and
            len(list(root.iter("testcase"))) == len(cases) and
            all(case.get("status", "run") == "run" for case in cases), "inconsistent JUnit count or execution status")
    return root


def verify_receipt(repo, build, report, runner):
    path = reference(repo, report["build_receipt"], build / "product-build-receipt.json")
    require(report["build_receipt"]["sha256"] == report["build_receipt_sha256"], "wrong build receipt digest")
    receipt = read_json(path)
    source = receipt["source"]
    require(integer(receipt["format"]) and receipt["format"] == 2 and receipt["status"] == "verified" and
            receipt["configuration"] == "Release" and receipt["trace"] is False and source["dirty"] is False and
            all(integer(receipt[key]) and receipt[key] == 0 for key in ("build_exit", "ctest_exit")) and
            source["commit"] == source["head"] == report["commit"] and source["source_sha256"] == report["source_sha256"],
            "test receipt does not bind the clean Release build")
    binaries = [item for item in receipt["test_binaries"] if Path(item["path"]) == runner]
    require(len(binaries) == 1 and binaries[0]["name"] == runner.name and
            binaries[0]["sha256"] == report["executable"]["sha256"] and integer(binaries[0]["bytes"], 1) and
            binaries[0]["bytes"] == report["executable"]["bytes"], "receipt did not test this executable")
    folder = plain_path(Path(receipt["evidence"]), build)
    inventory_path = plain_path(folder / "ctest-list.json", build)
    junit_path = plain_path(folder / "ctest.xml", build)
    require(digest(inventory_path) == receipt["ctest_list_sha256"] and digest(junit_path) == receipt["ctest_xml_sha256"],
            "receipt CTest files changed")
    inventory = read_json(inventory_path)["tests"]
    cases = passing_xml(junit_path).findall("testcase")
    require(type(inventory) is list and all(type(test) is dict and type(test.get("name")) is str for test in inventory),
            "invalid CTest inventory")
    registered_names = [test["name"] for test in inventory]
    require(len(set(registered_names)) == len(registered_names) and
            set(registered_names) == {case.get("name") for case in cases}, "CTest registered/executed inventory differs")
    counts = receipt["tests"]
    require(all(integer(counts[key]) for key in ("total", "passed", "failed", "skipped")) and
            counts["total"] == counts["passed"] == len(cases) and counts["failed"] == counts["skipped"] == 0,
            "receipt CTest counts disagree")
    for suffix in REGISTERED:
        name = f"editor.reference_model.{suffix}"
        registered = [test for test in inventory if test["name"] == name]
        executed = [case for case in cases if case.get("name") == name and case.get("status") == "run"]
        require(len(registered) == len(executed) == 1, f"receipt missing editor CTest case {suffix}")
        if suffix in ("stress", "replay_minimization"):
            require(Path(registered[0]["command"][0]) == runner, "editor CTest used a different executable")


def verify_package(repo, build, stage, manifest):
    # Reopen payload bytes, not just the mutable manifest's outer checksum. The
    # release package verifier additionally enforces the fixed payload policy.
    lines = manifest.read_text(encoding="utf-8-sig").splitlines()
    require(lines and lines[0] == "epocanva-package-manifest 2", "unsupported stress package manifest")
    payload = {}
    for line in lines[1:]:
        if not line.startswith("file\t"):
            continue
        columns = line.split("\t")
        require(len(columns) == 4 and matches(columns[3], "[1-9][0-9]*"), "malformed package file row")
        _, name, checksum, byte_count = columns
        require(name.casefold() not in payload and name.casefold() != "package.manifest", "duplicate package payload")
        item = {"path": (stage.relative_to(repo) / name).as_posix(), "sha256": checksum, "bytes": int(byte_count)}
        reference(repo, item, stage / name)
        payload[name.casefold()] = item
    require(payload, "empty package payload")
    # Path redirection and unlisted side files are rejected before traversal.
    observed = set()
    def visit(directory):
        for path in directory.iterdir():
            plain_path(path, stage)
            if path.is_dir():
                visit(path)
            else:
                observed.add(path.relative_to(stage).as_posix().casefold())
    visit(stage)
    require(observed == set(payload) | {"package.manifest"}, "package payload inventory differs")
    binaries = read_json(build / "product-build-receipt.json")["binaries"]
    require(type(binaries) is list and len(binaries) == 3, "missing package binary receipts")
    for name, filename in (("exe", "EpoCanva.exe"), ("tsf", "epocanva_tsf.dll"), ("rime", "rime.dll")):
        records = [item for item in binaries if item["name"] == name]
        require(len(records) == 1 and filename.casefold() in payload and
                matches(records[0]["sha256"], "[0-9a-f]{64}") and integer(records[0]["bytes"], 1) and
                all(records[0][key] == payload[filename.casefold()][key] for key in ("sha256", "bytes")),
                "package binary differs from build receipt")


def verify_formal(repo, build, stage, runner, log, report_path, junit, doc, rows):
    require(report_path.parent == junit.parent == log.parent, "stress report and JUnit are from different runs")
    report = read_json(report_path)
    require(report["format"] == "epocanva-stress-reference-model-v1" and report["result"] == "passed" and
            report["oracle"] == ORACLE and report["failure_reduction"] == REDUCTION and
            report["installed_host_acceptance"] is False and matches(report["commit"], "[0-9a-f]{40}") and
            matches(report["source_sha256"], "[0-9a-f]{64}") and integer(report["total_ops"]) and
            report["total_ops"] == len(SEEDS) * SEED_OPERATIONS and same_json(report["seeds"], rows),
            "formal editor report differs from raw results")
    for key in (*RAW_FILES, "native_junit", "source", "started_utc", "completed_utc", "runner_identity"):
        require(same_json(report[key], doc[key]), f"execution binding differs for {key}")
    require(same_json(report["executable"], doc["harness"]), "execution used a different harness")
    reference(repo, report["execution_log"], log)
    reference(repo, report["junit"], junit)
    verify_receipt(repo, build, report, runner)
    receipt_source = read_json(build / "product-build-receipt.json")["source"]
    require(all(same_json(receipt_source[key], doc["source"][key]) for key in SOURCE_KEYS),
            "execution source differs from build receipt")
    manifest = reference(repo, report["package_manifest"], stage / "package.manifest")
    require(report["package_manifest"]["sha256"] == report["package_manifest_sha256"], "wrong package digest")
    lines = manifest.read_text(encoding="utf-8-sig").splitlines()
    for key in ("commit", "version", "acceptance-profile"):
        observed = [line.partition("=")[2] for line in lines if line.startswith(key + "=")]
        require(observed == [report[key.replace("-", "_")]], f"package manifest differs for {key}")
    verify_package(repo, build, stage, manifest)
    root = passing_xml(junit)
    cases = root.findall("testcase")
    require(root.get("name") == "stress-reference-model" and root.get("tests") == str(len(CASES)) and
            all(root.get(key) == "0" for key in ("failures", "errors", "skipped")) and
            [case.get("name") for case in cases] == ["stress-reference-model." + case for case in CASES],
            "formal JUnit differs from required case inventory")
    properties = root.findall("properties/property")
    for key in BINDINGS:
        require([item.get("value") for item in properties if item.get("name") == f"epocanva.{key}"] == [report[key]],
                f"JUnit identity differs for {key}")
    assessment = read_json(reference(repo, report["resource_assessment"], log.parent / "resource-assessment.json"))
    current_assessment = resource_assessment(repo, rows)
    require(same_json(assessment, current_assessment), "resource assessment differs from current policy or measurements")
    require(current_assessment["status"] == "passed", ",".join(current_assessment["blockers"]))


def verify_current_candidate(repo, build, stage, report_path):
    """Reopen current source/engine/build identity through the shared contract.

    The standalone consumer must not bless historical evidence solely because
    its own report and receipt agree. Only fixed repository code is executed;
    no command from an evidence document is ever run.
    """
    shell = shutil.which("powershell.exe") or shutil.which("pwsh")
    require(shell is not None, "PowerShell is required to revalidate current candidate identity")
    def literal(path):
        return "'" + str(path).replace("'", "''") + "'"
    helper = plain_path(repo / "tools/product/acceptance_evidence.ps1", repo)
    script = ("$ErrorActionPreference = 'Stop'\n"
              "[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)\n. " + literal(helper) + "\n"
              "$identity = Get-AcceptanceIdentity -RepoRoot " + literal(repo) + " -BuildDir " + literal(build) +
              " -StageRoot " + literal(stage) + "\n$identity | ConvertTo-Json -Depth 20\n")
    encoded = base64.b64encode(script.encode("utf-16-le")).decode("ascii")
    result = subprocess.run([shell, "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded], cwd=repo,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120, check=False)
    require(result.returncode == 0, f"current candidate identity rejected (PowerShell exit {result.returncode})")
    current = parse_json(result.stdout.decode("utf-8-sig"))
    report = read_json(report_path)
    require(current["receipt"]["source"]["dirty"] is False and
            all(current[key] == report[key] for key in (*BINDINGS, "build_receipt_sha256")) and
            same_json(current["receipt"]["source"], report["source"]), "current candidate differs from stress execution")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("repo", "build-dir", "runner", "log"):
        parser.add_argument(f"--{option}", type=Path, required=True)
    for option in ("stage-root", "report", "junit"):
        parser.add_argument(f"--{option}", type=Path)
    parser.add_argument("--development", action="store_true")
    parser.add_argument("--resource-assessment", type=Path,
                        help="Development-only output of measured resources and the current policy decision")
    args = parser.parse_args()
    formal_paths = (args.stage_root, args.report, args.junit)
    if (args.development and any(formal_paths)) or (not args.development and not all(formal_paths)):
        parser.error("formal mode requires --stage-root, --report and --junit")
    if args.resource_assessment and not args.development:
        parser.error("--resource-assessment is a development output, not an approval input")
    try:
        repo = args.repo.resolve(strict=True)
        build = plain_path(args.build_dir.absolute(), repo)
        require(build.parent == repo / "build", "build directory must be under build/<preset>")
        runner = plain_path(args.runner.absolute(), build)
        log = plain_path(args.log.absolute(), build)
        doc, rows = verify_raw(repo, runner, log)
        if args.resource_assessment:
            assessment_path = args.resource_assessment.absolute()
            require(assessment_path == log.parent / "resource-assessment.json" and not assessment_path.exists(),
                    "resource assessment must be a new file in this execution directory")
            plain_path(assessment_path.parent, build)
            with assessment_path.open("x", encoding="utf-8") as output:
                json.dump(resource_assessment(repo, rows), output, indent=2)
                output.write("\n")
        if not args.development:
            stage, report, junit = (plain_path(path.absolute(), build) for path in formal_paths)
            verify_current_candidate(repo, build, stage, report)
            verify_formal(repo, build, stage, runner, log, report, junit, doc, rows)
    except (ValueError, KeyError, TypeError, AttributeError, OSError, ET.ParseError, subprocess.SubprocessError) as error:
        print(f"EDITOR_STRESS_BLOCKED {error}", file=sys.stderr)
        return 1
    print("EDITOR_STRESS_DEVELOPMENT_VERIFIED" if args.development else "EDITOR_STRESS_VERIFIED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
