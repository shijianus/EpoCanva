"""Independent known-vector tests for the offline n-gram training pipeline."""
from __future__ import annotations

import hashlib
import importlib.util
import json
import sys
import tempfile
import unittest
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TRAINER_PATH = ROOT / "tools" / "backend" / "train_ngram.py"
EVALUATOR_PATH = ROOT / "tools" / "backend" / "evaluate_predictions.py"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load module from {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


trainer = load_module("epocanva_train_ngram_tests", TRAINER_PATH)
evaluator = load_module("epocanva_evaluate_predictions_tests", EVALUATOR_PATH)


class TokenizerTests(unittest.TestCase):
    def test_english_french_unicode_and_contractors(self):
        self.assertEqual(
            trainer.tokenize("We're re-entering déjà vu.", "en"),
            [("we're", "word"), ("re-entering", "word"), ("déjà", "word"),
             ("vu", "word"), (".", "punct")],
        )
        self.assertEqual(trainer.tokenize("L'école française!", "fr"),
                         [("l'école", "word"), ("française", "word"), ("!", "punct")])

    def test_ascii_lower_only_not_casefold_or_nfc(self):
        self.assertEqual(trainer.ascii_lower("CAFÉ ÉCOLE İSTANBUL"), "cafÉ École İstanbul")
        self.assertEqual(trainer.ascii_lower("e\u0301"), "e\u0301")
        self.assertNotEqual(trainer.ascii_lower("é"), trainer.ascii_lower("e\u0301"))

    def test_korean_spacing_words(self):
        self.assertEqual(trainer.tokenize("안녕하세요 세상!", "ko"),
                         [("안녕하세요", "word"), ("세상", "word"), ("!", "punct")])

    def test_chinese_japanese_fallback_and_catalog_longest_match(self):
        self.assertEqual(trainer.tokenize("中文。", "zh"),
                         [("中", "word"), ("文", "word"), ("。", "punct")])
        self.assertEqual(trainer.tokenize("東京都市", "ja", {"ja": {"東京", "東京都", "都市"}}),
                         [("東京都", "word"), ("市", "word")])

    def test_token_controls_and_long_word_are_rejected(self):
        with self.assertRaises(trainer.TrainingError):
            trainer.tokenize("a\x00b", "en")
        with self.assertRaises(trainer.TrainingError):
            trainer.validate_lexeme("é" * 129)


class StableIdAndSplitTests(unittest.TestCase):
    def test_independent_fnv1a64_vectors(self):
        vectors = {
            ("en", "red"): 11499352395868389699,
            ("en", "blue"): 15663391281168816312,
            ("en", "green"): 6752004103742717471,
            ("en", "hello"): 5840500155624144132,
            ("fr", "École"): 8204529717888454200,
            ("fr", "école"): 9808910016276955800,
            ("zh", "中文"): 1409937755432690307,
            ("ja", "日本語"): 1886492069840619542,
            ("ko", "안녕"): 13064592397352898903,
        }
        for pair, expected in vectors.items():
            with self.subTest(pair=pair):
                self.assertEqual(trainer.stable_lexeme_id(*pair), expected)

    def test_document_split_hash_known_vectors(self):
        expected = {"fixture-group-0": "train", "fixture-group-2": "dev",
                    "fixture-group-7": "test"}
        for group, split in expected.items():
            with self.subTest(group=group):
                self.assertEqual(trainer.split_for_group(group, "fixture-v1", 80, 10), split)

    def test_group_split_prevents_group_leakage(self):
        records = [{"group": "same-doc", "id": "a"},
                   {"group": "same-doc", "id": "b"}]
        result = trainer.assign_splits(records, "fixture-v1")
        assignments = [record["split"] for rows in result.values() for record in rows]
        self.assertEqual(len(set(assignments)), 1)


class TrainingContractTests(unittest.TestCase):
    def _record(self, record_id: str, tokens: list[tuple[str, str]]):
        return {"id": record_id, "language": "en", "tokens": tokens}

    def test_counts_target_eos_without_eos_history_or_bos(self):
        records = [self._record("a", [("red", "word"), ("blue", "word"), ("green", "word")])]
        lexemes, unigrams, bigrams, trigrams, token_counts = trainer.count_ngrams(records, 10, 100)
        ids = {text: lexeme_id for lexeme_id, (_lang, _kind, text) in lexemes.items()}
        self.assertEqual(unigrams[0], 1)
        self.assertEqual(unigrams[ids["red"]], 1)
        self.assertIn((ids["green"], 0), bigrams)
        self.assertNotIn((0, ids["red"]), bigrams)
        self.assertIn((ids["red"], ids["blue"], ids["green"]), trigrams)
        self.assertNotIn((0, ids["red"], ids["blue"]), trigrams)
        self.assertEqual(token_counts, {"en": 3})
        self.assertNotIn(0, lexemes)

    def test_model_serialization_has_exact_header_hashes_and_numeric_sort(self):
        records = [self._record("a", [("red", "word"), ("blue", "word"), ("green", "word")])]
        lexemes, unigrams, bigrams, trigrams, _ = trainer.count_ngrams(records, 10, 100)
        source_sha = "1" * 64
        model, stats = trainer.model_bytes_for_source(
            lexemes, unigrams, bigrams, trigrams, "test-fixture", source_sha,
            trainer.MAX_MODEL_BYTES)
        header, payload = model.split(b"data\n", 1)
        fields = dict(line.split(b"\t", 1) for line in header.splitlines())
        self.assertEqual(fields[b"EPOCANVA_PREDICTION_TSV"], b"1")
        self.assertEqual(fields[b"id_scheme"], b"fnv1a64-language-normalized-v1")
        self.assertEqual(fields[b"normalization"], b"ascii-lower-utf8-v1")
        self.assertEqual(fields[b"source"], b"test-fixture")
        self.assertEqual(fields[b"source_sha256"], source_sha.encode())
        self.assertEqual(fields[b"payload_sha256"], hashlib.sha256(payload).hexdigest().encode())
        self.assertEqual(stats["model_bytes"], len(model))
        groups = [line[:1] for line in payload.splitlines()]
        self.assertEqual(groups, sorted(groups, key=lambda value: b"LUBT".index(value)))
        lrows = [int(line.split(b"\t")[1]) for line in payload.splitlines() if line.startswith(b"L\t")]
        self.assertEqual(lrows, sorted(lrows))

    def test_model_budget_prunes_deterministically_and_preserves_marginals(self):
        records = []
        for index in range(50):
            tokens = [(f"word{index}", "word"), (f"shared{index % 5}", "word"),
                      (f"tail{index % 7}", "word")]
            records.append(self._record(str(index), tokens))
        lexemes, unigrams, bigrams, trigrams, _ = trainer.count_ngrams(records, 1000, 10000)
        first, stats1 = trainer.model_bytes_for_source(lexemes, unigrams, bigrams, trigrams,
                                                       "test-fixture", "2" * 64, 4096)
        second, stats2 = trainer.model_bytes_for_source(lexemes, unigrams, bigrams, trigrams,
                                                        "test-fixture", "2" * 64, 4096)
        self.assertLessEqual(len(first), 4096)
        self.assertEqual(first, second)
        self.assertEqual(stats1, stats2)
        self.assertTrue(any(stats1["pruned"].values()))
        lines = first.split(b"data\n", 1)[1].splitlines()
        self.assertIn(b"U\t0\t50", lines)
        self.assertEqual(sum(1 for line in lines if line.startswith(b"L\t")), stats1["lexemes"])

    def test_grouped_split_provenance_is_stable(self):
        records = [{"canonical": {"id": "1"}}, {"canonical": {"id": "2"}}]
        one = trainer.split_hash(records)
        self.assertEqual(one, trainer.split_hash(records))
        self.assertEqual(len(one), 64)

    def test_eval_cases_have_exact_tsv_context_and_gold_target(self):
        record = {"id": "s-1", "language": "en", "group": "author-1",
                  "tokens": [("one", "word"), ("two", "word"), ("three", "word")]}
        cases = trainer.eval_cases([record])
        self.assertEqual(cases[0]["target_text"], "one")
        self.assertEqual(cases[2]["context"], ["one", "two"])
        self.assertEqual(cases[2]["target_text"], "three")
        self.assertEqual(cases[2]["group_id"], "author-1")
        self.assertEqual(trainer.eval_requests(cases).decode("utf-8").splitlines()[2],
                         "en:s-1:2\ten\tone\ttwo\t")

    def test_min_count_pruning_fixed_vector_retains_denominators(self):
        bigrams = Counter({(10, 20): 3, (20, 30): 2, (20, 40): 1,
                           (30, 0): 2, (40, 0): 1})
        trigrams = Counter({(10, 20, 30): 2, (10, 20, 40): 1,
                            (20, 30, 0): 2, (20, 40, 0): 1})
        kept_b, kept_t = trainer.prune_min_ngram_count(bigrams, trigrams, 2)
        self.assertEqual(kept_b, {(10, 20): 3, (20, 30): 2, (30, 0): 2})
        self.assertEqual(kept_t, {(10, 20, 30): 2, (20, 30, 0): 2})
        self.assertEqual(bigrams[(20, 40)], 1)
        with self.assertRaises(trainer.TrainingError):
            trainer.prune_min_ngram_count(bigrams, trigrams, 0)

    def test_dedup_removes_normalized_duplicates_before_split(self):
        def record(record_id, group, tokens):
            return {"id": record_id, "language": "en", "source": "fixture",
                    "license": "test-only", "group": group, "tokens": tokens}
        records = [record("b", "fixture-group-7", [("same", "word")]),
                   record("a", "fixture-group-0", [("same", "word")]),
                   record("c", "fixture-group-2", [("different", "word")])]
        deduped, removed = trainer.deduplicate_records(records)
        self.assertEqual(removed, 1)
        self.assertEqual([record["id"] for record in deduped], ["a", "c"])
        split = trainer.assign_splits(deduped, "fixture-v1")
        self.assertEqual([r["id"] for r in split["train"]], ["a"])
        self.assertEqual([r["id"] for r in split["dev"]], ["c"])
        self.assertEqual(split["test"], [])

    def test_structured_attribution_and_explicit_whole_record_exclusion(self):
        artifact_root = ROOT / "build" / "backend-training"
        artifact_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=artifact_root) as temp:
            path = Path(temp) / "corpus.jsonl"
            attribution = {"author": "fixture-author", "sentence_url": "fixture:1",
                           "license_url": "fixture:license"}
            rows = [{"id": "good", "language": "en", "text": "good words",
                     "source": "fixture", "license": "test-only", "document_id": "doc-a",
                     "attribution": attribution},
                    {"id": "bad", "language": "en", "text": "bad\u200bwords",
                     "source": "fixture", "license": "test-only", "document_id": "doc-b",
                     "attribution": attribution}]
            path.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")
            limits = dict(max_bytes=10000, max_records=10, max_tokens=10, max_lexemes=10)
            with self.assertRaises(trainer.TrainingError):
                trainer.read_corpus([path], {}, **limits)
            records, _digest, excluded = trainer.read_corpus([path], {}, skip_invalid=True, **limits)
            self.assertEqual([row["id"] for row in records], ["good"])
            self.assertEqual(records[0]["canonical"]["attribution"], attribution)
            self.assertEqual(excluded, {"non_whitespace_control_or_format": 1})

    def test_production_requires_real_corpus_scale(self):
        artifact_root = ROOT / "build" / "backend-training"
        artifact_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=artifact_root) as temp:
            path = Path(temp) / "tiny.jsonl"
            row = {"id": "fixture", "language": "en", "text": "actual words",
                   "source": "licensed-corpus", "license": "CC-BY", "attribution": "author",
                   "document_id": "fixture-document"}
            path.write_text(json.dumps(row) + "\n", encoding="utf-8")
            args = trainer.build_parser().parse_args([
                "--input", str(path), "--source-kind", "production-corpus",
                "--output", str(Path(temp) / "model.tsv"), "--train-percent", "80",
                "--dev-percent", "10", "--seed", "fixture-v1"])
            with self.assertRaisesRegex(trainer.TrainingError, "at least 1001"):
                trainer.train(args)


