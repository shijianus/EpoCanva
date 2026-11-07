# Tests for tools/backend/evaluate_candidates.py (E6 candidate-menu evaluator).
#
# Two layers, per docs/testing/candidate-evaluation-baseline.md section 8:
# 1. Every corpus/manifest validator rule is exercised as a negative test, so a
#    weakened rule cannot pass silently; the committed Tier A corpus and the
#    committed manifest must also pass against the live bindings file, which
#    makes the manifest's backend_manifest_sha256 a freshness gate.
# 2. The metric functions are pinned to hand-computed vectors, including the
#    raw-first contract behaviour, so a broken metric cannot pass silently.
# The native harness itself is not exercised here: it needs the Tier 2 staged
# inputs, and its real run is recorded in the phase report.
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "backend"))

import evaluate_candidates as ev  # noqa: E402

BINDINGS_PATH = REPO_ROOT / "data" / "locale" / "backend-bindings.json"
CASES_DIR = REPO_ROOT / "data" / "eval" / "candidate-v1" / "cases"
MANIFEST_PATH = REPO_ROOT / "data" / "eval" / "candidate-v1" / "manifest.json"

BASE_CASE = {
    "id": "en-prefix-hel-0001",
    "language": "en",
    "scheme": "latin",
    "profile_id": 1,
    "raw_input": "hel",
    "context": [],
    "expected": ["hello"],
    "expected_languages": ["en"],
    "group_id": "en-prefix-family-01",
    "split": "test",
    "source": "epocanva-synthetic-v1",
    "license": "AGPL-3.0-only",
}

DELETE = object()  # sentinel: remove the key entirely


def mutated_case(**changes):
    case = dict(BASE_CASE)
    for key, value in changes.items():
        if value is DELETE:
            case.pop(key, None)
        else:
            case[key] = value
    return case


class ValidatorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bindings = ev.load_bindings(BINDINGS_PATH)

    def validate(self, cases, bindings=None):
        seen: set[str] = set()
        ev.validate_cases("en", cases, bindings or self.bindings, seen)

    def test_committed_corpus_validates_against_live_bindings(self):
        seen: set[str] = set()
        total = 0
        for language in ev.LANGUAGES:
            records = ev.read_corpus_file(CASES_DIR / f"{language}.jsonl", language)
            validated = ev.validate_cases(language, records, self.bindings, seen)
            self.assertEqual(len(validated), len(records))
            total += len(validated)
        self.assertEqual(total, 15)

    def test_committed_manifest_is_fresh(self):
        counts = {}
        for language in ev.LANGUAGES:
            counts[language] = len(ev.read_corpus_file(CASES_DIR / f"{language}.jsonl", language))
        manifest = ev.load_manifest(MANIFEST_PATH, CASES_DIR,
                                    ev.sha256_bytes(BINDINGS_PATH.read_bytes()), counts)
        self.assertEqual(manifest["corpus_id"], "candidate-v1")

    def test_manifest_rejects_stale_bindings_hash(self):
        counts = {"en": 2, "zh": 4, "fr": 3, "ja": 4, "ko": 2}
        with self.assertRaises(ev.EvaluationError):
            ev.load_manifest(MANIFEST_PATH, CASES_DIR, "0" * 64, counts)

    def test_manifest_rejects_stale_case_digest(self):
        manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        stale = dict(manifest)
        stale["corpus_sha256"] = dict(manifest["corpus_sha256"])
        stale["corpus_sha256"]["en.jsonl"] = "0" * 64
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stale_path = root / "manifest.json"
            stale_path.write_text(json.dumps(stale), encoding="utf-8")
            counts = {"en": 2, "zh": 4, "fr": 3, "ja": 4, "ko": 2}
            with self.assertRaises(ev.EvaluationError):
                ev.load_manifest(stale_path, CASES_DIR,
                                 ev.sha256_bytes(BINDINGS_PATH.read_bytes()), counts)

    def test_negative_rules(self):
        duplicate = mutated_case(id="en-prefix-hel-0001")
        negatives = [
            ("duplicate id", [duplicate, duplicate]),
            ("unknown field", [mutated_case(unexpected_field=1)]),
            ("id charset", [mutated_case(id="bad id!")]),
            ("id length", [mutated_case(id="x" * 129)]),
            ("wrong language", [mutated_case(language="fr")]),
            ("undeclared scheme", [mutated_case(scheme="klingon")]),
            ("scheme not owned by binding", [mutated_case(scheme="pinyin")]),
            ("zero profile id", [mutated_case(profile_id=0)]),
            ("bool profile id", [mutated_case(profile_id=True)]),
            ("unknown binding", [mutated_case(profile_id=999)]),
            ("binding owns another language", [mutated_case(profile_id=3)]),
            ("binding owns another scheme", [mutated_case(profile_id=2)]),
            ("empty raw_input", [mutated_case(raw_input="")]),
            ("control scalar in raw_input", [mutated_case(raw_input="he\tl")]),
            ("raw_input byte bound", [mutated_case(raw_input="x" * 4097)]),
            ("escaped unicode in raw_input", [mutated_case(raw_input="h\\u00e9l")]),
            ("empty expected", [mutated_case(expected=[])]),
            ("too many surfaces", [mutated_case(expected=["a"] * 9)]),
            ("duplicate surfaces", [mutated_case(expected=["hello", "hello"])]),
            ("empty expected_languages", [mutated_case(expected_languages=[])]),
            ("expected_languages misses the case language", [mutated_case(expected_languages=["fr"])]),
            ("undeclared expected language", [mutated_case(expected_languages=["en", "es"])]),
            ("empty group_id", [mutated_case(group_id="")]),
            ("missing group_id", [mutated_case(group_id=DELETE)]),
            ("non-test split", [mutated_case(split="train")]),
            ("empty source", [mutated_case(source="")]),
            ("empty license", [mutated_case(license="")]),
            ("attribution not an object", [mutated_case(attribution="EpoCanva")]),
            ("empty tag", [mutated_case(tags=["ok", ""])]),
            ("context token bound", [mutated_case(context=["a", "b", "c"])]),
            ("control scalar in context", [mutated_case(context=["a\nb"])]),
        ]
        for name, cases in negatives:
            with self.subTest(rule=name):
                with self.assertRaises(ev.EvaluationError):
                    self.validate(cases)

    def test_positive_mutation_controls(self):
        # Near-misses that must be ACCEPTED, proving the negative tests fail for
        # the reason they claim and not because the validator is closed.
        self.validate([mutated_case(profile_id=1)])                     # valid binding
        self.validate([mutated_case(expected_languages=["en", "fr"])])  # superset is legal
        self.validate([mutated_case(raw_input="x" * 4096)])             # exactly at the bound


