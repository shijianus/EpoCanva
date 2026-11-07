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
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import uuid
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


def fixture_git_repo(name: str) -> Path:
    """Create a real git checkout under build/test-tmp (gitignored) for the
    source-identity tests; the evaluator's git calls then bind to a fixture
    whose history this test authored, never to the working repository."""
    root = REPO_ROOT / "build" / "test-tmp" / f"{name}-{os.getpid()}-{uuid.uuid4().hex[:8]}"
    root.mkdir(parents=True)
    return root


def fixture_git(root: Path, *arguments: str) -> str:
    environment = dict(os.environ,
                       GIT_CONFIG_GLOBAL=str(root / "gitconfig"),
                       GIT_CONFIG_SYSTEM=str(root / "gitconfig"))
    completed = subprocess.run(
        ["git", "-C", str(root), *arguments], stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace",
        timeout=120, check=False, env=environment)
    if completed.returncode != 0:
        raise AssertionError(f"fixture git {' '.join(arguments)} failed: {completed.stderr}")
    return completed.stdout.strip()


def init_fixture_commit(root: Path) -> str:
    (root / "source.txt").write_bytes(b"fixture source\n")
    fixture_git(root, "init")
    fixture_git(root, "add", "source.txt")
    fixture_git(root, "-c", "user.name=fixture", "-c", "user.email=fixture@example.invalid",
                "commit", "-m", "fixture")
    return fixture_git(root, "rev-parse", "HEAD")


@unittest.skipUnless(shutil.which("git"), "git is required for the identity fixtures")
class SourceIdentityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = fixture_git_repo("identity")
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def test_clean_repo_binds_the_real_head(self) -> None:
        head = init_fixture_commit(self.root)
        source = ev.resolve_source_identity(self.root)
        self.assertEqual(source["repo_commit"], head)
        self.assertTrue(source["source_tree_clean"])
        self.assertEqual(source["source_ref"],
                         fixture_git(self.root, "symbolic-ref", "--short", "HEAD"))
        ev.check_source_identity(source, head, "qualification")  # must not raise

    def test_detached_head_records_an_empty_ref_not_a_fabricated_one(self) -> None:
        init_fixture_commit(self.root)
        fixture_git(self.root, "checkout", "--detach")
        source = ev.resolve_source_identity(self.root)
        self.assertEqual(source["source_ref"], "")

    def test_expected_commit_mismatch_fails_in_both_modes(self) -> None:
        head = init_fixture_commit(self.root)
        source = ev.resolve_source_identity(self.root)
        other = "f" * 40
        self.assertNotEqual(other, head)
        for mode in ("baseline", "qualification"):
            with self.subTest(mode=mode):
                with self.assertRaises(ev.EvaluationError):
                    ev.check_source_identity(source, other, mode)

    def test_dirty_tracked_tree_fails_qualification(self) -> None:
        init_fixture_commit(self.root)
        (self.root / "source.txt").write_bytes(b"fixture source modified\n")
        source = ev.resolve_source_identity(self.root)
        self.assertFalse(source["source_tree_clean"])
        with self.assertRaises(ev.EvaluationError):
            ev.check_source_identity(source, source["repo_commit"], "qualification")

    def test_staged_tree_fails_qualification(self) -> None:
        init_fixture_commit(self.root)
        (self.root / "source.txt").write_bytes(b"staged change\n")
        fixture_git(self.root, "add", "source.txt")
        source = ev.resolve_source_identity(self.root)
        self.assertFalse(source["source_tree_clean"])
        with self.assertRaises(ev.EvaluationError):
            ev.check_source_identity(source, source["repo_commit"], "qualification")

    def test_untracked_file_fails_qualification(self) -> None:
        init_fixture_commit(self.root)
        (self.root / "untracked.txt").write_bytes(b"untracked\n")
        source = ev.resolve_source_identity(self.root)
        self.assertFalse(source["source_tree_clean"])
        with self.assertRaises(ev.EvaluationError):
            ev.check_source_identity(source, source["repo_commit"], "qualification")

    def test_baseline_records_dirtiness_without_failing(self) -> None:
        init_fixture_commit(self.root)
        (self.root / "source.txt").write_bytes(b"dirty\n")
        source = ev.resolve_source_identity(self.root)
        ev.check_source_identity(source, source["repo_commit"], "baseline")  # no raise
        self.assertFalse(source["source_tree_clean"])

    def test_non_git_directory_fails(self) -> None:
        plain = self.root / "plain"
        plain.mkdir()
        with self.assertRaises(ev.EvaluationError):
            ev.resolve_source_identity(plain)


