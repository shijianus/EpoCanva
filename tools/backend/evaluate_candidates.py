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
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
# Report schema: bumped to v2 when identity became evaluator-verified (real
# `git rev-parse HEAD`, tree cleanliness, declared mode) instead of caller-
# supplied. The corpus manifest format (v1) is unchanged.
EVALUATOR_SCHEMA_VERSION = "epocanva-candidate-eval-v2"
# The quality policy names the mandatory gate predicate set (docs/testing/
# candidate-evaluation-baseline.md section 9 and the quality_gate block of the
# report). Bump it whenever a predicate is added, tightened, or redefined.
QUALITY_POLICY_VERSION = "epocanva-candidate-quality-policy-v1"


class EvaluationError(ValueError):
    """Invalid, incomplete, or inconsistent evaluation input."""


# --- source identity ---------------------------------------------------------

def git_output(repo_root: Path, *arguments: str) -> str:
    """Run git for repository metadata only; failures are EvaluationErrors.

    The message names the subcommand, never stderr content, so diagnostics
    stay free of paths and host details beyond the repository root argument.
    """
    try:
        completed = subprocess.run(
            ["git", "-C", str(repo_root), *arguments], stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            encoding="utf-8", errors="replace", timeout=120, shell=False, check=False)
    except OSError as exc:
        raise EvaluationError(f"cannot run git in the repository: {exc}") from exc
    if completed.returncode != 0:
        raise EvaluationError(
            f"git {arguments[0]} failed with status {completed.returncode}")
    return completed.stdout


def resolve_source_identity(repo_root: Path) -> dict[str, Any]:
    """Bind the evaluation to the real source tree: HEAD, ref, cleanliness.

    repo_commit comes from ``git rev-parse --verify HEAD`` executed by the
    evaluator itself; it is never taken from a caller. The git toplevel must
    be exactly the requested root, so a subdirectory or an unrelated tree
    cannot be passed off as the identity root. source_ref is the branch name
    when HEAD is symbolic and empty when detached -- an empty value is honest,
    a fabricated one is forbidden. source_tree_clean uses
    ``git status --porcelain=v1 -uall``, which covers tracked modifications,
    staged changes and untracked files in one check.
    """
    repo_root = repo_root.resolve()
    toplevel = Path(git_output(repo_root, "rev-parse", "--show-toplevel").strip()).resolve()
    if os.path.normcase(str(toplevel)) != os.path.normcase(str(repo_root)):
        raise EvaluationError(
            "the identity root is not exactly a git work-tree top level; refusing to "
            "attribute another tree's commit to this source")
    head = git_output(repo_root, "rev-parse", "--verify", "HEAD").strip()
    if not COMMIT_PATTERN.match(head):
        raise EvaluationError("git rev-parse HEAD did not return a 40-hex commit SHA")
    try:
        ref = git_output(repo_root, "symbolic-ref", "--short", "-q", "HEAD").strip()
    except EvaluationError:
        ref = ""  # detached HEAD: recorded as empty, never invented
    status = git_output(repo_root, "status", "--porcelain=v1", "-uall")
    return {"repo_commit": head, "source_ref": ref, "source_tree_clean": status == ""}


def check_source_identity(source: dict[str, Any], expected_commit: str | None,
                          mode: str) -> None:
    """Assert the declared expectations against the real source identity.

    An --expected-repo-commit that disagrees with the real HEAD fails in both
    modes: it is an assertion by the caller, never an override. Qualification
    additionally demands a clean tree (no tracked, staged or untracked
    change); baseline records the dirtiness honestly instead of failing.
    """
    if expected_commit is not None and source["repo_commit"] != expected_commit:
        raise EvaluationError(
            "expected repo commit "
            f"{expected_commit} does not match the real HEAD {source['repo_commit']}")
    if mode == "qualification" and not source["source_tree_clean"]:
        raise EvaluationError(
            "qualification mode requires a clean source tree "
            "(tracked, staged and untracked changes are all disallowed)")


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
        # wrong_language is deliberately absent: the public v1 candidate ABI
        # carries no per-candidate language attribution, so no honest per-case
        # value exists. The report records wrong_language_rate as null with an
        # explicit not_observable status; it must never surface as 0.0.
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


# --- staged model identity ---------------------------------------------------

LANGUAGE_BY_STAGED_NUMBER = {1: "en", 2: "zh", 3: "fr", 4: "ja", 5: "ko"}


