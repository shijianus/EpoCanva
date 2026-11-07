#!/usr/bin/env python3
"""Exact CTest inventory verifier for the contract gate.

Runs ``ctest --show-only=json-v1`` in a configured build tree, selects the
tests that carry the ``contract-gate`` label, and compares that set exactly
against the version-controlled expected inventory
``tools/ci/contract-tests.json``. A missing test, a duplicate test name, or
an unregistered new contract test is a failure -- the previous regex-based
invocation proved the presence of a pattern, never that every expected group
actually ran.

The verified inventory is a precondition: run this before executing the
labelled suite, and run the suite by label (``ctest -L contract-gate``), so
the gate executes exactly the set it proved.

Exit codes: 0 the inventory matches exactly; 1 any mismatch or an
unreadable build tree / ctest output (fail closed); 2 invalid arguments.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

GATE_LABEL = "contract-gate"


class InventoryError(RuntimeError):
    """The real inventory could not be proven to match the expected one."""


def load_ctest_inventory(ctest: str, build_dir: Path, config: str) -> list[dict]:
    try:
        completed = subprocess.run(
            [ctest, "--test-dir", str(build_dir), "-C", config, "--show-only=json-v1"],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace", timeout=300, shell=False,
            check=False)
    except OSError as exc:
        raise InventoryError(f"cannot run ctest: {exc}") from exc
    if completed.returncode != 0:
        raise InventoryError(
            f"ctest --show-only=json-v1 failed with status {completed.returncode}")
    try:
        document = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise InventoryError(f"ctest inventory is not valid JSON: {exc}") from exc
    tests = document.get("tests")
    if not isinstance(tests, list):
        raise InventoryError("ctest inventory has no tests array")
    return tests


def test_labels(test: dict) -> set[str]:
    """Extract a test's labels from the json-v1 properties list.

    CMake 3.31 emits ``{"name": "LABELS", "value": [...]}``; older drafts used
    ``values``. Both are accepted, nothing else is guessed.
    """
    labels: set[str] = set()
    for prop in test.get("properties", []):
        if prop.get("name") == "LABELS":
            values = prop["value"] if "value" in prop else prop.get("values", [])
            if not isinstance(values, list):
                raise InventoryError("ctest LABELS property is not a list")
            labels.update(value for value in values if isinstance(value, str))
    return labels


def gate_test_names(tests: list[dict]) -> list[str]:
    """Names of the tests labelled contract-gate, in ctest's own order."""
    names: list[str] = []
    for test in tests:
        if not isinstance(test, dict):
            raise InventoryError("ctest inventory has a malformed test entry")
        if GATE_LABEL in test_labels(test):
            names.append(test["name"])
    return names


def compare_inventories(observed: list[str], expected: list[str]) -> None:
    """Exact set comparison; duplicates, gaps and extras all fail."""
    duplicates = sorted({name for name in observed if observed.count(name) > 1})
    if duplicates:
        raise InventoryError(f"duplicate contract test names in the build tree: {duplicates}")
    observed_set = set(observed)
    expected_set = set(expected)
    missing = sorted(expected_set - observed_set)
    unexpected = sorted(observed_set - expected_set)
    if missing:
        raise InventoryError(
            f"expected contract tests are not registered in the build tree: {missing}")
    if unexpected:
        raise InventoryError(
            f"tests carry the {GATE_LABEL} label but are not in the expected inventory: "
            f"{unexpected}; update tools/ci/contract-tests.json in the same change")
    if len(expected) != len(expected_set):
        raise InventoryError("the expected inventory file itself contains duplicates")


def load_expected(path: Path) -> list[str]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise InventoryError(f"expected inventory unreadable: {exc}") from exc
    expected = document.get("tests")
    if not isinstance(expected, list) or any(not isinstance(name, str) for name in expected):
        raise InventoryError(f"{path} must hold {{\"tests\": [names...]}}")
    return expected


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ctest", required=True, help="ctest executable path")
    parser.add_argument("--build-dir", required=True, help="configured CTest build tree")
    parser.add_argument("--config", default="Debug", help="multi-config configuration")
    parser.add_argument("--expected", required=True,
                        help="version-controlled expected inventory JSON")
    args = parser.parse_args(argv)
    try:
        observed = gate_test_names(
            load_ctest_inventory(args.ctest, Path(args.build_dir), args.config))
        expected = load_expected(Path(args.expected))
        compare_inventories(observed, expected)
    except InventoryError as exc:
        print(f"contract inventory: {exc}", file=sys.stderr)
        return 1
    print(f"contract inventory: exact match, {len(set(expected))} tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
