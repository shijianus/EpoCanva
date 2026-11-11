#!/usr/bin/env python3
"""Read-only backend release gate.

The gate writes only its requested report under build/<preset>. A backend quality
report is never accepted as evidence of a current test run by itself: the caller
must supply an actual verification receipt binding the current source identity,
CTest registration, JUnit output, commands, exit codes and binaries.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from typing import Any, Iterable

LANGUAGES = ("en", "zh", "fr", "ja", "ko")
REQUIRED_TEST_PREFIXES = ("backend.abi.", "backend.profiles.", "prediction.abbreviation.")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


class GateError(ValueError):
    """A receipt or current-run binding is not admissible."""


def digest(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def file_record(path: pathlib.Path) -> dict[str, Any]:
    return {"path": str(path), "sha256": digest(path), "bytes": path.stat().st_size}


def _normal_path(value: str) -> str:
    return value.replace("\\", "/")


def _relative_build_path(root: pathlib.Path, value: str, field: str) -> pathlib.Path:
    if not isinstance(value, str) or not value or "\x00" in value:
        raise GateError(f"{field} is not a path string")
    if "\\" in value or value.startswith("/") or re.match(r"^[A-Za-z]:", value):
        raise GateError(f"{field} must be a relative repository path")
    raw_parts = value.split("/")
    if any(part in ("", ".", "..") or ":" in part for part in raw_parts):
        raise GateError(f"{field} has an unsafe path")
    path = pathlib.PurePosixPath(value)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        raise GateError(f"{field} has an unsafe path")
    if len(path.parts) < 2 or path.parts[0] != "build":
        raise GateError(f"{field} must remain under build/")
    current = root
    for part in path.parts:
        current = current / part
        if current.is_symlink():
            raise GateError(f"{field} traverses a link")
    resolved = current.resolve(strict=False)
    build_root = (root / "build").resolve()
    if not resolved.is_relative_to(build_root):
        raise GateError(f"{field} escapes build/")
    return current


def _repo_relative(root: pathlib.Path, value: pathlib.Path) -> str:
    return _normal_path(str(value.resolve().relative_to(root.resolve())))


def _validate_hash(value: Any, field: str) -> str:
    if not isinstance(value, str) or not SHA256_RE.fullmatch(value):
        raise GateError(f"{field} is not a lowercase SHA-256")
    return value


def _read_file_ref(root: pathlib.Path, value: Any, field: str) -> tuple[pathlib.Path, str, int]:
    if not isinstance(value, dict):
        raise GateError(f"{field} is not a file reference")
    relative = value.get("path")
    path = _relative_build_path(root, relative, field + ".path")
    expected_hash = _validate_hash(value.get("sha256"), field + ".sha256")
    expected_bytes = value.get("bytes")
    if isinstance(expected_bytes, bool) or not isinstance(expected_bytes, int) or expected_bytes <= 0:
        raise GateError(f"{field}.bytes is not a positive integer")
    if path.is_symlink() or not path.is_file():
        raise GateError(f"{field} is missing or is a link")
    actual_bytes = path.stat().st_size
    actual_hash = digest(path)
    if actual_bytes != expected_bytes or actual_hash != expected_hash:
        raise GateError(f"{field} changed after verification")
    return path, actual_hash, actual_bytes


def _json_file(path: pathlib.Path, field: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise GateError(f"{field} is not valid JSON: {type(error).__name__}") from error
    if not isinstance(value, dict):
        raise GateError(f"{field} is not a JSON object")
    return value


def _test_specs(document: dict[str, Any], field: str) -> tuple[list[str], list[tuple[str, tuple[str, ...], str]]]:
    tests = document.get("tests")
    if not isinstance(tests, list) or not tests:
        raise GateError(f"{field} has no registered tests")
    names: list[str] = []
    specs: list[tuple[str, tuple[str, ...], str]] = []
    for ordinal, row in enumerate(tests):
        if not isinstance(row, dict) or not isinstance(row.get("name"), str) or not row["name"]:
            raise GateError(f"{field}.tests[{ordinal}] has no name")
        name = row["name"]
        command = row.get("command")
        if not isinstance(command, list) or not command or not all(isinstance(item, str) for item in command):
            raise GateError(f"{field}.tests[{ordinal}] has no command")
        config = row.get("config", "")
        if not isinstance(config, str):
            raise GateError(f"{field}.tests[{ordinal}] has invalid config")
        names.append(name)
        specs.append((name, tuple(command), config))
    if len(names) != len(set(names)):
        raise GateError(f"{field} contains duplicate test names")
    return names, specs


def _canonical_command(value: Iterable[str]) -> tuple[str, ...]:
    return tuple(_normal_path(item) for item in value)


def _command_record(commands: Any, name: str) -> tuple[list[str], int]:
    if not isinstance(commands, dict) or not isinstance(commands.get(name), dict):
        raise GateError(f"receipt commands.{name} is missing")
    row = commands[name]
    argv = row.get("argv", row.get("command"))
    if not isinstance(argv, list) or not argv or not all(isinstance(item, str) and item for item in argv):
        raise GateError(f"receipt commands.{name}.argv is invalid")
    exit_code = row.get("exit_code")
    if isinstance(exit_code, bool) or not isinstance(exit_code, int):
        raise GateError(f"receipt commands.{name}.exit_code is invalid")
    return argv, exit_code


def _command_value(command: list[str], option: str, field: str) -> str:
    positions = [index for index, value in enumerate(command) if value == option]
    if len(positions) != 1 or positions[0] + 1 >= len(command):
        raise GateError(f"{field} must contain exactly one {option} value")
    return command[positions[0] + 1]


def _configured_cmake(build_dir: pathlib.Path, root: pathlib.Path) -> pathlib.Path:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        raise GateError("configured cmake is unavailable")
    candidate: pathlib.Path | None = None
    for line in cache.read_text(encoding="utf-8", errors="strict").splitlines():
        if line.startswith("CMAKE_COMMAND:INTERNAL="):
            value = line.split("=", 1)[1].strip()
            candidate = pathlib.Path(value)
            if not candidate.is_absolute():
                candidate = root / candidate
            break
    if candidate is None:
        raise GateError("configured cmake is unavailable")
    candidate = candidate.resolve(strict=False)
    if not candidate.is_file():
        raise GateError("configured cmake executable is missing")
    return candidate


def _configured_ctest(build_dir: pathlib.Path, root: pathlib.Path) -> pathlib.Path:
    cache = build_dir / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8", errors="strict").splitlines():
            if line.startswith("CMAKE_CTEST_COMMAND:INTERNAL="):
                value = line.split("=", 1)[1].strip()
                candidate = pathlib.Path(value)
                if not candidate.is_absolute():
                    candidate = root / candidate
                return candidate.resolve(strict=False)
    candidate = shutil.which("ctest")
    if candidate:
        return pathlib.Path(candidate).resolve(strict=False)
    raise GateError("configured ctest is unavailable")


def _run_current_ctest_list(root: pathlib.Path, build_dir: pathlib.Path, configuration: str) -> tuple[dict[str, Any], list[str]]:
    ctest = _configured_ctest(build_dir, root)
    if not ctest.is_file():
        raise GateError("configured ctest executable is missing")
    argv = [str(ctest), "--test-dir", str(build_dir), "-C", configuration, "--show-only=json-v1"]
    completed = subprocess.run(argv, capture_output=True, check=False)
    if completed.returncode != 0:
        raise GateError(f"current ctest registration query exited {completed.returncode}")
    try:
        document = json.loads(completed.stdout.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise GateError(f"current ctest registration is not JSON: {type(error).__name__}") from error
    if not isinstance(document, dict):
        raise GateError("current ctest registration is not an object")
    names, _ = _test_specs(document, "current ctest registration")
    return document, names


def _current_source_identity(root: pathlib.Path) -> dict[str, Any]:
    helper = root / "tools" / "product" / "build_identity.ps1"
    if not helper.is_file():
        raise GateError("build_identity.ps1 is missing")
    powershell = shutil.which("powershell.exe")
    if not powershell:
        system_root = os.environ.get("SystemRoot", r"C:\Windows")
        candidate = pathlib.Path(system_root) / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe"
        if candidate.is_file():
            powershell = str(candidate)
    if not powershell:
        raise GateError("powershell.exe is unavailable for current source identity")

    def quote(value: pathlib.Path) -> str:
        return "'" + str(value).replace("'", "''") + "'"

    script = ("$ErrorActionPreference='Stop'; "
              f". {quote(helper)}; "
              f"Get-ProductSourceIdentity -RepoRoot {quote(root)} | ConvertTo-Json -Compress -Depth 5")
    completed = subprocess.run([powershell, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                                "-Command", script], capture_output=True, check=False)
    if completed.returncode != 0:
        raise GateError(f"current source identity exited {completed.returncode}")
    try:
        identity = json.loads(completed.stdout.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise GateError(f"current source identity is not JSON: {type(error).__name__}") from error
    if not isinstance(identity, dict):
        raise GateError("current source identity is not an object")
    for field in ("head", "commit", "source_sha256", "dirty"):
        if field not in identity:
            raise GateError(f"current source identity lacks {field}")
    _validate_hash(identity["source_sha256"], "current source identity.source_sha256")
    if not isinstance(identity["head"], str) or not identity["head"] or not isinstance(identity["commit"], str):
        raise GateError("current source identity has invalid commit fields")
    if not isinstance(identity["dirty"], bool):
        raise GateError("current source identity.dirty is invalid")
    return identity


def _junit_summary(path: pathlib.Path) -> tuple[list[str], dict[str, int]]:
    xml_bytes = path.read_bytes()
    if b"<!DOCTYPE" in xml_bytes.upper() or b"<!ENTITY" in xml_bytes.upper():
        raise GateError("JUnit DTD or entity declarations are not allowed")
    try:
        root = ET.fromstring(xml_bytes)
    except (ET.ParseError, UnicodeError) as error:
        raise GateError(f"JUnit is not valid XML: {type(error).__name__}") from error
    cases = root.findall(".//testcase")
    names = [case.get("name", "") for case in cases]
    if not names or any(not name for name in names) or len(names) != len(set(names)):
        raise GateError("JUnit has no unique testcase names")
    failures = root.findall(".//failure")
    errors = root.findall(".//error")
    skipped_nodes = root.findall(".//skipped")
    suites = [root] if root.tag == "testsuite" else root.findall(".//testsuite")

    def declared(attribute: str) -> int:
        values = []
        for suite in suites:
            value = suite.get(attribute)
            if value is not None:
                try:
                    values.append(int(value))
                except ValueError as error:
                    raise GateError(f"JUnit {attribute} total is not an integer") from error
        return sum(values) if values else 0

    totals = {"tests": declared("tests") or len(cases),
              "failures": declared("failures"), "errors": declared("errors"),
              "skipped": declared("skipped")}
    if totals["tests"] != len(cases) or totals["failures"] != len(failures) or totals["errors"] != len(errors):
        raise GateError("JUnit declared totals do not match testcase nodes")
    if totals["skipped"] != len(skipped_nodes):
        raise GateError("JUnit declared skipped total does not match testcase nodes")
    return names, totals


def _validate_verification_receipt(root: pathlib.Path, build_dir: pathlib.Path, junit_arg: pathlib.Path,
                                   receipt_path: pathlib.Path, source: dict[str, Any]) -> tuple[bool, str, dict[str, Any]]:
    try:
        if not receipt_path.is_file() or receipt_path.is_symlink():
            raise GateError("verification receipt is missing or is a link")
        receipt_relative = _repo_relative(root, receipt_path)
        _relative_build_path(root, receipt_relative, "verification receipt")
        receipt = _json_file(receipt_path, "verification receipt")
        if receipt.get("format") != 1 or receipt.get("kind") != "epocanva-backend-verification" or receipt.get("status") != "verified":
            raise GateError("verification receipt format/kind/status is not current")
        if receipt.get("build_dir") != _repo_relative(root, build_dir):
            raise GateError("verification receipt build_dir does not match --build-dir")
        if receipt.get("configuration") != "Release":
            raise GateError("verification receipt is not a Release verification")

        receipt_source = receipt.get("source")
        if not isinstance(receipt_source, dict):
            raise GateError("verification receipt source identity is missing")
        for field in ("head", "commit", "source_sha256", "dirty"):
            if receipt_source.get(field) != source.get(field):
                raise GateError(f"verification receipt source.{field} is stale")
        _validate_hash(receipt_source.get("source_sha256"), "verification receipt source.source_sha256")

        cache_ref = receipt.get("cache")
        _read_file_ref(root, cache_ref, "verification receipt cache")
        if _repo_relative(root, build_dir / "CMakeCache.txt") != cache_ref.get("path"):
            raise GateError("verification receipt cache is not the current build cache")

        registered_ref = receipt.get("registered_tests")
        registered_path, _, _ = _read_file_ref(root, registered_ref, "verification receipt registered_tests")
        stored_document = _json_file(registered_path, "registered test list")
        stored_names, stored_specs = _test_specs(stored_document, "registered test list")
        receipt_names = registered_ref.get("names")
        if receipt_names != stored_names:
            raise GateError("receipt registered test names do not match its test-list file")
        current_document, current_names = _run_current_ctest_list(root, build_dir, "Release")
        _, current_specs = _test_specs(current_document, "current ctest registration")
        if current_names != stored_names or current_specs != stored_specs:
            raise GateError("current registered CTest names or commands differ from the receipt")
        missing_groups = [prefix for prefix in REQUIRED_TEST_PREFIXES
                          if not any(name.startswith(prefix) for name in current_names)]
        if missing_groups:
            raise GateError("current CTest registration lacks required group(s): " + ", ".join(missing_groups))

        commands = receipt.get("commands")
        build_command, build_exit = _command_record(commands, "build")
        list_command, list_exit = _command_record(commands, "list")
        test_command, test_exit = _command_record(commands, "test")
        if build_exit != 0 or list_exit != 0 or test_exit != 0:
            raise GateError("verification command exit code is not zero")
        cmake = _configured_cmake(build_dir, root)
        ctest = _configured_ctest(build_dir, root)
        expected_cmake = _normal_path(str(cmake))
        expected_ctest = _normal_path(str(ctest))
        build_normal = list(_canonical_command(build_command))
        if build_normal[0].casefold() != expected_cmake.casefold() or "--build" not in build_normal:
            raise GateError("receipt build command does not use configured cmake --build")
        build_index = build_normal.index("--build")
        if build_index + 1 >= len(build_normal) or build_normal[build_index + 1].casefold() != _normal_path(str(build_dir)).casefold():
            raise GateError("receipt build command targets a different build directory")
        if _command_value(build_normal, "--config", "receipt build command") != "Release":
            raise GateError("receipt build command is not Release")
        if not _canonical_command(list_command)[0].casefold() == expected_ctest.casefold():
            raise GateError("receipt list command does not use configured ctest")
        list_normal = list(_canonical_command(list_command))
        if "--show-only=json-v1" not in list_normal or "--test-dir" not in list_normal:
            raise GateError("receipt list command is not ctest --show-only=json-v1")
        if _command_value(list_normal, "--test-dir", "receipt list command").casefold() != _normal_path(str(build_dir)).casefold():
            raise GateError("receipt list command targets a different build directory")
        if _command_value(list_normal, "-C", "receipt list command") != "Release":
            raise GateError("receipt list command is not Release")
        if "--test-dir" not in test_command or "--output-junit" not in test_command or "--no-tests=error" not in test_command:
            raise GateError("receipt test command does not record the complete CTest invocation")
        if _normal_path(_command_value(test_command, "--test-dir", "receipt test command")).casefold() != _normal_path(str(build_dir)).casefold():
            raise GateError("receipt test command targets a different build directory")
        if _command_value(test_command, "-C", "receipt test command") != "Release":
            raise GateError("receipt test command is not Release")
        test_normal = list(_canonical_command(test_command))
        if test_normal[0].casefold() != expected_ctest.casefold():
            raise GateError("receipt test command does not use configured ctest")
        junit_relative = _repo_relative(root, junit_arg)
        if pathlib.Path(_command_value(test_command, "--output-junit", "receipt test command")).resolve(strict=False) != junit_arg:
            raise GateError("receipt test command --output-junit differs from --junit")
        if receipt.get("junit", {}).get("path") != junit_relative:
            raise GateError("--junit does not match receipt JUnit path")
        junit_ref = receipt.get("junit")
        junit_path, _, _ = _read_file_ref(root, junit_ref, "verification receipt junit")
        if junit_path != junit_arg:
            raise GateError("receipt JUnit path does not match --junit")
        junit_names, junit_totals = _junit_summary(junit_path)
        if sorted(junit_names) != sorted(current_names):
            raise GateError("JUnit testcase names do not exactly match current registered tests")
        if any(junit_totals[key] != 0 for key in ("failures", "errors", "skipped")):
            raise GateError("JUnit contains failures, errors or skipped tests")
        if junit_ref.get("names") != junit_names or junit_ref.get("totals") != junit_totals:
            raise GateError("receipt JUnit names/totals do not match the JUnit file")

        binaries = receipt.get("binaries")
        if not isinstance(binaries, list) or not binaries:
            raise GateError("verification receipt has no binary references")
        binary_paths: dict[str, pathlib.Path] = {}
        for ordinal, binary in enumerate(binaries):
            if not isinstance(binary, dict) or not isinstance(binary.get("name"), str) or not binary["name"]:
                raise GateError(f"verification receipt binary[{ordinal}] is invalid")
            path, _, _ = _read_file_ref(root, binary, f"verification receipt binary[{ordinal}]")
            relative = binary["path"]
            if relative in binary_paths:
                raise GateError("verification receipt has duplicate binary references")
            binary_paths[relative] = path
        for name, command, _config in current_specs:
            for token in command:
                normalized = _normal_path(token)
                candidate_text = normalized.split("=", 1)[1] if "=" in normalized and normalized.split("=", 1)[0].upper().endswith(("EXE", "DLL", "RIME", "PROBE")) else normalized
                if not re.search(r"\.(?:exe|dll)$", candidate_text, re.IGNORECASE):
                    continue
                candidate = pathlib.Path(candidate_text)
                if not candidate.is_absolute():
                    continue
                candidate = candidate.resolve(strict=False)
                if candidate == ctest or candidate.name.casefold() in ("cmake.exe", "ctest.exe"):
                    continue
                if candidate.is_file():
                    relative = _repo_relative(root, candidate)
                    if relative not in binary_paths:
                        raise GateError(f"binary referenced by registered test is absent from receipt: {name}")

        detail = f"current source, {len(current_names)} exact registered tests, JUnit totals {junit_totals['tests']}, and {len(binary_paths)} binaries bound"
        return True, detail, receipt
    except (OSError, KeyError, TypeError, GateError, ValueError) as error:
        return False, str(error), {}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--models", type=pathlib.Path, required=True)
    parser.add_argument("--junit", type=pathlib.Path, required=True)
    parser.add_argument("--verification-receipt", type=pathlib.Path,
                        help="current actual backend verification receipt; absent receipts block the gate")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    args.models = args.models.resolve()
    args.build_dir = args.build_dir.resolve()
    args.junit = args.junit.resolve()
    out = args.output.resolve()
    build_root = (root / "build").resolve()
    if not out.is_relative_to(build_root):
        raise ValueError("report must remain in build/")
    checks: list[dict[str, Any]] = []

    def check(name: str, passed: bool, detail: str) -> None:
        checks.append({"id": name, "status": "passed" if passed else "blocked", "detail": detail})

    try:
        source = _current_source_identity(root)
    except GateError as error:
        source = {"head": "unavailable", "commit": "unavailable", "source_sha256": "0" * 64, "dirty": True}
        check("current-source-identity", False, str(error))
    else:
        check("current-source-identity", True, "Current source identity came from tools/product/build_identity.ps1.")
    check("clean-source", source.get("dirty") is False,
          "No uncommitted source may be relabeled as a stable artifact.")

    policy_path = root / "data" / "manifests" / "backend-readiness.json"
    try:
        policy = json.loads(policy_path.read_text(encoding="utf-8"))
        policy_ok = policy["required_languages"] == list(LANGUAGES)
        check("language-scope", policy_ok,
              "Five ordered languages are mandatory; frontend scope is separate.")
    except (OSError, json.JSONDecodeError, KeyError, TypeError) as error:
        policy = {"artifact_scope": "unknown", "limits": {"aggregate_ngram_bytes": 0}, "languages": []}
        check("language-scope", False, type(error).__name__)
    for language in LANGUAGES:
        rows = [row for row in policy.get("languages", []) if row.get("id") == language]
        check("complete-" + language,
              len(rows) == 1 and rows[0].get("status") == "complete" and not rows[0].get("blockers"),
              "; ".join(rows[0].get("blockers", [])) if rows else "missing language")

    if args.verification_receipt is None:
        check("verification-receipt", False,
              "No --verification-receipt supplied; current backend execution evidence is required.")
        check("executed-backend-tests", False,
              "A historical JUnit or name-prefix match cannot substitute for a current verification receipt.")
    else:
        receipt_path = args.verification_receipt.resolve()
        try:
            receipt_relative = _repo_relative(root, receipt_path)
            _relative_build_path(root, receipt_relative, "verification receipt")
            passed, detail, receipt = _validate_verification_receipt(
                root, args.build_dir, args.junit, receipt_path, source)
        except (OSError, ValueError, GateError) as error:
            passed, detail, receipt = False, str(error), {}
        check("verification-receipt", passed, detail)
        check("executed-backend-tests", passed,
              detail if passed else "Current registered tests/JUnit/commands/binaries are not bound: " + detail)

    models: list[dict[str, Any]] = []
    for language in LANGUAGES:
        model = args.models / language / "model.tsv"
        quality = args.models / language / "evaluation.json"
        report = args.models / language / "report.json"
        try:
            value = json.loads(quality.read_text(encoding="utf-8"))
            provenance = json.loads(report.read_text(encoding="utf-8"))
            actual = digest(model)
            bindings = value.get("inputs_sha256", {})
            measured = value.get("model", {})
            base = value.get("unigram_baseline", {})
            ci = value.get("bootstrap_95_percentile_ci", {}).get("delta.mrr", {})
            valid = (value.get("quality_claim_eligible") is True and
                     measured.get("mrr", 0) > base.get("mrr", 1) and ci.get("low_95", -1) > 0)
            binding_hash = bindings.get("model")
            check("quality-" + language, valid and binding_hash == actual,
                  "Exact-model held-out MRR improvement with positive grouped confidence interval; semantic coverage is separately gated.")
            models.append({"language": language, "path": str(model), "sha256": actual,
                           "bytes": model.stat().st_size, "quality_sha256": digest(quality),
                           "training_report_sha256": digest(report)})
        except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
            check("quality-" + language, False, type(error).__name__)
    total = sum(row["bytes"] for row in models)
    limit = policy.get("limits", {}).get("aggregate_ngram_bytes", 0)
    check("aggregate-model-budget", len(models) == 5 and total <= limit,
          f"{total} bytes across {len(models)} models; budget {limit}.")
    check("measured-rss-and-tail-latency", False,
          "Core query timing is not peak-RSS or complete synchronous backend p99 evidence.")
    check("source-reconstruction", False,
          "A source-only commit is not a verified offline reconstruction of third-party data/models and relinkable native libraries.")
    check("data-distribution", False,
          "Official notices recorded; complete model/word-list distribution package not yet verified.")
    check("neural-default-disabled", policy.get("limits", {}).get("neural_default_admitted") is False,
          "Measured CPU reranker misses deadline; not silently enabled.")
    ready = all(row["status"] == "passed" for row in checks)
    result = {"format": 1, "scope": policy.get("artifact_scope", "unknown"),
              "source_commit": source.get("commit"), "source_sha256": source.get("source_sha256"),
              "policy_sha256": digest(policy_path) if policy_path.is_file() else None,
              "verification_receipt": str(args.verification_receipt) if args.verification_receipt else None,
              "stable_ready": ready, "checks": checks, "models": models,
              "exit_code": 0 if ready else 1}
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("backend_stable_ready=" + str(ready).lower())
    for row in checks:
        if row["status"] == "blocked":
            print("BLOCKED " + row["id"] + ": " + row["detail"])
    return result["exit_code"]


if __name__ == "__main__":
    raise SystemExit(main())