def read_staged_model_maps(resources_index: bytes) -> tuple[dict[str, str | None],
                                                            dict[str, str]]:
    """Map language -> staged model SHA-256 and profile_id -> model SHA-256.

    The index has one row per profile; same-language profiles share one model
    file by staging design. That sharing is asserted explicitly, never
    assumed: a language whose profiles disagree gets a null per-language hash
    plus a note, and the per-profile map stays the authority. The first
    profile's hash is never silently adopted for its language.
    """
    by_language: dict[str, str] = {}
    disputed: set[str] = set()
    by_profile: dict[str, str] = {}
    for line in resources_index.decode("utf-8").splitlines()[2:]:
        fields = line.split("\t")
        if len(fields) != 14:
            raise EvaluationError("staged resources.tsv has a malformed row")
        if not fields[0].isdigit() or not fields[1].isdigit():
            raise EvaluationError("staged resources.tsv has a nonnumeric id column")
        profile_id, language_number = int(fields[0]), int(fields[1])
        if profile_id == 0 or str(profile_id) in by_profile:
            raise EvaluationError(
                f"staged resources.tsv has a duplicate or zero profile_id {profile_id}")
        model_sha256 = fields[7]
        if not SHA256_PATTERN.match(model_sha256):
            raise EvaluationError(
                f"staged resources.tsv profile {profile_id} has a malformed model SHA-256")
        by_profile[str(profile_id)] = model_sha256
        language = LANGUAGE_BY_STAGED_NUMBER.get(language_number)
        if language is None:
            raise EvaluationError(
                f"staged resources.tsv profile {profile_id} has unknown language number "
                f"{language_number}")
        if language in by_language and by_language[language] != model_sha256:
            disputed.add(language)
        by_language.setdefault(language, model_sha256)
    if not by_profile:
        raise EvaluationError("staged resources.tsv has no profile rows")
    per_language: dict[str, str | None] = {}
    for language in LANGUAGES:
        if language not in by_language:
            continue
        if language in disputed:
            per_language[language] = None
        else:
            per_language[language] = by_language[language]
    notes = {language: "profiles of this language do not share one staged model; "
                       "see model_sha256_by_profile"
             for language in sorted(disputed)}
    return per_language, by_profile, notes


# --- quality gate ------------------------------------------------------------

# Mandatory predicates of QUALITY_POLICY_VERSION. Any predicate that is
# measurably failed makes the gate "failed"; any that cannot be evaluated
# makes it at best "not_evaluable"; "passed" requires every predicate passed.
GATE_PREDICATES = (
    "minimum_samples",
    "exact_process_and_repeat_protocol",
    "all_expected_cases_executed",
    "no_extra_cases",
    "validation_failures_empty",
    "cross_process_determinism",
    "in_process_repeat_determinism",
    "empty_menu_rate_zero",
    "raw_contract",
    "candidate_quality_metrics_recorded",
    "latency_memory_recorded",
    "privacy_and_abi_status",
    "wrong_language_observable",
)
WRONG_LANGUAGE_REASON = ("the public v1 candidate ABI carries no per-candidate language "
                         "attribution, so wrong-language behaviour is not observable at "
                         "this ABI; it is reported as null, never as 0.0, and no Unicode "
                         "script heuristic substitutes for a measurement")