class CorpusFileTests(unittest.TestCase):
    def test_rejects_bom(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "en.jsonl"
            path.write_bytes(b"\xef\xbb\xbf" + json.dumps(BASE_CASE).encode("utf-8") + b"\n")
            with self.assertRaises(ev.EvaluationError):
                ev.read_corpus_file(path, "en")

    def test_rejects_crlf(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "en.jsonl"
            path.write_bytes(json.dumps(BASE_CASE).encode("utf-8") + b"\r\n")
            with self.assertRaises(ev.EvaluationError):
                ev.read_corpus_file(path, "en")

    def test_rejects_blank_line(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "en.jsonl"
            payload = json.dumps(BASE_CASE).encode("utf-8")
            path.write_bytes(payload + b"\n\n")
            with self.assertRaises(ev.EvaluationError):
                ev.read_corpus_file(path, "en")

    def test_rejects_non_json_line(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "en.jsonl"
            path.write_bytes(b"not json\n")
            with self.assertRaises(ev.EvaluationError):
                ev.read_corpus_file(path, "en")

    def test_rejects_non_object_line(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "en.jsonl"
            path.write_bytes(b"[1,2]\n")
            with self.assertRaises(ev.EvaluationError):
                ev.read_corpus_file(path, "en")

    def test_trailing_newline_and_last_record_accepted(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "en.jsonl"
            path.write_bytes(json.dumps(BASE_CASE).encode("utf-8") + b"\n")
            records = ev.read_corpus_file(path, "en")
            self.assertEqual(len(records), 1)


METRIC_CASE = mutated_case(raw_input="goo", expected=["good", "goodness"])


def repeat(rows, latency_ns=1_000_000, complete=True):
    return {"latency_ns": latency_ns, "search_complete": complete,
            "candidates": rows}


def row(kind, text, ids=(), begin=0, end=3):
    return {"rank": 0, "kind": kind, "text_utf8": text, "lexeme_ids": list(ids),
            "input_begin_utf8_byte": begin, "input_end_utf8_byte": end}


class MetricVectorTests(unittest.TestCase):
    def test_raw_first_contract_vector(self):
        # Menu: raw "goo" at ordinal 0, then "good", "goodness".
        record = {"status": "ok", "repeats": [repeat([
            row(0, "goo"), row(1, "good", ids=[11]), row(1, "goodness", ids=[12]),
        ])]}
        values = ev.metric_values(METRIC_CASE, record)
        self.assertEqual(values["top1"], 0.0)            # rank 1 is the raw text
        self.assertEqual(values["top3"], 1.0)            # "good" sits inside the first three
        self.assertEqual(values["mrr"], 0.5)             # "good" is at rank 2
        self.assertEqual(values["suggestion_top1"], 1.0)
        self.assertEqual(values["suggestion_top3"], 1.0)
        self.assertEqual(values["suggestion_mrr"], 1.0)
        self.assertEqual(values["raw_present"], 1.0)
        self.assertEqual(values["raw_first"], 1.0)
        self.assertEqual(values["empty_menu"], 0.0)
        self.assertEqual(values["wrong_language"], 0.0)
        self.assertEqual(values["latency_ms"], 1.0)

    def test_missed_target_vector(self):
        record = {"status": "ok", "repeats": [repeat([row(0, "goo"), row(1, "goose")])]}
        values = ev.metric_values(METRIC_CASE, record)
        self.assertEqual(values["top1"], 0.0)
        self.assertEqual(values["top3"], 0.0)
        self.assertEqual(values["mrr"], 0.0)
        self.assertEqual(values["suggestion_mrr"], 0.0)

    def test_raw_missing_vector(self):
        record = {"status": "ok", "repeats": [repeat([row(1, "good", ids=[11])])]}
        values = ev.metric_values(METRIC_CASE, record)
        self.assertEqual(values["raw_present"], 0.0)
        self.assertEqual(values["raw_first"], 0.0)

    def test_raw_present_but_not_first_vector(self):
        record = {"status": "ok", "repeats": [repeat([row(1, "good", ids=[11]), row(0, "goo")])]}
        values = ev.metric_values(METRIC_CASE, record)
        self.assertEqual(values["raw_present"], 1.0)
        self.assertEqual(values["raw_first"], 0.0)

    def test_empty_menu_vector(self):
        record = {"status": "ok", "repeats": [repeat([])]}
        values = ev.metric_values(METRIC_CASE, record)
        self.assertEqual(values["empty_menu"], 1.0)
        self.assertEqual(values["top1"], 0.0)

    def test_exact_raw_match_required(self):
        record = {"status": "ok", "repeats": [repeat([row(0, "go"), row(1, "good", ids=[11])])]}
        values = ev.metric_values(METRIC_CASE, record)
        self.assertEqual(values["raw_present"], 0.0)


class HelperTests(unittest.TestCase):
    def test_optional_ratio_zero_denominator_is_null(self):
        self.assertIsNone(ev.optional_ratio(0, 0))
        self.assertEqual(ev.optional_ratio(1, 2), 0.5)

    def test_percentile_pinned(self):
        values = [1.0, 2.0, 3.0, 4.0, 5.0]
        self.assertEqual(ev.percentile(values, 0.50), 3.0)
        self.assertEqual(ev.percentile(values, 0.95), 5.0)
        self.assertEqual(ev.percentile(values, 0.99), 5.0)

    def test_percentile_rejects_empty(self):
        with self.assertRaises(ev.EvaluationError):
            ev.percentile([], 0.5)

    def test_menu_signature_covers_order_and_fields(self):
        first = repeat([row(0, "goo"), row(1, "good", ids=[11])])
        same = repeat([row(0, "goo"), row(1, "good", ids=[11])])
        reordered = repeat([row(1, "good", ids=[11]), row(0, "goo")])
        different_ids = repeat([row(0, "goo"), row(1, "good", ids=[12])])
        self.assertEqual(ev.menu_signature(first), ev.menu_signature(same))
        self.assertNotEqual(ev.menu_signature(first), ev.menu_signature(reordered))
        self.assertNotEqual(ev.menu_signature(first), ev.menu_signature(different_ids))

    def test_grouped_bootstrap_is_deterministic_and_pinned(self):
        cases = [mutated_case(id=f"en-case-{ordinal}", group_id=f"group-{ordinal % 2}")
                 for ordinal in range(6)]
        values_by_id = {case["id"]: {"top1": 1.0 if ordinal % 2 == 0 else 0.0,
                                     "top3": 1.0, "mrr": 0.5,
                                     "suggestion_top1": 1.0, "suggestion_top3": 1.0,
                                     "suggestion_mrr": 1.0}
                        for ordinal, case in enumerate(cases)}
        first = ev.grouped_bootstrap(cases, values_by_id, ("top1",), seed=7, replicates=1000)
        second = ev.grouped_bootstrap(cases, values_by_id, ("top1",), seed=7, replicates=1000)
        self.assertEqual(first, second)
        low, high = first["top1"]
        self.assertLessEqual(low, 0.5 + 1e-9)
        self.assertGreaterEqual(high, 0.5 - 1e-9)
        with self.assertRaises(ev.EvaluationError):
            ev.grouped_bootstrap(cases, values_by_id, ("top1",), seed=7, replicates=99)

    def test_aggregate_uses_only_measured_cases(self):
        cases = [mutated_case(id="en-a"), mutated_case(id="en-b")]
        values_by_id = {"en-a": {"top1": 1.0, "top3": 1.0, "mrr": 1.0,
                                 "suggestion_top1": 1.0, "suggestion_top3": 1.0,
                                 "suggestion_mrr": 1.0}}
        result = ev.aggregate(cases, values_by_id, seed=7, replicates=100)
        self.assertEqual(result["top1"], 1.0)

    def test_output_path_rules(self):
        ev.require_safe_output_path(REPO_ROOT / "build" / "candidate-eval" / "report.json",
                                    REPO_ROOT)
        ev.require_safe_output_path(Path(tempfile.gettempdir()) / "report.json", REPO_ROOT)
        with self.assertRaises(ev.EvaluationError):
            ev.require_safe_output_path(REPO_ROOT / "docs" / "report.json", REPO_ROOT)


class ResultsReaderTests(unittest.TestCase):
    def test_read_results_round_trip(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "results.jsonl"
            meta = {"language": "en", "repeat": 2, "case_count": 1, "load_ms": 1.0,
                    "peak_working_set_bytes": 2}
            payload = (json.dumps({"run_meta": meta}) + "\n" +
                       json.dumps({"case_id": "en-a", "profile_id": 1, "status": "ok",
                                   "repeats": [repeat([row(0, "goo")]), repeat([row(0, "goo")])]}) + "\n")
            path.write_text(payload, encoding="utf-8")
            read_meta, records = ev.read_results(path)
            self.assertEqual(read_meta["language"], "en")
            self.assertIn("en-a", records)

    def test_read_results_rejects_missing_meta(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "results.jsonl"
            path.write_text(json.dumps({"case_id": "en-a"}) + "\n", encoding="utf-8")
            with self.assertRaises(ev.EvaluationError):
                ev.read_results(path)

    def test_read_results_rejects_duplicate_case(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "results.jsonl"
            line = json.dumps({"case_id": "en-a", "status": "ok", "repeats": []})
            path.write_text(json.dumps({"run_meta": {}}) + "\n" + line + "\n" + line + "\n",
                            encoding="utf-8")
            with self.assertRaises(ev.EvaluationError):
                ev.read_results(path)


class ReportWriterTests(unittest.TestCase):
    def test_report_bytes_are_deterministic(self):
        report = {"b": 1, "a": {"c": "你"}, "d": [1, 2]}
        with tempfile.TemporaryDirectory() as temporary:
            first = Path(temporary) / "first.json"
            second = Path(temporary) / "second.json"
            ev.save_report(first, report)
            ev.save_report(second, report)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            self.assertTrue(first.read_bytes().endswith(b"\n"))
            self.assertIn('"a"', first.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