MODEL_ROWS = (
    "epocanva-backend-resources\t1\n"
    "profile_id\tlanguage\tinput_mode\toutput_mode\tkeyboard_layout\tvariant_id\t"
    "model_path\tmodel_sha256\tconversion_path\tconversion_sha256\tauxiliary_path\t"
    "auxiliary_sha256\tabbreviation_path\tabbreviation_sha256\n"
)


def model_row(profile: int, language: int, sha: str) -> str:
    return (f"{profile}\t{language}\t1\t1\t1\tx\tdata/m.tsv\t{sha}\t-\t-\t-\t-\t-\t-\n")


class StagedModelMapTests(unittest.TestCase):
    def test_shared_models_map_per_language_and_per_profile(self) -> None:
        sha_en, sha_zh = "a" * 64, "b" * 64
        index = MODEL_ROWS + model_row(1, 1, sha_en) + model_row(2, 2, sha_zh) \
            + model_row(6, 2, sha_zh)
        per_language, by_profile, notes = ev.read_staged_model_maps(index.encode("utf-8"))
        self.assertEqual(per_language, {"en": sha_en, "zh": sha_zh})
        self.assertEqual(by_profile, {"1": sha_en, "2": sha_zh, "6": sha_zh})
        self.assertEqual(notes, {})

    def test_disagreeing_profiles_never_inherit_the_first_hash(self) -> None:
        first, second = "a" * 64, "b" * 64
        index = MODEL_ROWS + model_row(1, 1, first) + model_row(11, 1, second)
        per_language, by_profile, notes = ev.read_staged_model_maps(index.encode("utf-8"))
        self.assertIsNone(per_language["en"])
        self.assertEqual(by_profile, {"1": first, "11": second})
        self.assertIn("en", notes)

    def test_duplicate_profile_id_is_rejected(self) -> None:
        index = MODEL_ROWS + model_row(1, 1, "a" * 64) + model_row(1, 1, "a" * 64)
        with self.assertRaises(ev.EvaluationError):
            ev.read_staged_model_maps(index.encode("utf-8"))

    def test_malformed_model_digest_is_rejected(self) -> None:
        index = MODEL_ROWS + model_row(1, 1, "ZZ" * 32)
        with self.assertRaises(ev.EvaluationError):
            ev.read_staged_model_maps(index.encode("utf-8"))

    def test_malformed_row_is_rejected(self) -> None:
        index = MODEL_ROWS + "1\t1\ttoofew\n"
        with self.assertRaises(ev.EvaluationError):
            ev.read_staged_model_maps(index.encode("utf-8"))

    def test_unknown_language_number_is_rejected(self) -> None:
        index = MODEL_ROWS + model_row(1, 9, "a" * 64)
        with self.assertRaises(ev.EvaluationError):
            ev.read_staged_model_maps(index.encode("utf-8"))


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
        # A literal backslash followed by 'u' is ordinary text data; the \uXXXX
        # ban is lexical on the raw JSON bytes, not a decoded-substring rule.
        self.validate([mutated_case(raw_input="h\\u00e9l")])


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

    def test_rejects_literal_unicode_escape_in_raw_bytes(self):
        # Integration negative: the escape is real raw JSON bytes ("\u0068"),
        # exactly what json.loads would silently decode and the native parser
        # would reject; the evaluator must reject it before decoding.
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "en.jsonl"
            payload = json.dumps(BASE_CASE).encode("utf-8").replace(b'"hel"', b'"\\u0068"')
            self.assertIn(b'"\\u0068"', payload)  # the fixture is what it claims
            path.write_bytes(payload + b"\n")
            with self.assertRaises(ev.EvaluationError):
                ev.read_corpus_file(path, "en")

    def test_escaped_backslash_before_u_is_not_an_escape(self):
        # "\\u0068" in raw bytes is an escaped backslash followed by 'u'; it
        # decodes to a literal backslash-u text and must not false-positive.
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "en.jsonl"
            payload = json.dumps(BASE_CASE).encode("utf-8").replace(b'"hel"', b'"\\\\u0068"')
            self.assertIn(b'"\\\\u0068"', payload)
            path.write_bytes(payload + b"\n")
            records = ev.read_corpus_file(path, "en")
            self.assertEqual(records[0]["raw_input"], "\\u0068")
            seen: set[str] = set()
            ev.validate_cases("en", records, ev.load_bindings(BINDINGS_PATH), seen)