def evaluate_quality_gate(*, per_language: dict[str, Any], failures: list[dict[str, str]],
                          expected_case_ids: dict[str, set[str]],
                          observed_case_ids: dict[str, set[str]],
                          protocol: dict[str, Any]) -> dict[str, Any]:
    """Derive the fail-closed quality_gate block from measured report data.

    Statuses: "failed" when a mandatory predicate measurably fails,
    "not_evaluable" when a mandatory predicate cannot be evaluated (insufficient
    samples, unobservable measurement), "passed" only when every predicate
    passed. "met" is true exactly when status == "passed".
    """
    predicates: dict[str, dict[str, str]] = {}
    unmet_reasons: list[dict[str, str]] = []
    not_evaluated: list[dict[str, str]] = []

    def record(name: str, status: str, reason: str = "") -> None:
        predicates[name] = {"status": status, **({"reason": reason} if reason else {})}
        if status == "failed" and reason:
            unmet_reasons.append({"predicate": name, "reason": reason})
        if status == "not_evaluable" and reason:
            not_evaluated.append({"predicate": name, "reason": reason})

    # minimum_samples -----------------------------------------------------------
    shortfalls = [f"{language}: {values['cases']}/{GATE_MINIMUM_CASES[language]} cases"
                  for language, values in per_language.items()
                  if values["cases"] < GATE_MINIMUM_CASES[language]]
    record("minimum_samples", "failed" if shortfalls else "passed",
           "; ".join(shortfalls))

    # exact_process_and_repeat_protocol ----------------------------------------
    protocol_problems = []
    for language, observed in protocol.items():
        if observed["fresh_runs"] != observed["fresh_runs_expected"]:
            protocol_problems.append(f"{language}: {observed['fresh_runs']} fresh runs, "
                                     f"expected {observed['fresh_runs_expected']}")
        if observed["repeat_counts"] != [observed["repeat_expected"]]:
            protocol_problems.append(
                f"{language}: repeat counts {observed['repeat_counts']}, "
                f"expected exactly [{observed['repeat_expected']}]")
    record("exact_process_and_repeat_protocol",
           "failed" if protocol_problems else "passed", "; ".join(protocol_problems))

    # all_expected_cases_executed / no_extra_cases ------------------------------
    missing = [f"{language}: {len(expected_case_ids.get(language, set()) - observed_case_ids.get(language, set()))} "
               "expected cases absent from the results"
               for language in per_language
               if expected_case_ids.get(language, set()) - observed_case_ids.get(language, set())]
    extra = [f"{language}: {len(observed_case_ids.get(language, set()) - expected_case_ids.get(language, set()))} "
             "unexpected cases present"
             for language in observed_case_ids
             if observed_case_ids.get(language, set()) - expected_case_ids.get(language, set())]
    record("all_expected_cases_executed", "failed" if missing else "passed", "; ".join(missing))
    record("no_extra_cases", "failed" if extra else "passed", "; ".join(extra))

    # validation_failures_empty -------------------------------------------------
    record("validation_failures_empty", "failed" if failures else "passed",
           f"{len(failures)} case-level failures recorded" if failures else "")

    # determinism / empty menu / raw contract -----------------------------------
    for name, key in (("cross_process_determinism", "cross_process_deterministic_rate"),
                      ("in_process_repeat_determinism", "in_process_repeat_deterministic_rate")):
        problems = []
        unevaluable = []
        for language, values in per_language.items():
            rate = values.get(key)
            if rate is None:
                unevaluable.append(language)
            elif rate < 1.0:
                problems.append(f"{language}: {rate:.3f}")
        record(name, "failed" if problems else ("not_evaluable" if unevaluable else "passed"),
               "; ".join(problems + [f"{language}: no measured cases" for language in unevaluable]))
    empty_problems = [f"{language}: empty_menu_rate {values['empty_menu_rate']}"
                      for language, values in per_language.items()
                      if values["empty_menu_rate"] is not None and values["empty_menu_rate"] != 0.0]
    empty_unevaluable = [language for language, values in per_language.items()
                         if values["empty_menu_rate"] is None]
    record("empty_menu_rate_zero",
           "failed" if empty_problems else ("not_evaluable" if empty_unevaluable else "passed"),
           "; ".join(empty_problems + [f"{language}: no measured cases" for language in empty_unevaluable]))
    raw_problems = []
    raw_unevaluable = []
    for language, values in per_language.items():
        for key in ("raw_present_rate", "raw_first_rate"):
            rate = values[key]
            if rate is None:
                raw_unevaluable.append(f"{language}: {key} not measurable")
            elif rate < 1.0:
                raw_problems.append(f"{language}: {key} {rate:.3f}")
    record("raw_contract", "failed" if raw_problems else ("not_evaluable" if raw_unevaluable else "passed"),
           "; ".join(raw_problems + raw_unevaluable))

    # candidate quality metrics + CIs --------------------------------------------
    quality_gaps = []
    for language, values in per_language.items():
        for key in ("top1", "top3", "mrr", "suggestion_top1", "suggestion_top3", "suggestion_mrr"):
            if values.get(key) is None:
                quality_gaps.append(f"{language}: {key} not recorded")
        if not values.get("ci95"):
            quality_gaps.append(f"{language}: grouped confidence intervals empty")
    record("candidate_quality_metrics_recorded",
           "failed" if quality_gaps else ("not_evaluable" if not per_language else "passed"),
           "; ".join(quality_gaps) if quality_gaps else
           ("no languages measured" if not per_language else ""))

    # latency / memory ------------------------------------------------------------
    latency_gaps = []
    for language, values in per_language.items():
        latency = values["latency_ms"]
        if latency["cold"] is None:
            latency_gaps.append(f"{language}: cold latency not recorded")
        for key in ("p50", "p95", "p99"):
            if latency["warm"][key] is None:
                latency_gaps.append(f"{language}: warm {key} not recorded")
        if values["peak_working_set_bytes"] is None:
            latency_gaps.append(f"{language}: peak working set not recorded")
        if values["resource_load_ms"] is None:
            latency_gaps.append(f"{language}: resource load time not recorded")
    record("latency_memory_recorded",
           "failed" if latency_gaps else ("not_evaluable" if not per_language else "passed"),
           "; ".join(latency_gaps) if latency_gaps else
           ("no languages measured" if not per_language else ""))

    # privacy / ABI ---------------------------------------------------------------
    # Evaluable by construction of this evaluator: candidate surfaces reach only
    # the build/-contained results files and report, every harness session is
    # opened with learning_enabled=0 and no learning root, and the evaluated
    # code path performs no network I/O (architectural property, AGENTS.md §2).
    record("privacy_and_abi_status", "passed",
           "surfaces confined to build/ artifacts; learning disabled; no network I/O path")

    # wrong language observability -------------------------------------------------
    record("wrong_language_observable", "not_evaluable", WRONG_LANGUAGE_REASON)

    if any(entry["status"] == "failed" for entry in predicates.values()):
        status = "failed"
    elif any(entry["status"] == "not_evaluable" for entry in predicates.values()):
        status = "not_evaluable"
    else:
        status = "passed"
    return {
        "policy_version": QUALITY_POLICY_VERSION,
        "minimum_cases": GATE_MINIMUM_CASES,
        "status": status,
        "met": status == "passed",
        "claim": {"passed": "gate-met", "failed": "gate-failed",
                  "not_evaluable": "gate-not-evaluable"}[status],
        "unmet_reasons": unmet_reasons,
        "not_evaluated_predicates": not_evaluated,
        "predicates": predicates,
    }


