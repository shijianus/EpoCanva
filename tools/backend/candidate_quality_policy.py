"""The evaluator's single policy entry and approved-corpus lock validation.

Thresholds live only in quality-policy-v2.json. This module validates their
shape; it does not keep a second table of threshold values. No network I/O.
"""
from __future__ import annotations

import hashlib
import json
import math
import re
from pathlib import Path
from typing import Any

POLICY_PATH = Path(__file__).resolve().parents[2] / "data/eval/candidate-v1/quality-policy-v2.json"


class PolicyError(ValueError):
    pass


def _keys(value: Any, expected: set[str], where: str) -> None:
    if not isinstance(value, dict) or set(value) != expected:
        raise PolicyError(f"{where} has missing or unknown fields")


def _positive_int(value: Any, where: str) -> None:
    if type(value) is not int or value < 1:
        raise PolicyError(f"{where} must be a positive integer")


def _finite(value: Any, where: str, maximum: float | None = None) -> None:
    if (type(value) not in (int, float) or not math.isfinite(value) or value < 0 or
            (maximum is not None and value > maximum)):
        raise PolicyError(f"{where} must be a finite nonnegative number in range")


def validate_policy(policy: Any) -> dict[str, Any]:
    _keys(policy, {"format", "version", "languages", "qualification", "minimum_cases",
                   "thresholds", "regression", "invariants", "absolute_quality", "tier_b_lock"}, "policy")
    if policy["format"] != "epocanva-candidate-quality-policy-v2" or policy["version"] != 2:
        raise PolicyError("unsupported quality policy schema")
    languages = policy["languages"]
    if (not isinstance(languages, list) or not languages or len(set(languages)) != len(languages)
            or any(not isinstance(item, str) or not re.fullmatch(r"[a-z]{2}", item) for item in languages)):
        raise PolicyError("policy languages must be unique primary language tags")
    _keys(policy["qualification"], {"runs", "repeat", "warmup_queries", "seed",
                                    "bootstrap_replicates"}, "qualification")
    for key, value in policy["qualification"].items():
        _positive_int(value, key)
    _keys(policy["minimum_cases"], set(languages), "minimum_cases")
    for key, value in policy["minimum_cases"].items():
        _positive_int(value, key)
    _keys(policy["thresholds"], {"deterministic_case_rate_min", "wrong_language_rate_max",
                                "empty_menu_rate_max", "raw_present_rate_min", "raw_first_rate_min",
                                "not_observable_is_failure"}, "thresholds")
    for key, value in policy["thresholds"].items():
        if key == "not_observable_is_failure":
            if value is not True:
                raise PolicyError("not_observable must fail closed")
        else:
            _finite(value, key, 1)
    _keys(policy["regression"], {"warm_p95_ms", "peak_working_set_bytes"}, "regression")
    for key, limits in policy["regression"].items():
        _keys(limits, {"relative_increment_max", "absolute_increment_max"}, key)
        for subkey, value in limits.items():
            _finite(value, f"{key}.{subkey}")
    invariants = policy["invariants"]
    _keys(invariants, {"runtime_network", "telemetry", "raw_user_content_logging",
                       "product_learning_writes", "public_backend_abi"}, "invariants")
    for key, value in invariants.items():
        if key == "public_backend_abi":
            if type(value) is not int or value != 1:
                raise PolicyError("this evaluator only supports the public backend ABI v1")
        elif value is not False:
            raise PolicyError(f"privacy invariant {key} must be false")
    quality = policy["absolute_quality"]
    _keys(quality, {"decision", "top1_min", "top3_min", "mrr_min"}, "absolute_quality")
    if quality["decision"] == "pending_product_owner":
        if any(quality[key] is not None for key in ("top1_min", "top3_min", "mrr_min")):
            raise PolicyError("pending quality decisions cannot fabricate thresholds")
    elif quality["decision"] == "approved":
        for key in ("top1_min", "top3_min", "mrr_min"):
            _finite(quality[key], key, 1)
    else:
        raise PolicyError("unknown absolute quality decision status")
    lock = policy["tier_b_lock"]
    if not isinstance(lock, str) or Path(lock).name != lock or not lock.endswith(".json"):
        raise PolicyError("tier_b_lock must name a sibling JSON file")
    return policy


