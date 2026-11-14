#!/usr/bin/env python3
"""Reject missing, duplicate, skipped or nonpassing root CTest executions."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import xml.etree.ElementTree as ET


def verify(registered_path: Path, junit_path: Path) -> int:
    registered = [row["name"] for row in json.loads(registered_path.read_text(encoding="utf-8-sig"))["tests"]]
    raw = junit_path.read_bytes()
    if b"<!DOCTYPE" in raw.upper() or b"<!ENTITY" in raw.upper():
        raise ValueError("DTD and entity declarations are not accepted")
    root = ET.fromstring(raw)
    cases = list(root.iter("testcase"))
    executed = [case.get("name") for case in cases]
    if not registered or len(registered) != len(set(registered)):
        raise ValueError("registered tests absent or duplicated")
    if len(executed) != len(set(executed)) or sorted(executed) != sorted(registered):
        raise ValueError("executed tests differ from the exact registered inventory")
    if any(list(root.iter(tag)) for tag in ("failure", "error", "skipped")):
        raise ValueError("failed, errored or skipped test execution")
    for case in cases:
        if case.get("status", "run") not in ("run", "passed"):
            raise ValueError("unexecuted testcase status")
    for suite in list(root.iter("testsuite")) + list(root.iter("testsuites")):
        for name in ("failures", "errors", "skipped", "disabled"):
            if int(suite.get(name, "0")) != 0:
                raise ValueError("nonzero JUnit summary")
        if "tests" in suite.attrib and int(suite.attrib["tests"]) != len(list(suite.iter("testcase"))):
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