class EvaluationMetricTests(unittest.TestCase):
    def setUp(self):
        self.cases = [
            {"id": "one", "language": "en", "target_text": "a", "context": [],
             "split": "test", "group_id": "doc-1"},
            {"id": "two", "language": "en", "target_text": "b", "context": ["a"],
             "split": "test", "group_id": "doc-2"},
            {"id": "three", "language": "en", "target_text": "c", "context": ["a", "b"],
             "split": "test", "group_id": "doc-2"},
        ]

    def test_known_hit_metrics_and_coverage(self):
        ranked = {"one": ["a"], "two": ["x", "b"], "three": []}
        metrics = evaluator.rank_metrics(self.cases, ranked)
        self.assertEqual(metrics["examples"], 3)
        self.assertEqual(metrics["coverage"], 1.0)
        self.assertAlmostEqual(metrics["hit_at_1"], 1 / 3)
        self.assertAlmostEqual(metrics["hit_at_3"], 2 / 3)
        self.assertAlmostEqual(metrics["hit_at_5"], 2 / 3)
        self.assertAlmostEqual(metrics["mrr"], 1 / 2)

    def test_missing_rows_and_oov_are_misses_not_deleted(self):
        metrics = evaluator.rank_metrics(self.cases, {"one": ["a"]})
        self.assertEqual(metrics["missing_predictions"], 2)
        self.assertAlmostEqual(metrics["coverage"], 1 / 3)
        self.assertAlmostEqual(metrics["hit_at_1"], 1 / 3)

    def test_grouped_bootstrap_is_deterministic_and_reports_delta(self):
        model = {"one": ["a"], "two": ["b"], "three": []}
        baseline = {"one": ["x"], "two": ["x"], "three": ["c"]}
        first = evaluator.bootstrap_intervals(self.cases, model, baseline,
                                              seed=73, replicates=200)
        second = evaluator.bootstrap_intervals(self.cases, model, baseline,
                                               seed=73, replicates=200)
        self.assertEqual(first, second)
        self.assertIn("delta.hit_at_1", first)
        self.assertLessEqual(first["delta.hit_at_1"]["low_95"],
                             first["delta.hit_at_1"]["high_95"])

    def test_backend_status_complete_and_language_gates_quality(self):
        rows = []
        for case in self.cases:
            rows.append({"id": case["id"], "language": case["language"], "ranked": [],
                         "latency_ms": 0.25, "status": "ok", "complete": True})
        predictions, stats = evaluator.validate_predictions(rows, self.cases, "fixture")
        self.assertEqual(len(predictions), 3)
        self.assertTrue(stats["quality_eligible"])
        rows[0]["status"] = "budget-exhausted"
        _, failed = evaluator.validate_predictions(rows, self.cases, "fixture")
        self.assertFalse(failed["quality_eligible"])
        rows[0]["status"] = "ok"
        rows[1]["complete"] = False
        _, incomplete = evaluator.validate_predictions(rows, self.cases, "fixture")
        self.assertFalse(incomplete["quality_eligible"])

    def test_result_rejects_unknown_duplicate_or_language_mismatch(self):
        base = {"id": "one", "language": "en", "ranked": [], "status": "ok", "complete": True}
        with self.assertRaises(evaluator.EvaluationError):
            evaluator.validate_predictions([base, base], self.cases, "fixture")
        wrong = dict(base, language="fr")
        _, stats = evaluator.validate_predictions([wrong], self.cases, "fixture")
        self.assertFalse(stats["quality_eligible"])

    def test_claim_is_withheld_for_small_test_and_backend_integrity(self):
        results = {case["id"]: [] for case in self.cases}
        integrity = {"quality_eligible": True}
        report = evaluator.evaluate(self.cases, results, results, integrity,
                                   integrity, seed=1, replicates=100, min_examples=4)
        self.assertFalse(report["quality_claim_eligible"])
        self.assertEqual(report["languages_below_minimum"], ["en"])
        failed_integrity = dict(integrity, quality_eligible=False)
        report = evaluator.evaluate(self.cases, results, results, failed_integrity,
                                   integrity, seed=1, replicates=100, min_examples=3)
        self.assertFalse(report["quality_claim_eligible"])


if __name__ == "__main__":
    unittest.main()
