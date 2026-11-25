#!/usr/bin/env python3
"""Reject missing, duplicate, skipped or nonpassing root CTest executions."""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import re
import xml.etree.ElementTree as ET


def _unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    folded: set[str] = set()
    result: dict[str, object] = {}
    for name, value in pairs:
        if name.casefold() in folded:
            raise ValueError("duplicate or case-aliased CTest inventory member")
        folded.add(name.casefold())
        result[name] = value
    return result


def _finite_number(token: str) -> float:
    value = float(token)
    if not math.isfinite(value):
        raise ValueError("nonfinite CTest inventory number")
    return value


def verify(registered_path: Path, junit_path: Path) -> int:
    inventory = json.loads(registered_path.read_text(encoding="utf-8-sig"),
                           object_pairs_hook=_unique_object, parse_float=_finite_number,
                           parse_constant=_finite_number)
    if (type(inventory) is not dict or inventory.get("kind") != "ctestInfo"
            or type(inventory.get("version")) is not dict or type(inventory.get("tests")) is not list):
        raise ValueError("invalid CTest inventory schema")
    version = inventory["version"]
    if (type(version.get("major")) is not int or version["major"] != 1
            or type(version.get("minor")) is not int or version["minor"] < 0):
        raise ValueError("unsupported CTest inventory version")
    registered = []
    for row in inventory["tests"]:
        if type(row) is not dict or type(row.get("name")) is not str or not row["name"].strip():
            raise ValueError("invalid registered testcase name")
        registered.append(row["name"])
    raw = junit_path.read_text(encoding="utf-8-sig")
    if "\x00" in raw or "<!DOCTYPE" in raw.upper() or "<!ENTITY" in raw.upper():
        raise ValueError("DTD and entity declarations are not accepted")
    root = ET.fromstring(raw)
    if root.tag not in ("testsuite", "testsuites") or any("}" in node.tag for node in root.iter()):
        raise ValueError("unsupported JUnit schema")
    cases = list(root.iter("testcase"))
    executed = [case.get("name") for case in cases]
    if not registered or len(registered) != len(set(registered)):
        raise ValueError("registered tests absent or duplicated")
    if len(executed) != len(set(executed)) or set(executed) != set(registered):
        raise ValueError("executed tests differ from the exact registered inventory")
    if any(child.tag == "testcase" and parent.tag != "testsuite" for parent in root.iter() for child in parent):
        raise ValueError("misplaced JUnit testcase")
    if any(list(root.iter(tag)) for tag in ("failure", "error", "skipped")):
        raise ValueError("failed, errored or skipped test execution")
    for case in cases:
        if case.get("status", "run") not in ("run", "passed"):
            raise ValueError("unexecuted testcase status")
        if case.get("result", "completed") not in ("completed", "passed"):
            raise ValueError("nonpassing testcase result")
    for suite in list(root.iter("testsuite")) + list(root.iter("testsuites")):
        for name in ("failures", "errors", "skipped", "disabled"):
            if not re.fullmatch(r"0+", suite.get(name, "0")):
                raise ValueError("nonzero JUnit summary")
        if "tests" in suite.attrib:
            count = suite.attrib["tests"]
            if not re.fullmatch(r"[0-9]+", count) or int(count) != len(list(suite.iter("testcase"))):
                raise ValueError("JUnit summary count mismatch")
    return len(cases)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registered", required=True, type=Path)
    parser.add_argument("--junit", required=True, type=Path)
    args = parser.parse_args()
    try:
        count = verify(args.registered, args.junit)
    except (OSError, ValueError, KeyError, TypeError, ET.ParseError) as error:
        print(f"ROOT JUNIT BLOCKED: {error}")
        return 1
    print(f"ROOT JUNIT PASS: {count} exact registered/executed tests; zero failures/errors/skips")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