# --- report ------------------------------------------------------------------

def evaluate(args: argparse.Namespace) -> dict[str, Any]:
    repo_root = Path(__file__).resolve().parents[2]
    source = resolve_source_identity(repo_root)
    check_source_identity(source, args.expected_repo_commit, args.mode)

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
    manifest_path = Path(args.manifest)
    manifest = load_manifest(manifest_path, cases_dir, bindings_sha256,
                             {language: len(corpus[language]) for language in LANGUAGES})
    corpus_sha256 = {f"{language}.jsonl": sha256_bytes((cases_dir / f"{language}.jsonl").read_bytes())
                     for language in LANGUAGES}

    resources_root = Path(args.resources)
    resources_index = (resources_root / "data" / "backend" / "resources.tsv").read_bytes()
    model_sha256, model_sha256_by_profile, model_sha256_notes = read_staged_model_maps(
        resources_index)

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
    harness_sha256s = {runs[0][0]["command"]["harness_sha256_before"]
                       for runs in per_run.values()}
    if len(harness_sha256s) != 1:
        raise EvaluationError("the harness binary changed identity between language runs")
    harness_sha256 = harness_sha256s.pop()

    per_language: dict[str, Any] = {}
    failures: list[dict[str, str]] = []
    unverified: dict[str, dict[str, Any]] = {}
    observed_case_ids: dict[str, set[str]] = {}
    protocol: dict[str, Any] = {}
    for language, runs in per_run.items():
        cases = corpus[language]
        verified_cases = [case for case in cases if case["scheme"] != "zhuyin"]
        zhuyin_cases = [case for case in cases if case["scheme"] == "zhuyin"]
        if zhuyin_cases:
            unverified["zhuyin"] = {"cases": len(zhuyin_cases), "reason": "scheme-unverified"}

        observed_case_ids[language] = set()
        for _, records_by_id in runs:
            observed_case_ids[language].update(records_by_id)
        protocol[language] = {
            "fresh_runs": len(runs),
            "fresh_runs_expected": args.runs,
            "repeat_counts": sorted({meta.get("repeat") for meta, _ in runs}),
            "repeat_expected": args.repeat,
        }

        values_by_id: dict[str, dict[str, Any]] = {}
        in_process_rates: list[float] = []
        fresh_process_rates: list[float] = []
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
            within = lengths_ok and all(sigs == signatures[0] for sigs in signatures)
            across = within and all(sigs == signatures[0] for sigs in signatures)
            in_process_rates.append(1.0 if within else 0.0)
            fresh_process_rates.append(1.0 if across else 0.0)
            if not lengths_ok:
                failures.append({"case_id": case_id, "reason": "repeat-count-mismatch"})
            elif not within:
                failures.append({"case_id": case_id, "reason": "menu-not-deterministic"})
            elif not across:
                failures.append({"case_id": case_id,
                                 "reason": "menu-not-deterministic-across-processes"})
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
            # The v1 candidate ABI exposes no per-candidate language, so no
            # honest wrong-language measurement exists; null + not_observable,
            # never 0.0 and never a Unicode script heuristic.
            "wrong_language_rate": None,
            "wrong_language_observability": {
                "status": "not_observable",
                "reason": ("the public v1 candidate ABI carries no per-candidate language "
                           "attribution; the case runs in its declared profile's session"),
            },
            "in_process_repeat_deterministic_rate": (sum(in_process_rates) / len(in_process_rates)
                                                     if in_process_rates else None),
            "cross_process_deterministic_rate": (sum(fresh_process_rates) / len(fresh_process_rates)
                                                 if fresh_process_rates else None),
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

    quality_gate = evaluate_quality_gate(
        per_language=per_language, failures=failures,
        expected_case_ids={language: {case["id"] for case in corpus[language]}
                           for language in per_language},
        observed_case_ids=observed_case_ids, protocol=protocol)
    report = {
        "format": EVALUATOR_SCHEMA_VERSION,
        "corpus": {"id": manifest["corpus_id"], "version": manifest["corpus_version"],
                   "license": manifest["license"]},
        "identity": {
            "repo_commit": source["repo_commit"],
            "source_ref": source["source_ref"],
            "source_tree_clean": source["source_tree_clean"],
            "mode": args.mode,
            "evaluator_schema_version": EVALUATOR_SCHEMA_VERSION,
            "quality_policy_version": QUALITY_POLICY_VERSION,
            "backend_manifest_sha256": bindings_sha256,
            "manifest_sha256": sha256_bytes(manifest_path.read_bytes()),
            "corpus_sha256": corpus_sha256,
            "resource_manifest_sha256": sha256_bytes(resources_index),
            "model_sha256": model_sha256,
            "model_sha256_by_profile": model_sha256_by_profile,
            "model_sha256_notes": model_sha256_notes,
            "harness_sha256": harness_sha256,
        },
        "per_language": per_language,
        "unverified": unverified,
        "failures": failures,
        "determinism": {"repeats_per_case": args.repeat, "fresh_runs": args.runs,
                        "method": "full ordered (kind, text_utf8, lexeme_ids, input range) list equality"},
        "quality_gate": quality_gate,
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
    parser.add_argument("--mode", required=True, choices=("baseline", "qualification"),
                        help="baseline records an unqualified run; qualification enforces "
                             "the full gate and a clean tree")
    parser.add_argument("--expected-repo-commit",
                        help="optional 40-hex assertion: must equal the real HEAD "
                             "(never written into the report as identity)")
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
    if args.expected_repo_commit is not None and not COMMIT_PATTERN.match(
            args.expected_repo_commit):
        parser.error("--expected-repo-commit must be a 40-hex commit SHA")
    if args.mode == "qualification" and args.expected_repo_commit is None:
        parser.error("--expected-repo-commit is required in qualification mode")
    try:
        report = evaluate(args)
        save_report(Path(args.output), report)
        print(f"wrote candidate evaluation report to {args.output}")
        status = report["quality_gate"]["status"]
        if status != "passed":
            reasons = "; ".join(entry["reason"] for entry in report["quality_gate"]["unmet_reasons"])
            unevaluated = "; ".join(entry["predicate"] for entry in
                                    report["quality_gate"]["not_evaluated_predicates"])
            print(f"quality gate {status} (mode {args.mode})", file=sys.stderr)
            if reasons:
                print(f"unmet: {reasons}", file=sys.stderr)
            if unevaluated:
                print(f"not evaluated: {unevaluated}", file=sys.stderr)
        if report["failures"]:
            print(f"failures recorded: {len(report['failures'])}", file=sys.stderr)
        # baseline: an unmet quality gate is recorded evidence, not an exit
        # failure; structural, identity, schema and run-integrity errors above
        # already exit 1. qualification: any unmet or unevaluated mandatory
        # predicate is a nonzero exit, after the report is written as evidence.
        if args.mode == "qualification" and status != "passed":
            return 1
    except (OSError, EvaluationError, UnicodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