@unittest.skipUnless(os.name == "nt", "the fake harness fixture is a Windows batch file")
class HarnessStderrTests(unittest.TestCase):
    """run_harness integration: a harness that exits 0 with stderr output
    violates its own protocol and must be rejected even with a results file."""

    def setUp(self) -> None:
        self.root = REPO_ROOT / "build" / "test-tmp" / f"harness-{os.getpid()}-{uuid.uuid4().hex[:8]}"
        self.root.mkdir(parents=True)
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.output = self.root / "results.jsonl"
        self.cmd = Path(os.environ.get("ComSpec", r"C:\Windows\System32\cmd.exe"))

    def make_bat(self, body: str) -> Path:
        path = self.root / "fake-harness.cmd"
        path.write_bytes(body.encode("ascii"))
        return path

    def run_fake(self, body: str) -> dict:
        harness = self.make_bat(body)
        # run_harness inherits the process environment; point the fixture at
        # the expected results path for the duration of the call.
        original = os.environ.get("EPOCANVA_FAKE_OUT")
        os.environ["EPOCANVA_FAKE_OUT"] = str(self.output)
        try:
            return ev.run_harness(self.cmd, ["/c", str(harness)], self.output, 60)
        finally:
            if original is None:
                os.environ.pop("EPOCANVA_FAKE_OUT", None)
            else:
                os.environ["EPOCANVA_FAKE_OUT"] = original

    def test_silent_success_passes(self):
        command = self.run_fake("@echo off\r\ntype nul > \"%EPOCANVA_FAKE_OUT%\"\r\n")
        self.assertEqual(command["exit_status"], 0)
        self.assertTrue(command["stderr_empty"])
        self.assertTrue(self.output.is_file())

    def test_stderr_on_success_is_fatal(self):
        with self.assertRaises(ev.EvaluationError):
            self.run_fake("@echo off\r\n"
                          "echo unexpected diagnostic 1>&2\r\n"
                          "type nul > \"%EPOCANVA_FAKE_OUT%\"\r\n")


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
        self.assertNotIn("wrong_language", values)  # never claimed as 0.0
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


def gate_language(**overrides):
    """A per-language block for gate tests: every mandatory predicate healthy
    except wrong_language (structurally not observable at this ABI)."""
    values = {
        "cases": 1001,
        "measured_cases": 1001,
        "minimum_cases_met": True,
        "top1": 0.5, "top3": 0.6, "mrr": 0.4,
        "suggestion_top1": 0.5, "suggestion_top3": 0.6, "suggestion_mrr": 0.4,
        "ci95": {"top1": [0.4, 0.6]},
        "raw_present_rate": 1.0,
        "raw_first_rate": 1.0,
        "empty_menu_rate": 0.0,
        "wrong_language_rate": None,
        "in_process_repeat_deterministic_rate": 1.0,
        "cross_process_deterministic_rate": 1.0,
        "latency_ms": {"cold": 1.0,
                       "warm": {"p50": 1.0, "p95": 2.0, "p99": 3.0, "queries": 10},
                       "warmup_queries": 0},
        "resource_load_ms": 5.0,
        "peak_working_set_bytes": 1000,
    }
    values.update(overrides)
    return values


HEALTHY_PROTOCOL = {"en": {"fresh_runs": 2, "fresh_runs_expected": 2,
                           "repeat_counts": [2], "repeat_expected": 2}}
HEALTHY_IDS = {"en": {"en-a", "en-b"}}


def healthy_gate(**overrides):
    per_language = {"en": gate_language(**overrides)}
    return ev.evaluate_quality_gate(
        per_language=per_language, failures=[],
        expected_case_ids=HEALTHY_IDS, observed_case_ids=HEALTHY_IDS,
        protocol=HEALTHY_PROTOCOL)


