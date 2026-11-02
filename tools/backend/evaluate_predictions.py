#!/usr/bin/env python3
"""Evaluate real offline backend predictions against held-out corpus cases.

The evaluator never invents candidates. It runs the native prediction CLI with
--evaluate in model and --baseline-unigram modes, or accepts the two actual
JSONL output files. See docs/testing/backend-prediction-quality.md.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

MIN_LANGUAGE_EXAMPLES = 1001
DEFAULT_BOOTSTRAPS = 10_000
EXPECTED_STATUS = "ok"


class EvaluationError(ValueError):
    """Invalid, incomplete, or inconsistent evaluation input."""


def read_jsonl(path: Path, description: str) -> list[dict[str, Any]]:
    if not path.is_file():
        raise EvaluationError(f"{description} is not a regular file: {path}")
    rows: list[dict[str, Any]] = []
    with path.open("rb") as stream:
        for line_number, raw in enumerate(stream, 1):
            if not raw.strip():
                continue
            try:
                item = json.loads(raw.decode("utf-8", errors="strict"))
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                raise EvaluationError(f"{description} line {line_number} is not UTF-8 JSON") from exc
            if not isinstance(item, dict):
                raise EvaluationError(f"{description} line {line_number} must be a JSON object")
            rows.append(item)
    if not rows:
        raise EvaluationError(f"{description} contains no records")
    return rows


def read_cases(path: Path) -> list[dict[str, Any]]:
    cases = read_jsonl(path, "evaluation cases")
    seen: set[str] = set()
    for line_number, case in enumerate(cases, 1):
        case_id, language = case.get("id"), case.get("language")
        context, target = case.get("context"), case.get("target_text")
        group_id = case.get("group_id")
        if not isinstance(case_id, str) or not case_id or case_id in seen:
            raise EvaluationError(f"evaluation case line {line_number} needs a unique nonempty string id")
        if not isinstance(language, str) or not language:
            raise EvaluationError(f"evaluation case line {line_number} needs language")
        if not isinstance(context, list) or len(context) > 2 or any(not isinstance(token, str) for token in context):
            raise EvaluationError(f"evaluation case line {line_number} context must have <=2 text tokens")
        if not isinstance(target, str) or not target:
            raise EvaluationError(f"evaluation case line {line_number} needs a nonempty target_text")
        if not isinstance(group_id, str) or not group_id:
            raise EvaluationError(f"evaluation case line {line_number} needs group_id for grouped confidence intervals")
        if case.get("split", "test") != "test":
            raise EvaluationError(f"evaluation case line {line_number} is not from the held-out test split")
        seen.add(case_id)
    return cases


def validate_predictions(rows: list[dict[str, Any]], cases: list[dict[str, Any]],
                         description: str) -> tuple[dict[str, list[str]], dict[str, Any]]:
    expected = {case["id"]: case["language"] for case in cases}
    ranked: dict[str, list[str]] = {}
    bad_status = 0
    incomplete = 0
    language_mismatch = 0
    latencies: list[float] = []
    status_counts: dict[str, int] = defaultdict(int)
    for line_number, item in enumerate(rows, 1):
        case_id = item.get("id")
        language = item.get("language")
        candidates = item.get("ranked")
        status = item.get("status")
        complete = item.get("complete")
        if not isinstance(case_id, str) or not case_id:
            raise EvaluationError(f"{description} line {line_number} needs a nonempty id")
        if case_id not in expected:
            raise EvaluationError(f"{description} contains unknown id {case_id!r}")
        if case_id in ranked:
            raise EvaluationError(f"{description} contains duplicate id {case_id!r}")
        if not isinstance(language, str):
            raise EvaluationError(f"{description} line {line_number} needs language")
        if language != expected[case_id]:
            language_mismatch += 1
        if not isinstance(candidates, list) or len(candidates) > 64 or any(not isinstance(word, str) for word in candidates):
            raise EvaluationError(f"{description} line {line_number} ranked must be a text array of at most 64 candidates")
        if len(set(candidates)) != len(candidates):
            raise EvaluationError(f"{description} line {line_number} has duplicate candidate texts")
        if not isinstance(status, str) or not status:
            raise EvaluationError(f"{description} line {line_number} needs status")
        status_counts[status] += 1
        if status != EXPECTED_STATUS:
            bad_status += 1
        if not isinstance(complete, bool):
            raise EvaluationError(f"{description} line {line_number} needs boolean complete")
        if not complete:
            incomplete += 1
        latency = item.get("latency_ms")
        if latency is not None:
            if isinstance(latency, bool) or not isinstance(latency, (int, float)) or not math.isfinite(latency) or latency < 0:
                raise EvaluationError(f"{description} line {line_number} latency_ms must be finite and nonnegative")
            latencies.append(float(latency))
        ranked[case_id] = candidates
    stats = {
        "result_rows": len(rows),
        "missing_rows": len(expected) - len(ranked),
        "non_ok_status_rows": bad_status,
        "incomplete_rows": incomplete,
        "language_mismatch_rows": language_mismatch,
        "status_counts": dict(sorted(status_counts.items())),
        "latency_samples": len(latencies),
        "latency_ms_mean": sum(latencies) / len(latencies) if latencies else None,
        "latency_ms_p50": percentile(latencies, 0.50) if latencies else None,
        "latency_ms_p95": percentile(latencies, 0.95) if latencies else None,
        "quality_eligible": not (bad_status or incomplete or language_mismatch or len(expected) != len(ranked)),
    }
    return ranked, stats


def percentile(values: list[float], proportion: float) -> float:
    if not values:
        raise EvaluationError("percentile of empty values")
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(proportion * len(ordered)) - 1))
    return ordered[index]


def run_backend(executable: Path, model: Path, requests: Path, output: Path,
                *, baseline: bool, timeout: float) -> dict[str, Any]:
    """Invoke the compiled CLI once; it loads the model once for all request rows."""
    argv = [str(executable), "--model", str(model), "--evaluate", str(requests),
            "--output", str(output)]
    if baseline:
        argv.append("--baseline-unigram")
    executable_sha256_before = hashlib.sha256(executable.read_bytes()).hexdigest()
    try:
        completed = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, encoding="utf-8",
                                   errors="replace", timeout=timeout, shell=False, check=False)
    except OSError as exc:
        raise EvaluationError(f"cannot start native backend CLI {executable}: {exc}") from exc
    except subprocess.TimeoutExpired as exc:
        raise EvaluationError(f"native backend CLI exceeded timeout ({timeout:g}s)") from exc
    if completed.returncode != 0:
        # Include only status, not stderr, because host-side diagnostics may contain
        # corpus content. The actual return code is retained for honest reporting.
        raise EvaluationError(f"native backend CLI exited with observed status {completed.returncode}")
    if not output.is_file():
        raise EvaluationError("native backend CLI reported success but did not create results JSONL")
    executable_sha256_after = hashlib.sha256(executable.read_bytes()).hexdigest()
    if executable_sha256_before != executable_sha256_after:
        raise EvaluationError("native backend CLI changed while evaluation was running; results are invalid")
    return {"command_exit_status": completed.returncode, "baseline_mode": baseline,
            "executable_sha256_before": executable_sha256_before,
            "executable_sha256_after": executable_sha256_after,
            "results_sha256": hashlib.sha256(output.read_bytes()).hexdigest()}


def rank_metrics(cases: list[dict[str, Any]], ranked_by_id: dict[str, list[str]]) -> dict[str, Any]:
    hits = {1: 0, 3: 0, 5: 0}
    reciprocal_sum = 0.0
    missing = 0
    for case in cases:
        candidates = ranked_by_id.get(case["id"])
        if candidates is None:
            missing += 1
            continue
        try:
            rank = candidates.index(case["target_text"]) + 1
        except ValueError:
            continue
        reciprocal_sum += 1.0 / rank
        for cutoff in hits:
            if rank <= cutoff:
                hits[cutoff] += 1
    count = len(cases)
    if not count:
        return {"examples": 0, "missing_predictions": 0, "coverage": 0.0,
                "hit_at_1": 0.0, "hit_at_3": 0.0, "hit_at_5": 0.0, "mrr": 0.0}
    return {"examples": count, "missing_predictions": missing,
            "coverage": (count - missing) / count,
            "hit_at_1": hits[1] / count, "hit_at_3": hits[3] / count,
            "hit_at_5": hits[5] / count, "mrr": reciprocal_sum / count}


def _group_values(cases: list[dict[str, Any]], ranked: dict[str, list[str]], metric: str) -> dict[str, list[float]]:
    result: dict[str, list[float]] = defaultdict(list)
    for case in cases:
        group_id = case["group_id"]
        candidates = ranked.get(case["id"])
        if metric == "coverage":
            value = 1.0 if candidates is not None else 0.0
        elif candidates is None:
            value = 0.0
        else:
            try:
                rank = candidates.index(case["target_text"]) + 1
            except ValueError:
                rank = 0
            if metric == "hit_at_1":
                value = float(rank == 1)
            elif metric == "hit_at_3":
                value = float(0 < rank <= 3)
            elif metric == "hit_at_5":
                value = float(0 < rank <= 5)
            elif metric == "mrr":
                value = 1.0 / rank if rank else 0.0
            else:
                raise EvaluationError("unknown metric")
        result[group_id].append(value)
    return result


def _weighted_group_mean(sampled_groups: list[str], grouped: dict[str, list[float]]) -> float:
    numerator = sum(sum(grouped[group]) for group in sampled_groups)
    denominator = sum(len(grouped[group]) for group in sampled_groups)
    return numerator / denominator if denominator else 0.0


def bootstrap_intervals(cases: list[dict[str, Any]], model: dict[str, list[str]],
                        baseline: dict[str, list[str]], *, seed: int,
                        replicates: int) -> dict[str, Any]:
    """Paired percentile bootstrap over document/source groups, not token rows."""
    if replicates < 100:
        raise EvaluationError("bootstrap replicates must be >=100")
    import random
    groups = sorted({case["group_id"] for case in cases}, key=lambda item: item.encode("utf-8"))
    if not groups:
        return {}
    rng = random.Random(seed)
    metrics = ("hit_at_1", "hit_at_3", "hit_at_5", "mrr", "coverage")
    grouped_model = {metric: _group_values(cases, model, metric) for metric in metrics}
    grouped_baseline = {metric: _group_values(cases, baseline, metric) for metric in metrics}
    samples: dict[str, list[float]] = {}
    for metric in metrics:
        samples[f"model.{metric}"] = []
        samples[f"unigram_baseline.{metric}"] = []
        samples[f"delta.{metric}"] = []
    for _ in range(replicates):
        sample = [groups[rng.randrange(len(groups))] for _ in groups]
        for metric in metrics:
            model_value = _weighted_group_mean(sample, grouped_model[metric])
            baseline_value = _weighted_group_mean(sample, grouped_baseline[metric])
            samples[f"model.{metric}"].append(model_value)
            samples[f"unigram_baseline.{metric}"].append(baseline_value)
            samples[f"delta.{metric}"].append(model_value - baseline_value)
    return {key: {"low_95": percentile(values, 0.025), "high_95": percentile(values, 0.975)}
            for key, values in sorted(samples.items())}


def evaluate(cases: list[dict[str, Any]], model_predictions: dict[str, list[str]],
             baseline_predictions: dict[str, list[str]], model_stats: dict[str, Any],
             baseline_stats: dict[str, Any], *, seed: int, replicates: int,
             min_examples: int) -> dict[str, Any]:
    by_language: dict[str, Any] = {}
    insufficient: list[str] = []
    for language in sorted({case["language"] for case in cases}):
        subset = [case for case in cases if case["language"] == language]
        count = len(subset)
        if count < min_examples:
            insufficient.append(language)
        model = rank_metrics(subset, model_predictions)
        baseline = rank_metrics(subset, baseline_predictions)
        by_language[language] = {
            "test_examples": count,
            "minimum_examples_met": count >= min_examples,
            "model": model,
            "unigram_baseline": baseline,
            "bootstrap_95_percentile_ci": bootstrap_intervals(
                subset, model_predictions, baseline_predictions,
                seed=seed, replicates=replicates),
        }
    baseline_eligible = bool(baseline_stats.get("quality_eligible"))
    model_eligible = bool(model_stats.get("quality_eligible"))
    return {
        "protocol": "epocanva-prediction-eval-v1",
        "cases": len(cases),
        "groups": len({case["group_id"] for case in cases}),
        "minimum_examples_per_language": min_examples,
        "languages_below_minimum": insufficient,
        "model_quality_eligible": model_eligible and not insufficient,
        "baseline_quality_eligible": baseline_eligible and not insufficient,
        "quality_claim_eligible": model_eligible and baseline_eligible and not insufficient,
        "model": rank_metrics(cases, model_predictions),
        "unigram_baseline": rank_metrics(cases, baseline_predictions),
        "bootstrap_95_percentile_ci": bootstrap_intervals(
            cases, model_predictions, baseline_predictions, seed=seed,
            replicates=replicates),
        "by_language": by_language,
        "model_result_integrity": model_stats,
        "baseline_result_integrity": baseline_stats,
        "metrics": "missing predictions and OOV targets count as misses; coverage is prediction-row coverage",
    }


def _save_report(path: Path, report: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    encoded = (json.dumps(report, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode("utf-8")
    temporary = path.with_name(path.name + ".tmp")
    try:
        temporary.write_bytes(encoded)
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cases", required=True, help="held-out target_text/group_id JSONL")
    parser.add_argument("--requests", required=True, help="matching 5-column backend TSV requests")
    parser.add_argument("--model-results", help="actual native model-mode JSONL results")
    parser.add_argument("--baseline-results", help="actual native --baseline-unigram JSONL results")
    parser.add_argument("--backend-executable", help="native backend CLI; run model and baseline modes")
    parser.add_argument("--model", required=True, help="trained v1 TSV used by native backend CLI")
    parser.add_argument("--output", default="build/backend-training/evaluation.json")
    parser.add_argument("--min-examples", type=int, default=MIN_LANGUAGE_EXAMPLES)
    parser.add_argument("--bootstrap-replicates", type=int, default=DEFAULT_BOOTSTRAPS)
    parser.add_argument("--seed", type=int, default=20261102)
    parser.add_argument("--backend-timeout", type=float, default=3600.0)
    args = parser.parse_args(argv)
    if args.min_examples <= 0 or args.bootstrap_replicates < 100 or args.backend_timeout <= 0:
        parser.error("minimum examples, bootstrap replicates >=100, and timeout must be positive")
    supplied_results = args.model_results is not None or args.baseline_results is not None
    if supplied_results == (args.backend_executable is not None):
        parser.error("provide both --model-results and --baseline-results, or --backend-executable")
    if supplied_results and (not args.model_results or not args.baseline_results):
        parser.error("--model-results and --baseline-results must be provided together")
    try:
        cases_path = Path(args.cases)
        requests_path = Path(args.requests)
        cases = read_cases(cases_path)
        if args.backend_executable:
            executable = Path(args.backend_executable)
            model_path = Path(args.model)
            output_path = Path(args.output)
            output_path.parent.mkdir(parents=True, exist_ok=True)
            model_temp = output_path.with_name(output_path.stem + ".model-results.jsonl")
            baseline_temp = output_path.with_name(output_path.stem + ".baseline-results.jsonl")
            model_command = run_backend(executable, model_path, requests_path, model_temp,
                                        baseline=False, timeout=args.backend_timeout)
            baseline_command = run_backend(executable, model_path, requests_path, baseline_temp,
                                           baseline=True, timeout=args.backend_timeout)
            model_rows = read_jsonl(model_temp, "native model results")
            baseline_rows = read_jsonl(baseline_temp, "native baseline results")
            model_command["output_path"] = str(model_temp)
            baseline_command["output_path"] = str(baseline_temp)
        else:
            model_temp = Path(args.model_results)
            baseline_temp = Path(args.baseline_results)
            model_rows = read_jsonl(model_temp, "model results")
            baseline_rows = read_jsonl(baseline_temp, "unigram baseline results")
            model_command = {"source": "provided actual native output", "results_sha256": hashlib.sha256(model_temp.read_bytes()).hexdigest()}
            baseline_command = {"source": "provided actual native baseline output", "results_sha256": hashlib.sha256(baseline_temp.read_bytes()).hexdigest()}
        model_predictions, model_stats = validate_predictions(model_rows, cases, "model results")
        baseline_predictions, baseline_stats = validate_predictions(baseline_rows, cases, "baseline results")
        model_stats["command"] = model_command
        baseline_stats["command"] = baseline_command
        report = evaluate(cases, model_predictions, baseline_predictions,
                          model_stats, baseline_stats, seed=args.seed,
                          replicates=args.bootstrap_replicates,
                          min_examples=args.min_examples)
        report["inputs_sha256"] = {
            "cases": hashlib.sha256(cases_path.read_bytes()).hexdigest(),
            "requests": hashlib.sha256(requests_path.read_bytes()).hexdigest(),
            "model": hashlib.sha256(Path(args.model).read_bytes()).hexdigest(),
        }
        report["bootstrap"] = {"method": "paired percentile bootstrap resampling groups/document_id rows",
                               "seed": args.seed, "replicates": args.bootstrap_replicates}
        report_path = Path(args.output)
        _save_report(report_path, report)
        print(f"wrote evaluation metrics to {report_path}")
        if report["languages_below_minimum"]:
            print("quality claim withheld: fewer than %d target examples for: %s" %
                  (args.min_examples, ", ".join(report["languages_below_minimum"])), file=sys.stderr)
        if not model_stats["quality_eligible"] or not baseline_stats["quality_eligible"]:
            print("quality claim withheld: backend status, completion, coverage, or language integrity failed", file=sys.stderr)
    except (OSError, EvaluationError, UnicodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