def load_policy(path: Path = POLICY_PATH) -> dict[str, Any]:
    try:
        policy = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise PolicyError("quality policy is unreadable") from exc
    return validate_policy(policy)


def validate_tier_b_lock(path: Path, *, policy: dict[str, Any], cases_dir: Path,
                         corpus: dict[str, list[dict[str, Any]]], manifest_path: Path,
                         repo_root: Path) -> dict[str, Any]:
    """Validate approved immutable bytes and provenance; absence stays explicit.

    An approved entry names the exact language corpus under the repository and
    its source, licence conclusion, approval reference and reproduction route.
    The lock also pins the manifest bytes. Pending decisions never qualify.
    """
    try:
        lock = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise PolicyError("Tier B lock is unreadable") from exc
    _keys(lock, {"format", "version", "status", "languages"}, "Tier B lock")
    if lock["format"] != "epocanva-candidate-tier-b-lock-v1" or lock["version"] != 1:
        raise PolicyError("unsupported Tier B lock schema")
    if lock["status"] == "not_approved":
        if lock["languages"] != {}:
            raise PolicyError("unapproved Tier B lock cannot claim approved entries")
        return {"status": "not_evaluable", "tier": "A", "reason": "Tier B approval is missing for " +
                ", ".join(policy["languages"]), "lock_sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    if lock["status"] != "approved":
        raise PolicyError("unknown Tier B lock status")
    _keys(lock["languages"], set(policy["languages"]), "Tier B languages")
    for language, entry in lock["languages"].items():
        _keys(entry, {"path", "sha256", "manifest_sha256", "case_count", "source",
                      "license", "license_conclusion", "approval_reference", "no_user_content",
                      "reproduction"}, f"Tier B {language}")
        for field in ("source", "license", "approval_reference"):
            if not isinstance(entry[field], str) or not entry[field].strip():
                raise PolicyError(f"Tier B {language}: {field} missing")
        if entry["license_conclusion"] != "approved_for_evaluation" or entry["no_user_content"] is not True:
            raise PolicyError(f"Tier B {language}: licence or no-user-content conclusion missing")
        _keys(entry["reproduction"], {"kind", "reference"}, "reproduction")
        if (entry["reproduction"]["kind"] not in ("approved_fixed_input", "reproducible_generation") or
                not isinstance(entry["reproduction"]["reference"], str) or
                not entry["reproduction"]["reference"].strip()):
            raise PolicyError(f"Tier B {language}: reproduction evidence missing")
        relative = Path(entry["path"]) if isinstance(entry["path"], str) else Path("..")
        if relative.is_absolute() or ".." in relative.parts:
            raise PolicyError(f"Tier B {language}: path escapes repository")
        resolved = (repo_root / relative).resolve()
        if not resolved.is_relative_to(repo_root.resolve()) or resolved != (cases_dir / f"{language}.jsonl").resolve():
            raise PolicyError(f"Tier B {language}: locked path is not the evaluated corpus")
        for field, artifact in (("sha256", resolved), ("manifest_sha256", manifest_path)):
            digest = entry[field]
            if (not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest) or
                    hashlib.sha256(artifact.read_bytes()).hexdigest() != digest):
                raise PolicyError(f"Tier B {language}: {field} mismatch")
        _positive_int(entry["case_count"], "case_count")
        if entry["case_count"] != len(corpus[language]):
            raise PolicyError(f"Tier B {language}: locked case count mismatch")
        if any(case["source"] != entry["source"] or case["license"] != entry["license"]
               for case in corpus[language]):
            raise PolicyError(f"Tier B {language}: case provenance disagrees with lock")
    return {"status": "passed", "tier": "B", "lock_sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
