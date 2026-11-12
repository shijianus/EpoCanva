#!/usr/bin/env python3
"""Repeat a candidate menu across fresh processes and fail on any divergence.

This is the determinism regression protocol, not corpus qualification. It runs
the real evaluator in baseline mode with an explicit fresh-process count and
in-process repeat count, then requires every language's ordered menu to be
identical within each process and across all processes. Any divergence, failed
case, wrong protocol or missing language exits nonzero.

Optionally it starts CPU load processes for the duration of the run; the same
input must still produce the same menu under contention. All artifacts stay
under the repository build/ directory. Candidate surfaces remain in the
evaluator's runs files; this summary records rates, counts and hashes only.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


class ProtocolError(ValueError):
    pass


def confined(path: Path) -> Path:
    build = (REPO / "build").resolve()
    if ".." in path.parts:
        raise ProtocolError("artifact path must not contain '..'")
    resolved = path.resolve()
    if not (resolved == build or build in resolved.parents):
        raise ProtocolError("artifact path must stay under the repository build/ directory")
    return resolved


def git_head() -> str:
    completed = subprocess.run(
        ["git", "-C", str(REPO), "rev-parse", "--verify", "HEAD"],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, encoding="utf-8", errors="replace", check=False, timeout=120)
    head = completed.stdout.strip()
    if completed.returncode != 0 or len(head) != 40:
        raise ProtocolError("cannot determine the current commit")
    return head


def start_load(count: int) -> list[subprocess.Popen]:
    processes = []
    code = "while True: pass\n"
    for _ in range(count):
        processes.append(subprocess.Popen(
            [sys.executable, "-c", code], stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    return processes


def stop_load(processes: list[subprocess.Popen]) -> None:
    for process in processes:
        try:
            process.terminate()
        except OSError:
            pass
    for process in processes:
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)


def assess(report: dict, runs: int, repeat: int) -> dict:
    languages = ("en", "zh", "fr", "ja", "ko")
    problems: list[str] = []
    determinism = report.get("determinism") or {}
    if determinism.get("fresh_runs") != runs:
        problems.append(f"fresh_runs {determinism.get('fresh_runs')!r} != {runs}")
    if determinism.get("repeats_per_case") != repeat:
        problems.append(f"repeats_per_case {determinism.get('repeats_per_case')!r} != {repeat}")
    if report.get("failures"):
        problems.append(f"{len(report['failures'])} case-level failures")
    per_language = report.get("per_language") or {}
    if set(per_language) != set(languages):
        problems.append("per_language does not cover exactly the five languages")
    summary = {}
    for language in languages:
        values = per_language.get(language) or {}
        rates = {
            "deterministic_case_rate": values.get("deterministic_case_rate"),
            "in_process_repeat_deterministic_rate": values.get("in_process_repeat_deterministic_rate"),
            "cross_process_deterministic_rate": values.get("cross_process_deterministic_rate"),
        }
        summary[language] = {
            "cases": values.get("cases"),
            "measured_cases": values.get("measured_cases"),
            "rates": rates,
        }
        for name, rate in rates.items():
            if rate != 1.0:
                problems.append(f"{language}: {name}={rate!r}")
        if values.get("cases") != values.get("measured_cases"):
            problems.append(f"{language}: not every case produced a measured menu")
    return {"status": "passed" if not problems else "failed",
            "problems": problems, "per_language": summary}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cases", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--harness", type=Path, required=True)
    parser.add_argument("--rime-dll", type=Path, required=True)
    parser.add_argument("--rime-dll-sha256", required=True)
    parser.add_argument("--rime-shared", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--runs", type=int, default=20)
    parser.add_argument("--repeat", type=int, default=8)
    parser.add_argument("--warmup-queries", type=int, default=0)
    parser.add_argument("--cpu-load-processes", type=int, default=0)
    parser.add_argument("--timeout", type=float, default=7200.0)
    args = parser.parse_args(argv)
    try:
        if not (1 <= args.repeat <= 8):
            raise ProtocolError("repeat must be 1..8")
        if args.runs < 2:
            raise ProtocolError("runs must be >= 2 to test cross-process determinism")
        if args.cpu_load_processes < 0:
            raise ProtocolError("cpu load process count cannot be negative")
        output = confined(args.output_dir / "report.json")
        output.parent.mkdir(parents=True, exist_ok=True)
        head = git_head()
        argv_eval = [
            sys.executable, "-B", str(REPO / "tools/backend/evaluate_candidates.py"),
            "--cases", str(args.cases.resolve()),
            "--manifest", str(args.manifest.resolve()),
            "--bindings", str((REPO / "data/locale/backend-bindings.json").resolve()),
            "--resources", str(args.resources.resolve()),
            "--harness", str(args.harness.resolve()),
            "--output", str(output),
            "--mode", "baseline",
            "--expected-repo-commit", head,
            "--runs", str(args.runs),
            "--repeat", str(args.repeat),
            "--warmup-queries", str(args.warmup_queries),
            "--seed", "20261107",
            "--bootstrap-replicates", "100",
            "--rime-dll", str(args.rime_dll.resolve()),
            "--rime-dll-sha256", args.rime_dll_sha256,
            "--rime-shared", str(args.rime_shared.resolve()),
            "--timeout", str(args.timeout),
        ]
        load = start_load(args.cpu_load_processes) if args.cpu_load_processes else []
        try:
            completed = subprocess.run(
                argv_eval, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, encoding="utf-8",
                errors="replace", timeout=args.timeout, shell=False, check=False)
        finally:
            stop_load(load)
        (args.output_dir / "evaluator.stdout.log").write_text(completed.stdout, encoding="utf-8")
        (args.output_dir / "evaluator.stderr.log").write_text(completed.stderr, encoding="utf-8")
        if completed.returncode != 0:
            summary = {"status": "failed", "label": args.label,
                       "problems": [f"evaluator exit {completed.returncode}"],
                       "protocol": {"runs": args.runs, "repeat": args.repeat,
                                    "warmup_queries": args.warmup_queries,
                                    "cpu_load_processes": args.cpu_load_processes},
                       "repo_commit": head}
            (args.output_dir / "summary.json").write_text(
                json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            print(f"determinism {args.label}: FAILED (evaluator exit {completed.returncode})",
                  file=sys.stderr)
            return 1
        report = json.loads(output.read_text(encoding="utf-8"))
        result = assess(report, args.runs, args.repeat)
        summary = {
            "status": result["status"], "label": args.label,
            "problems": result["problems"],
            "protocol": {"runs": args.runs, "repeat": args.repeat,
                         "warmup_queries": args.warmup_queries,
                         "cpu_load_processes": args.cpu_load_processes},
            "repo_commit": head,
            "report_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
            "per_language": result["per_language"],
        }
        (args.output_dir / "summary.json").write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        if result["status"] != "passed":
            for problem in result["problems"]:
                print(f"determinism {args.label}: {problem}", file=sys.stderr)
            return 1
        print(f"determinism {args.label}: passed ({args.runs} processes x {args.repeat} repeats per case)")
        return 0
    except (OSError, ProtocolError, ValueError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
