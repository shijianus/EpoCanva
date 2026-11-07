#!/usr/bin/env python3
"""Evaluate the real candidate menu the backend publishes (E6 baseline).

Reads the committed Tier A corpus (data/eval/candidate-v1), replays it through
the native ``epocanva_candidate_eval_cli`` harness against the staged backend
resources, and computes the design-document metrics over the actual ordered
menus. The evaluator never invents candidates: every measured surface comes
from the harness's results JSONL, and every metric that cannot be computed is
reported as null rather than guessed.

See docs/testing/candidate-evaluation-baseline.md for the metric definitions,
the raw-first contract note, and what this baseline does and does not claim.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import random
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

LANGUAGES = ("en", "zh", "fr", "ja", "ko")
SCHEMES = ("latin", "pinyin", "zhuyin", "romaji", "kana", "two_set", "katakana")
KIND_RAW = 0
DEFAULT_BOOTSTRAPS = 10_000
DEFAULT_SEED = 20261107
DEFAULT_RUNS = 2
DEFAULT_REPEAT = 2
GATE_MINIMUM_CASES = {"en": 1001, "zh": 1001, "fr": 1001, "ja": 1001, "ko": 500}
ALLOWED_CASE_FIELDS = frozenset((
    "id", "language", "scheme", "profile_id", "raw_input", "preedit", "context",
    "expected", "expected_languages", "group_id", "split", "source", "license",
    "attribution", "tags"))
ID_PATTERN = re.compile(r"^[A-Za-z0-9._-]+$")
COMMIT_PATTERN = re.compile(r"^[0-9a-f]{40}$")


class EvaluationError(ValueError):
    """Invalid, incomplete, or inconsistent evaluation input."""


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def percentile(values: list[float], proportion: float) -> float:
    if not values:
        raise EvaluationError("percentile of empty values")
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(proportion * len(ordered)) - 1))
    return ordered[index]


def optional_ratio(numerator: int, denominator: int) -> float | None:
    """A zero denominator is reported as null, never as 0.0 or 1.0."""
    if denominator == 0:
        return None
    return numerator / denominator


# --- corpus ------------------------------------------------------------------

def read_corpus_file(path: Path, language: str) -> list[dict[str, Any]]:
    if not path.is_file():
        raise EvaluationError(f"corpus file missing: {path.name}")
    raw = path.read_bytes()
    if raw.startswith(b"\xef\xbb\xbf"):
        raise EvaluationError(f"corpus file has a UTF-8 BOM: {path.name}")
    if b"\r" in raw:
        raise EvaluationError(f"corpus file is not LF-only: {path.name}")
    cases: list[dict[str, Any]] = []
    for line_number, line in enumerate(raw.split(b"\n"), 1):
        if line == b"":
            if line_number == len(raw.split(b"\n")):
                break  # a single trailing newline is required, more is tolerated
            raise EvaluationError(f"{path.name} line {line_number} is blank")
        try:
            text = line.decode("utf-8", errors="strict")
        except UnicodeDecodeError as exc:
            raise EvaluationError(f"{path.name} line {line_number} is not UTF-8") from exc
        try:
            record = json.loads(text)
        except json.JSONDecodeError as exc:
            raise EvaluationError(f"{path.name} line {line_number} is not JSON") from exc
        if not isinstance(record, dict):
            raise EvaluationError(f"{path.name} line {line_number} is not a JSON object")
        cases.append(record)
    if not cases:
        raise EvaluationError(f"{path.name} contains no records")
    return cases


def check_text_field(value: Any, name: str, where: str, maximum_bytes: int) -> None:
    if not isinstance(value, str) or not value:
        raise EvaluationError(f"{where} {name} must be a nonempty string")
    if len(value.encode("utf-8")) > maximum_bytes:
        raise EvaluationError(f"{where} {name} exceeds {maximum_bytes} UTF-8 bytes")
    if any(ord(scalar) < 0x20 or ord(scalar) == 0x7F for scalar in value):
        raise EvaluationError(f"{where} {name} contains a control scalar")
    if "\\u" in value:
        # The corpus is authored without \\u escapes so the native parser and the
        # Python validator read identical bytes.
        raise EvaluationError(f"{where} {name} uses a \\\\u escape")


def validate_cases(language: str, records: list[dict[str, Any]], bindings: dict[int, dict[str, Any]],
                   seen_ids: set[str]) -> list[dict[str, Any]]:
    cases: list[dict[str, Any]] = []
    for ordinal, record in enumerate(records, 1):
        where = f"{language}.jsonl record {ordinal}"
        unknown = set(record) - ALLOWED_CASE_FIELDS
        if unknown:
            raise EvaluationError(f"{where} has unknown fields {sorted(unknown)}")
        case_id = record.get("id")
        if not isinstance(case_id, str) or not ID_PATTERN.match(case_id) or len(case_id) > 128:
            raise EvaluationError(f"{where} id must match [A-Za-z0-9._-]+ and stay under 128 bytes")
        if case_id in seen_ids:
            raise EvaluationError(f"{where} duplicates id {case_id!r}")
        seen_ids.add(case_id)
        if record.get("language") != language:
            raise EvaluationError(f"{where} language must be {language!r}")
        scheme = record.get("scheme")
        if scheme not in SCHEMES:
            raise EvaluationError(f"{where} scheme {scheme!r} is not a declared input scheme")
        profile_id = record.get("profile_id")
        if not isinstance(profile_id, int) or isinstance(profile_id, bool) or profile_id <= 0:
            raise EvaluationError(f"{where} profile_id must be a positive integer")
        binding = bindings.get(profile_id)
        if binding is None:
            raise EvaluationError(f"{where} profile_id {profile_id} is not a committed backend binding")
        if binding["language_tag"] != language:
            raise EvaluationError(f"{where} binding {profile_id} belongs to {binding['language_tag']!r}")
        if binding["scheme_id"] != scheme:
            raise EvaluationError(
                f"{where} binding {profile_id} declares scheme {binding['scheme_id']!r}, not {scheme!r}")
        check_text_field(record.get("raw_input"), "raw_input", where, 4096)
        preedit = record.get("preedit")
        if preedit is not None:
            check_text_field(preedit, "preedit", where, 4096)
        context = record.get("context", [])
        if not isinstance(context, list) or len(context) > 2:
            raise EvaluationError(f"{where} context must be an array of at most 2 tokens")
        for token_ordinal, token in enumerate(context, 1):
            check_text_field(token, f"context token {token_ordinal}", where, 256)
        expected = record.get("expected")
        if not isinstance(expected, list) or not 1 <= len(expected) <= 8:
            raise EvaluationError(f"{where} expected must hold 1..8 acceptable surfaces")
        for surface_ordinal, surface in enumerate(expected, 1):
            check_text_field(surface, f"expected surface {surface_ordinal}", where, 4096)
        if len(set(expected)) != len(expected):
            raise EvaluationError(f"{where} expected contains duplicate surfaces")
        expected_languages = record.get("expected_languages")
        if (not isinstance(expected_languages, list) or not expected_languages or
                any(item not in LANGUAGES for item in expected_languages)):
            raise EvaluationError(f"{where} expected_languages must be nonempty within the five languages")
        if language not in expected_languages:
            raise EvaluationError(f"{where} expected_languages must admit the case language {language!r}")
        group_id = record.get("group_id")
        if not isinstance(group_id, str) or not group_id:
            raise EvaluationError(f"{where} needs group_id for grouped confidence intervals")
        if record.get("split") != "test":
            raise EvaluationError(f"{where} must come from the test split")
        check_text_field(record.get("source"), "source", where, 128)
        license_field = record.get("license")
        if not isinstance(license_field, str) or not license_field:
            raise EvaluationError(f"{where} needs a license identifier")
        attribution = record.get("attribution")
        if attribution is not None and not isinstance(attribution, dict):
            raise EvaluationError(f"{where} attribution must be an object when present")
        tags = record.get("tags", [])
        if not isinstance(tags, list) or any(not isinstance(tag, str) or not tag for tag in tags):
            raise EvaluationError(f"{where} tags must be an array of nonempty strings when present")
        cases.append(record)
    return cases


def load_bindings(path: Path) -> dict[int, dict[str, Any]]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise EvaluationError(f"backend bindings manifest unreadable: {exc}") from exc
    bindings: dict[int, dict[str, Any]] = {}
    for row in document.get("bindings", []):
        if not isinstance(row, dict) or not isinstance(row.get("numeric_id"), int):
            raise EvaluationError("backend bindings manifest has a malformed row")
        bindings[row["numeric_id"]] = row
    if not bindings:
        raise EvaluationError("backend bindings manifest has no bindings")
    return bindings


def load_manifest(path: Path, cases_dir: Path, bindings_sha256: str,
                  case_counts: dict[str, int]) -> dict[str, Any]:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise EvaluationError(f"corpus manifest unreadable: {exc}") from exc
    if manifest.get("format") != "epocanva-candidate-eval-manifest-v1":
        raise EvaluationError("corpus manifest format is not epocanva-candidate-eval-manifest-v1")
    if manifest.get("corpus_id") != "candidate-v1" or manifest.get("corpus_version") != 1:
        raise EvaluationError("corpus manifest identity mismatch")
    if manifest.get("license") != "AGPL-3.0-only":
        raise EvaluationError("corpus manifest license is not the committed Tier A licence")
    if manifest.get("harness_version") != 1:
        raise EvaluationError("corpus manifest harness_version mismatch")
    if manifest.get("backend_manifest_sha256") != bindings_sha256:
        raise EvaluationError(
            "corpus manifest backend_manifest_sha256 is stale against data/locale/backend-bindings.json")
    digests = manifest.get("corpus_sha256")
    counts = manifest.get("case_count")
    expected_files = {f"{language}.jsonl" for language in LANGUAGES}
    if not isinstance(digests, dict) or set(digests) != expected_files:
        raise EvaluationError("corpus manifest corpus_sha256 must cover exactly the five language files")
    if not isinstance(counts, dict) or set(counts) != set(LANGUAGES):
        raise EvaluationError("corpus manifest case_count must cover exactly the five languages")
    present = {entry.name for entry in cases_dir.iterdir()} if cases_dir.is_dir() else set()
    if present != expected_files:
        raise EvaluationError(
            f"corpus directory must contain exactly {sorted(expected_files)}, found {sorted(present)}")
    for language in LANGUAGES:
        actual = sha256_bytes((cases_dir / f"{language}.jsonl").read_bytes())
        if digests[f"{language}.jsonl"] != actual:
            raise EvaluationError(f"corpus manifest digest is stale for {language}.jsonl")
        if counts[language] != case_counts[language]:
            raise EvaluationError(f"corpus manifest case_count is stale for {language}")
    return manifest


# --- harness invocation ------------------------------------------------------

def run_harness(executable: Path, argv: list[str], output_path: Path, timeout: float) -> dict[str, Any]:
    executable_sha256_before = sha256_bytes(executable.read_bytes())
    try:
        completed = subprocess.run(
            [str(executable)] + argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace",
            timeout=timeout, shell=False, check=False)
    except OSError as exc:
        raise EvaluationError(f"cannot start native harness {executable}: {exc}") from exc
    except subprocess.TimeoutExpired as exc:
        raise EvaluationError(f"native harness exceeded timeout ({timeout:g}s)") from exc
    if completed.returncode != 0:
        # stderr may name stages and codes only; the exit code is kept, the text
        # is dropped, because harness diagnostics must stay text-free by design.
        raise EvaluationError(f"native harness exited with observed status {completed.returncode}")
    executable_sha256_after = sha256_bytes(executable.read_bytes())
    if executable_sha256_before != executable_sha256_after:
        raise EvaluationError("native harness changed while evaluation was running; results are invalid")
    if not output_path.is_file():
        raise EvaluationError("native harness reported success but did not create results JSONL")
    return {"exit_status": completed.returncode,
            "harness_sha256_before": executable_sha256_before,
            "harness_sha256_after": executable_sha256_after,
            "results_sha256": sha256_bytes(output_path.read_bytes())}


def read_results(path: Path) -> tuple[dict[str, Any], dict[str, list[dict[str, Any]]]]:
    meta: dict[str, Any] | None = None
    cases: dict[str, list[dict[str, Any]]] = {}
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError as exc:
            raise EvaluationError(f"{path.name} line {line_number} is not JSON") from exc
        if "run_meta" in record:
            if meta is not None or len(record) != 1:
                raise EvaluationError(f"{path.name} line {line_number} has a malformed run_meta line")
            meta = record["run_meta"]
            continue
        case_id = record.get("case_id")
        if not isinstance(case_id, str) or not case_id:
            raise EvaluationError(f"{path.name} line {line_number} needs a case_id")
        if case_id in cases:
            raise EvaluationError(f"{path.name} line {line_number} repeats case {case_id!r}")
        cases[case_id] = record
    if meta is None:
        raise EvaluationError(f"{path.name} has no run_meta line")
    return meta, cases


def menu_signature(repeat: dict[str, Any]) -> tuple:
    return tuple(
        (row.get("kind"), row.get("text_utf8"), tuple(row.get("lexeme_ids", [])),
         row.get("input_begin_utf8_byte"), row.get("input_end_utf8_byte"))
        for row in repeat.get("candidates", []))


# --- metrics -----------------------------------------------------------------

def metric_values(case: dict[str, Any], record: dict[str, Any]) -> dict[str, Any]:
    """Per-case metric contributions from one measured menu."""
    raw_input = case["raw_input"]
    repeats = record.get("repeats", [])
    first = repeats[0] if repeats else {}
    rows = first.get("candidates", [])
    texts = [row.get("text_utf8") for row in rows]
    raw_rows = [row for row in rows if row.get("kind") == KIND_RAW]
    raw_match = [row for row in rows
                 if row.get("text_utf8") == raw_input and row.get("kind") == KIND_RAW]
    suggestions = [row for row in rows if row.get("kind") != KIND_RAW]
    suggestion_texts = [row.get("text_utf8") for row in suggestions]

    def rank_in(expected: list[str], ordered: list[str]) -> int:
        for ordinal, text in enumerate(ordered, 1):
            if text in expected:
                return ordinal
        return 0

    values = {
        "top1": 1.0 if texts and texts[0] in case["expected"] else 0.0,
        "top3": 1.0 if any(text in case["expected"] for text in texts[:3]) else 0.0,
        "mrr": 1.0 / rank_in(case["expected"], texts) if rank_in(case["expected"], texts) else 0.0,
        "suggestion_top1": 1.0 if suggestion_texts and suggestion_texts[0] in case["expected"] else 0.0,
        "suggestion_top3": 1.0 if any(text in case["expected"] for text in suggestion_texts[:3]) else 0.0,
        "suggestion_mrr": (1.0 / rank_in(case["expected"], suggestion_texts))
                          if rank_in(case["expected"], suggestion_texts) else 0.0,
        "raw_present": 1.0 if raw_match else 0.0,
        "raw_first": 1.0 if raw_match and raw_rows and raw_rows[0] is rows[0] else 0.0,
        "empty_menu": 1.0 if not rows else 0.0,
        "wrong_language": 0.0,  # structural: each case runs in its declared profile's session
        "latency_ms": first.get("latency_ns", 0) / 1e6,
    }
    return values


def grouped_bootstrap(cases: list[dict[str, Any]], values_by_id: dict[str, dict[str, Any]],
                      metric_keys: tuple[str, ...], *, seed: int, replicates: int) -> dict[str, Any]:
    if replicates < 100:
        raise EvaluationError("bootstrap replicates must be >=100")
    groups = sorted({case["group_id"] for case in cases})
    if not groups:
        return {}
    rng = random.Random(seed)
    grouped: dict[str, dict[str, list[float]]] = {metric: defaultdict(list) for metric in metric_keys}
    for case in cases:
        values = values_by_id[case["id"]]
        for metric in metric_keys:
            grouped[metric][case["group_id"]].append(values[metric])
    samples: dict[str, list[float]] = {metric: [] for metric in metric_keys}
    for _ in range(replicates):
        sample = [groups[rng.randrange(len(groups))] for _ in groups]
        for metric in metric_keys:
            numerator = sum(sum(grouped[metric][group]) for group in sample)
            denominator = sum(len(grouped[metric][group]) for group in sample)
            samples[metric].append(numerator / denominator if denominator else 0.0)
    return {metric: [percentile(values, 0.025), percentile(values, 0.975)]
            for metric, values in samples.items()}


def aggregate(cases: list[dict[str, Any]], values_by_id: dict[str, dict[str, Any]],
              *, seed: int, replicates: int) -> dict[str, Any]:
    """Aggregate only the cases that actually produced a measured menu."""
    measured = [case for case in cases if case["id"] in values_by_id]
    metric_keys = ("top1", "top3", "mrr", "suggestion_top1", "suggestion_top3", "suggestion_mrr")
    aggregate_values: dict[str, Any] = {}
    for key in metric_keys:
        aggregate_values[key] = (sum(values_by_id[case["id"]][key] for case in measured) / len(measured)
                                 if measured else None)
    aggregate_values["ci95"] = grouped_bootstrap(measured, values_by_id, metric_keys,
                                                 seed=seed, replicates=replicates)
    return aggregate_values


def ratio(cases: list[dict[str, Any]], values_by_id: dict[str, dict[str, Any]], key: str) -> float | None:
    measured = [case for case in cases if case["id"] in values_by_id]
    return optional_ratio(sum(int(values_by_id[case["id"]][key]) for case in measured), len(measured))


# --- report ------------------------------------------------------------------

def evaluate(args: argparse.Namespace) -> dict[str, Any]:
    repo_root = Path(__file__).resolve().parents[2]
    bindings_path = Path(args.bindings)
    bindings_bytes = bindings_path.read_bytes()
    bindings_sha256 = sha256_bytes(bindings_bytes)
    bindings = load_bindings(bindings_path)

    cases_dir = Path(args.cases)
    seen_ids: set[str] = set()
    corpus: dict[str, list[dict[str, Any]]] = {}
    for language in LANGUAGES:
        corpus[language] = validate_cases(language, read_corpus_file(cases_dir / f"{language}.jsonl",
                                                                     language),
                                          bindings, seen_ids)
    manifest = load_manifest(Path(args.manifest), cases_dir, bindings_sha256,
                             {language: len(corpus[language]) for language in LANGUAGES})

    resources_root = Path(args.resources)
    resources_index = (resources_root / "data" / "backend" / "resources.tsv").read_bytes()
    model_sha256: dict[str, str] = {}
    for line in resources_index.decode("utf-8").splitlines()[2:]:
        fields = line.split("\t")
        if len(fields) != 14:
            raise EvaluationError("staged resources.tsv has a malformed row")
        language_by_number = {1: "en", 2: "zh", 3: "fr", 4: "ja", 5: "ko"}
        if not fields[1].isdigit():
            raise EvaluationError("staged resources.tsv has a nonnumeric language column")
        language = language_by_number.get(int(fields[1]))
        if language and language not in model_sha256:
            model_sha256[language] = fields[7]

    output_path = Path(args.output)
    require_safe_output_path(output_path, repo_root)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    runs_dir = output_path.parent / "runs"
    runs_dir.mkdir(exist_ok=True)

    zh_cases = [case for case in corpus["zh"]]
    rime_argv: list[str] = []
    if zh_cases:
        for name in ("rime_dll", "rime_dll_sha256", "rime_shared"):
            if not getattr(args, name):
                raise EvaluationError(f"--{name.replace('_', '-')} is required when zh cases exist")
        work_root = runs_dir / "rime-work"
        work_root.mkdir(exist_ok=True)
        rime_argv = ["--rime-dll", args.rime_dll, "--rime-dll-sha256", args.rime_dll_sha256,
                     "--rime-shared", args.rime_shared, "--work", str(work_root)]

    per_run: dict[str, list[tuple[dict[str, Any], dict[str, list[dict[str, Any]]]]]] = {}
    for language in LANGUAGES:
        if not corpus[language]:
            continue
        per_run[language] = []
        for run_ordinal in range(args.runs):
            results_path = runs_dir / f"{language}.run{run_ordinal}.jsonl"
            argv = ["--resources", str(resources_root),
                    "--cases", str(cases_dir / f"{language}.jsonl"),
                    "--language", language, "--output", str(results_path),
                    "--repeat", str(args.repeat)]
            if rime_argv and language == "zh":
                argv += rime_argv
            command = run_harness(Path(args.harness), argv, results_path, args.timeout)
            meta, records = read_results(results_path)
            meta["command"] = command
            per_run[language].append((meta, records))

    per_language: dict[str, Any] = {}
    failures: list[dict[str, str]] = []
    unverified: dict[str, dict[str, Any]] = {}
    for language, runs in per_run.items():
        cases = corpus[language]
        verified_cases = [case for case in cases if case["scheme"] != "zhuyin"]
        zhuyin_cases = [case for case in cases if case["scheme"] == "zhuyin"]
        if zhuyin_cases:
            unverified["zhuyin"] = {"cases": len(zhuyin_cases), "reason": "scheme-unverified"}

        values_by_id: dict[str, dict[str, Any]] = {}
        deterministic: list[float] = []
        latencies_warm: list[float] = []
        cold_latency: float | None = None
        for case in verified_cases:
            case_id = case["id"]
            records = [records_by_id.get(case_id) for _, records_by_id in runs]
            if any(record is None for record in records):
                failures.append({"case_id": case_id, "reason": "missing-result"})
                continue
            statuses = {record.get("status") for record in records}
            if statuses != {"ok"}:
                failures.append({"case_id": case_id,
                                 "reason": "status-" + "+".join(sorted(str(status) for status in statuses))})
                continue
            signatures = [[menu_signature(repeat) for repeat in record.get("repeats", [])]
                          for record in records]
            lengths_ok = all(len(repeats) == args.repeat for repeats in signatures)
            deterministic.append(1.0 if lengths_ok and all(repeats == signatures[0] for repeats in signatures)
                                 else 0.0)
            if not lengths_ok:
                failures.append({"case_id": case_id, "reason": "repeat-count-mismatch"})
            elif any(repeats != signatures[0] for repeats in signatures):
                failures.append({"case_id": case_id, "reason": "menu-not-deterministic"})
            values_by_id[case_id] = metric_values(case, records[0])
            for run_ordinal, record in enumerate(records):
                for repeat_ordinal, repeat in enumerate(record.get("repeats", [])):
                    latency = repeat.get("latency_ns", 0) / 1e6
                    if run_ordinal == 0 and case is verified_cases[0] and repeat_ordinal == 0:
                        cold_latency = latency
                    else:
                        latencies_warm.append(latency)

        language_values: dict[str, Any] = {
            "cases": len(verified_cases),
            "measured_cases": len(values_by_id),
            "minimum_cases_met": len(verified_cases) >= GATE_MINIMUM_CASES[language],
            **aggregate(verified_cases, values_by_id, seed=args.seed,
                        replicates=args.bootstrap_replicates),
            "raw_present_rate": ratio(verified_cases, values_by_id, "raw_present"),
            "raw_first_rate": ratio(verified_cases, values_by_id, "raw_first"),
            "empty_menu_rate": ratio(verified_cases, values_by_id, "empty_menu"),
            "wrong_language_rate": ratio(verified_cases, values_by_id, "wrong_language"),
            "deterministic_cases": (sum(deterministic) / len(deterministic) if deterministic else None),
            "latency_ms": {"cold": cold_latency,
                           "warm": {"p50": percentile(latencies_warm, 0.50) if latencies_warm else None,
                                    "p95": percentile(latencies_warm, 0.95) if latencies_warm else None,
                                    "p99": percentile(latencies_warm, 0.99) if latencies_warm else None,
                                    "queries": len(latencies_warm)},
                           "warmup_queries": 0},
            "resource_load_ms": runs[0][0].get("load_ms"),
            "peak_working_set_bytes": runs[0][0].get("peak_working_set_bytes"),
        }
        schemes: dict[str, Any] = {}
        for scheme in sorted({case["scheme"] for case in verified_cases}):
            scheme_cases = [case for case in verified_cases if case["scheme"] == scheme]
            schemes[scheme] = {"cases": len(scheme_cases),
                               **aggregate(scheme_cases, values_by_id, seed=args.seed,
                                           replicates=args.bootstrap_replicates)}
        if schemes:
            language_values["schemes"] = schemes
        per_language[language] = language_values

    gate_met = all(per_language[language]["minimum_cases_met"]
                   for language in per_language) and per_language
    report = {
        "format": "epocanva-candidate-eval-v1",
        "corpus": {"id": manifest["corpus_id"], "version": manifest["corpus_version"],
                   "license": manifest["license"]},
        "identity": {
            "repo_commit": args.repo_commit,
            "backend_manifest_sha256": bindings_sha256,
            "resource_manifest_sha256": sha256_bytes(resources_index),
            "model_sha256": model_sha256,
            "harness_sha256": per_run[next(iter(per_run))][0][0]["command"]["harness_sha256_before"],
        },
        "per_language": per_language,
        "unverified": unverified,
        "failures": failures,
        "determinism": {"repeats_per_case": args.repeat, "fresh_runs": args.runs,
                        "method": "full ordered (kind, text_utf8, lexeme_ids, input range) list equality"},
        "quality_gate": {"minimum_cases": GATE_MINIMUM_CASES, "met": bool(gate_met),
                         "claim": "baseline-only" if not gate_met else "gate-met"},
        "metrics_note": (
            "The backend contract pins the raw candidate at ordinal 0 for completion queries, "
            "so top1/top3/mrr measure target position under that contract; the suggestion_* "
            "variants exclude raw-kind rows and track suggestion ordering directly."),
    }
    return report


def require_safe_output_path(output_path: Path, repo_root: Path) -> None:
    """Candidate surfaces land only under build/ or outside the repository."""
    resolved = output_path.resolve()
    try:
        relative = resolved.relative_to(repo_root.resolve())
    except ValueError:
        return  # outside the repository entirely
    if relative.parts and relative.parts[0] == "build":
        return
    raise EvaluationError(
        "report path inside the repository must stay under build/ (results carry candidate surfaces)")


def save_report(path: Path, report: dict[str, Any]) -> None:
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
    parser.add_argument("--cases", required=True, help="data/eval/candidate-v1/cases directory")
    parser.add_argument("--manifest", required=True, help="data/eval/candidate-v1/manifest.json")
    parser.add_argument("--bindings", default="data/locale/backend-bindings.json",
                        help="committed backend binding manifest (ownership oracle)")
    parser.add_argument("--resources", required=True, help="staged backend resource root")
    parser.add_argument("--harness", required=True, help="epocanva_candidate_eval_cli executable")
    parser.add_argument("--output", required=True, help="report JSON path (under build/)")
    parser.add_argument("--repo-commit", required=True, help="40-hex commit SHA under test")
    parser.add_argument("--runs", type=int, default=DEFAULT_RUNS,
                        help="fresh-process harness runs per language (>=1)")
    parser.add_argument("--repeat", type=int, default=DEFAULT_REPEAT,
                        help="in-process repeats per case (>=1)")
    parser.add_argument("--bootstrap-replicates", type=int, default=DEFAULT_BOOTSTRAPS)
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--timeout", type=float, default=3600.0)
    parser.add_argument("--rime-dll", help="rime.dll path, required for zh cases")
    parser.add_argument("--rime-dll-sha256", help="expected rime.dll SHA-256")
    parser.add_argument("--rime-shared", help="reviewed ranked Chinese shared-data directory")
    args = parser.parse_args(argv)
    if args.runs < 1 or args.repeat < 1 or args.bootstrap_replicates < 100 or args.timeout <= 0:
        parser.error("runs/repeat >= 1, bootstrap replicates >= 100, and a positive timeout are required")
    if not COMMIT_PATTERN.match(args.repo_commit):
        parser.error("--repo-commit must be a 40-hex commit SHA")
    try:
        report = evaluate(args)
        save_report(Path(args.output), report)
        print(f"wrote candidate evaluation report to {args.output}")
        if not report["quality_gate"]["met"]:
            print("quality claim withheld: the Tier A corpus is below the E4 entry gate minimums",
                  file=sys.stderr)
        if report["failures"]:
            print(f"failures recorded: {len(report['failures'])}", file=sys.stderr)
    except (OSError, EvaluationError, UnicodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