class QualityGateTests(unittest.TestCase):
    """The fail-closed gate semantics: unmet or unevaluated mandatory
    predicates can never produce met == true (policy v1)."""

    def test_everything_measured_still_cannot_pass_at_this_abi(self):
        # wrong_language_observable is not evaluable on the v1 candidate ABI,
        # so the honest ceiling of policy v1 is not_evaluable, never passed.
        gate = healthy_gate()
        self.assertEqual(gate["status"], "not_evaluable")
        self.assertFalse(gate["met"])
        self.assertIn("wrong_language_observable",
                      [entry["predicate"] for entry in gate["not_evaluated_predicates"]])
        self.assertEqual(gate["unmet_reasons"], [])

    def test_single_validation_failure_blocks_passed(self):
        gate = ev.evaluate_quality_gate(
            per_language={"en": gate_language()},
            failures=[{"case_id": "en-a", "reason": "menu-not-deterministic"}],
            expected_case_ids=HEALTHY_IDS, observed_case_ids=HEALTHY_IDS,
            protocol=HEALTHY_PROTOCOL)
        self.assertEqual(gate["status"], "failed")
        self.assertFalse(gate["met"])
        self.assertIn("validation_failures_empty",
                      [entry["predicate"] for entry in gate["unmet_reasons"]])

    def test_determinism_below_one_blocks_passed(self):
        for key in ("cross_process_deterministic_rate", "in_process_repeat_deterministic_rate"):
            with self.subTest(key=key):
                gate = healthy_gate(**{key: 0.9})
                self.assertEqual(gate["status"], "failed")
                self.assertFalse(gate["met"])

    def test_empty_menu_blocks_passed(self):
        gate = healthy_gate(empty_menu_rate=0.1)
        self.assertEqual(gate["status"], "failed")
        self.assertIn("empty_menu_rate_zero",
                      [entry["predicate"] for entry in gate["unmet_reasons"]])

    def test_raw_contract_failure_blocks_passed(self):
        gate = healthy_gate(raw_present_rate=0.5, raw_first_rate=0.5)
        self.assertEqual(gate["status"], "failed")
        self.assertIn("raw_contract",
                      [entry["predicate"] for entry in gate["unmet_reasons"]])

    def test_insufficient_samples_block_pass_and_are_named(self):
        gate = healthy_gate(cases=15, measured_cases=15, minimum_cases_met=False)
        self.assertEqual(gate["status"], "failed")
        self.assertIn("minimum_samples",
                      [entry["predicate"] for entry in gate["unmet_reasons"]])
        self.assertIn("en: 15/1001 cases", gate["unmet_reasons"][0]["reason"])

    def test_missing_expected_case_blocks_passed(self):
        gate = ev.evaluate_quality_gate(
            per_language={"en": gate_language()}, failures=[],
            expected_case_ids=HEALTHY_IDS, observed_case_ids={"en": {"en-a"}},
            protocol=HEALTHY_PROTOCOL)
        self.assertEqual(gate["status"], "failed")
        self.assertIn("all_expected_cases_executed",
                      [entry["predicate"] for entry in gate["unmet_reasons"]])

    def test_extra_unexpected_case_blocks_passed(self):
        gate = ev.evaluate_quality_gate(
            per_language={"en": gate_language()}, failures=[],
            expected_case_ids=HEALTHY_IDS,
            observed_case_ids={"en": {"en-a", "en-b", "en-ghost"}},
            protocol=HEALTHY_PROTOCOL)
        self.assertEqual(gate["status"], "failed")
        self.assertIn("no_extra_cases",
                      [entry["predicate"] for entry in gate["unmet_reasons"]])

    def test_protocol_mismatch_blocks_passed(self):
        protocol = {"en": {"fresh_runs": 1, "fresh_runs_expected": 2,
                           "repeat_counts": [2, 3], "repeat_expected": 2}}
        gate = ev.evaluate_quality_gate(
            per_language={"en": gate_language()}, failures=[],
            expected_case_ids=HEALTHY_IDS, observed_case_ids=HEALTHY_IDS,
            protocol=protocol)
        self.assertEqual(gate["status"], "failed")
        self.assertIn("exact_process_and_repeat_protocol",
                      [entry["predicate"] for entry in gate["unmet_reasons"]])

    def test_baseline_records_the_same_gate_without_passing(self):
        # The report the baseline mode writes for the same evidence carries the
        # identical gate block: unmet reasons named, met still false.
        failing = healthy_gate(cases=15, minimum_cases_met=False)
        self.assertFalse(failing["met"])
        self.assertIn(failing["status"], ("failed", "not_evaluable"))
        self.assertTrue(failing["unmet_reasons"] or failing["not_evaluated_predicates"])

    def test_met_is_true_only_when_status_is_passed(self):
        for gate in (healthy_gate(), healthy_gate(cases=15),
                     healthy_gate(empty_menu_rate=0.1)):
            self.assertEqual(gate["met"], gate["status"] == "passed")


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
        # Candidate surfaces may land only under the resolved build/ tree.
        ev.require_contained_build_path(REPO_ROOT / "build" / "candidate-eval" / "report.json",
                                        REPO_ROOT)
        for forbidden in (
                REPO_ROOT / "docs" / "report.json",                 # source tree, not build/
                REPO_ROOT / "build" / ".." / "docs" / "evil.json",  # .. traversal
                Path(tempfile.gettempdir()) / "report.json",        # outside the repository
                REPO_ROOT / "buildx" / "report.json",               # prefix lookalike
        ):
            with self.subTest(path=str(forbidden)):
                with self.assertRaises(ev.EvaluationError):
                    ev.require_contained_build_path(forbidden, REPO_ROOT)

    def test_junction_escape_is_rejected(self):
        if os.name != "nt":
            self.skipTest("junction reparse points are a Windows behaviour")
        link = REPO_ROOT / "build" / "test-tmp" / f"junction-{os.getpid()}-{uuid.uuid4().hex[:8]}"
        link.parent.mkdir(parents=True, exist_ok=True)

        def remove_junction() -> None:
            try:
                os.rmdir(link)  # removes the reparse point itself, never the target
            except OSError:
                pass

        try:
            import _winapi
            _winapi.CreateJunction(str(REPO_ROOT / "docs"), str(link))
        except (ImportError, OSError):
            self.skipTest("creating a junction requires privileges this host lacks")
        self.addCleanup(remove_junction)
        with self.assertRaises(ev.EvaluationError):
            ev.require_contained_build_path(link / "report.json", REPO_ROOT)


