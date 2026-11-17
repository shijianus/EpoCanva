"""Policy/approval mutation tests; synthetic approval bytes are never owner records."""
from __future__ import annotations

import copy
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/backend"))
import candidate_quality_policy as policy_module


class QualityPolicyTests(unittest.TestCase):
    def test_pending_policy_cannot_acquire_numeric_thresholds(self):
        policy = policy_module.load_policy()
        self.assertEqual(policy["absolute_quality"]["decision"], "pending_product_owner")
        for field in ("top1_min", "top3_min", "mrr_min"):
            mutated = copy.deepcopy(policy)
            mutated["absolute_quality"][field] = 0.0
            with self.subTest(field=field), self.assertRaises(policy_module.PolicyError):
                policy_module.validate_policy(mutated)

    def test_approved_shape_requires_finite_rates_and_integer_schema(self):
        for invalid in (True, "0.5", None, float("nan"), float("inf"), -0.1, 1.1):
            policy = policy_module.load_policy()
            policy["absolute_quality"] = dict(decision="approved", top1_min=0.5, top3_min=0.5, mrr_min=invalid)
            with self.subTest(invalid=repr(invalid)), self.assertRaises(policy_module.PolicyError):
                policy_module.validate_policy(policy)
        for invalid in (2.0, True, "2"):
            policy = policy_module.load_policy()
            policy["version"] = invalid
            with self.assertRaises(policy_module.PolicyError):
                policy_module.validate_policy(policy)

    def test_invalid_language_payload_fails_with_policy_error(self):
        policy = policy_module.load_policy()
        policy["languages"] = [[]]
        with self.assertRaises(policy_module.PolicyError):
            policy_module.validate_policy(policy)

    def test_tier_b_lock_binds_actual_corpus_and_manifest_bytes(self):
        parent = ROOT / "build" / "policy-tests"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as temporary:
            fixture = Path(temporary)
            cases = fixture / "cases"
            cases.mkdir()
            manifest = fixture / "manifest.json"
            manifest.write_text('{"tier":"B"}\n', encoding="utf-8")
            digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
            policy = policy_module.load_policy()
            corpus = {}
            lock = {"format": "epocanva-candidate-tier-b-lock-v1", "version": 1,
                    "status": "approved", "languages": {}}
            for language in policy["languages"]:
                corpus[language] = [{"source": "synthetic-parser-only", "license": "AGPL-3.0-only"}]
                path = cases / f"{language}.jsonl"
                path.write_text(json.dumps(corpus[language][0]) + "\n", encoding="utf-8")
                lock["languages"][language] = {
                    "path": path.relative_to(fixture).as_posix(), "sha256": digest(path),
                    "manifest_sha256": digest(manifest), "case_count": 1,
                    "source": "synthetic-parser-only", "license": "AGPL-3.0-only",
                    "license_conclusion": "approved_for_evaluation", "no_user_content": True,
                    "approval_reference": "synthetic-fixture-only",
                    "reproduction": {"kind": "approved_fixed_input", "reference": "synthetic-fixture-only"}}
            lock_path = fixture / "lock.json"

            def validate(value):
                lock_path.write_text(json.dumps(value), encoding="utf-8")
                return policy_module.validate_tier_b_lock(lock_path, policy=policy, cases_dir=cases,
                    corpus=corpus, manifest_path=manifest, repo_root=fixture)

            self.assertEqual(validate(lock)["status"], "passed")
            for field, value in (("sha256", "0" * 64), ("manifest_sha256", "0" * 64),
                    ("case_count", True), ("case_count", 2), ("approval_reference", ""),
                    ("license_conclusion", "not_approved"), ("no_user_content", False),
                    ("path", "../escaped.jsonl"), ("path", "cases/fr.jsonl")):
                mutated = copy.deepcopy(lock)
                mutated["languages"]["en"][field] = value
                with self.subTest(field=field, value=value), self.assertRaises(policy_module.PolicyError):
                    validate(mutated)
            for invalid_version in (True, 1.0, "1"):
                mutated = copy.deepcopy(lock)
                mutated["version"] = invalid_version
                with self.assertRaises(policy_module.PolicyError):
                    validate(mutated)
            mutated = copy.deepcopy(lock)
            mutated["status"] = "not_approved"
            with self.assertRaises(policy_module.PolicyError):
                validate(mutated)
            mutated["languages"] = {}
            self.assertEqual(validate(mutated)["status"], "not_evaluable")
            (cases / "en.jsonl").write_text("changed\n", encoding="utf-8")
            with self.assertRaises(policy_module.PolicyError):
                validate(lock)


if __name__ == "__main__":
    unittest.main()