class ResultsReaderTests(unittest.TestCase):
    @staticmethod
    def write_results(path: Path, meta_overrides: dict | None = None,
                      record_overrides: dict | None = None,
                      meta: dict | None = None) -> None:
        base_meta = {"language": "en", "repeat": 2, "case_count": 1, "load_ms": 1.0,
                     "peak_working_set_bytes": 2}
        if meta is None:
            meta = dict(base_meta)
            meta.update(meta_overrides or {})
        record = {"case_id": "en-a", "profile_id": 1, "status": "ok",
                  "repeats": [repeat([row(0, "goo")]), repeat([row(0, "goo")])]}
        record.update(record_overrides or {})
        if record is not None and record_overrides and record_overrides.get("repeats") is DELETE:
            record.pop("repeats")
        path.write_text(json.dumps({"run_meta": meta}) + "\n" + json.dumps(record) + "\n",
                        encoding="utf-8")

    def read(self, path: Path, **overrides):
        expectations = {"language": "en", "repeat": 2, "case_count": 1}
        expectations.update(overrides)
        return ev.read_results(path, **expectations)

    def test_read_results_round_trip(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "en.run0.jsonl"
            self.write_results(path)
            read_meta, records = self.read(path)
            self.assertEqual(read_meta["language"], "en")
            self.assertIn("en-a", records)

    def test_read_results_rejects_wrong_file_name_language(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "zh.run0.jsonl"
            self.write_results(path)
            with self.assertRaises(ev.EvaluationError):
                self.read(path)

    def test_read_results_rejects_missing_meta(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "en.run0.jsonl"
            path.write_text(json.dumps({"case_id": "en-a"}) + "\n", encoding="utf-8")
            with self.assertRaises(ev.EvaluationError):
                self.read(path)

    def test_read_results_rejects_duplicate_case(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "en.run0.jsonl"
            line = json.dumps({"case_id": "en-a", "status": "ok", "repeats": []})
            path.write_text(json.dumps({"run_meta": {"language": "en", "repeat": 2,
                                                     "case_count": 1}}) + "\n" + line + "\n" + line + "\n",
                            encoding="utf-8")
            with self.assertRaises(ev.EvaluationError):
                self.read(path)

    def test_meta_mismatches_are_fatal(self):
        with tempfile.TemporaryDirectory() as temporary:
            for name, overrides, kwargs in (
                    ("language", {"language": "fr"}, {}),
                    ("repeat", {"repeat": 3}, {}),
                    ("case-count", {"case_count": 5}, {}),
            ):
                with self.subTest(rule=name):
                    path = Path(temporary) / f"{name}.jsonl"
                    self.write_results(path, meta_overrides=overrides)
                    with self.assertRaises(ev.EvaluationError):
                        self.read(path)

    def test_record_schema_violations_are_fatal(self):
        bad_rows = [
            ("unknown status", {"status": "mysterious"}),
            ("missing repeats", {"repeats": DELETE}),
            ("negative latency", {"repeats": [repeat([row(0, "goo")], latency_ns=-1),
                                              repeat([row(0, "goo")])]}),
            ("non-bool search_complete", {"repeats": [repeat([row(0, "goo")], complete=1),
                                                      repeat([row(0, "goo")])]}),
            ("rank not contiguous", {"repeats": [repeat([{**row(0, "goo"), "rank": 4}]),
                                                 repeat([row(0, "goo")])]}),
            ("kind outside enum", {"repeats": [repeat([row(7, "goo")]),
                                               repeat([row(7, "goo")])]}),
            ("text not a string", {"repeats": [repeat([{**row(0, "x"), "text_utf8": 5}]),
                                               repeat([{**row(0, "x"), "text_utf8": 5}])]}),
            ("lexeme id not uint64", {"repeats": [repeat([{**row(0, "goo"), "lexeme_ids": [-1]}]),
                                                  repeat([{**row(0, "goo"), "lexeme_ids": [-1]}])]}),
            ("inverted byte range", {"repeats": [repeat([{**row(0, "goo"),
                                                          "input_begin_utf8_byte": 2,
                                                          "input_end_utf8_byte": 1}]),
                                                 repeat([{**row(0, "goo"),
                                                          "input_begin_utf8_byte": 2,
                                                          "input_end_utf8_byte": 1}])]}),
            ("negative byte offset", {"repeats": [repeat([{**row(0, "goo"),
                                                           "input_begin_utf8_byte": -1}]),
                                                  repeat([{**row(0, "goo"),
                                                           "input_begin_utf8_byte": -1}])]}),
        ]
        for name, record_overrides in bad_rows:
            with self.subTest(rule=name):
                with tempfile.TemporaryDirectory() as temporary:
                    path = Path(temporary) / "en.run0.jsonl"
                    self.write_results(path, record_overrides=record_overrides)
                    with self.assertRaises(ev.EvaluationError):
                        self.read(path)

    def test_rank_and_kind_are_accepted_when_correct(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "en.run0.jsonl"
            rows = [dict(row(0, "goo"), rank=0), dict(row(1, "good", ids=[11]), rank=1)]
            self.write_results(path, record_overrides={
                "repeats": [repeat(list(rows)), repeat(list(rows))]})
            self.read(path)  # must not raise


class CorpusCrossCheckTests(unittest.TestCase):
    CORPUS_CASE = {"id": "en-a", "profile_id": 1, "raw_input": "goo"}

    def records(self, case: dict) -> dict:
        return {case["id"]: {"profile_id": case["profile_id"], "status": "ok", "repeats": [
            {"latency_ns": 1, "search_complete": True,
             "candidates": [dict(rank=0, kind=0, text_utf8="goo", lexeme_ids=[],
                                  input_begin_utf8_byte=0, input_end_utf8_byte=3)]}]}}
        # raw_input "goo" is exactly 3 UTF-8 bytes

    def test_matching_records_pass(self):
        ev.validate_records_against_corpus(self.records(self.CORPUS_CASE),
                                           [self.CORPUS_CASE], "en.run0.jsonl")

    def test_missing_case_is_fatal(self):
        with self.assertRaises(ev.EvaluationError):
            ev.validate_records_against_corpus({}, [self.CORPUS_CASE], "en.run0.jsonl")

    def test_extra_case_is_fatal(self):
        records = self.records(self.CORPUS_CASE)
        records["en-ghost"] = dict(records["en-a"])
        with self.assertRaises(ev.EvaluationError):
            ev.validate_records_against_corpus(records, [self.CORPUS_CASE], "en.run0.jsonl")

    def test_profile_mismatch_is_fatal(self):
        records = self.records(self.CORPUS_CASE)
        records["en-a"]["profile_id"] = 12
        with self.assertRaises(ev.EvaluationError):
            ev.validate_records_against_corpus(records, [self.CORPUS_CASE], "en.run0.jsonl")

    def test_byte_range_beyond_raw_input_is_fatal(self):
        records = self.records(self.CORPUS_CASE)
        records["en-a"]["repeats"][0]["candidates"][0]["input_end_utf8_byte"] = 4
        with self.assertRaises(ev.EvaluationError):
            ev.validate_records_against_corpus(records, [self.CORPUS_CASE], "en.run0.jsonl")


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
