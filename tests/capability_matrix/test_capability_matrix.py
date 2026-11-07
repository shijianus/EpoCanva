# Validation for data/locale/capability-matrix.json, the machine-readable E3
# capability matrix.
#
# The matrix is an authored claim file, not generated data: it records what is
# actually verified per backend binding and per dimension, with a committed
# evidence path for every positive status. This test makes those claims
# falsifiable. It rejects:
#   - non-canonical bytes, wrong format/owner, a changed status vocabulary;
#   - rows whose binding_id does not exist in data/locale/backend-bindings.json,
#     whose language does not match the binding, or duplicate
#     (binding_id, dimension) pairs;
#   - incomplete coverage (every binding must carry all four dimensions);
#   - any positive status without an existing, repo-relative committed evidence
#     file;
#   - any host_verified claim, and any host_acceptance row that is not
#     not_implemented (format 1 hard rule; bump format and validator together);
#   - language-level rows for languages that DO have bindings, and any positive
#     status for the engine-less declared languages (es/de stay not_implemented);
#   - the unverified zhuyin binding lacking its zhuyin-unverified note.
# Every rule below is also exercised as a negative mutation, so the validator
# cannot pass silently.
from __future__ import annotations

import copy
import json
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MATRIX_PATH = REPO_ROOT / "data" / "locale" / "capability-matrix.json"
BINDINGS_PATH = REPO_ROOT / "data" / "locale" / "backend-bindings.json"

DIMENSIONS = ("engine", "resources", "real_input_integration", "host_acceptance")
STATUSES = ("source_verified", "unit_tested", "integration_tested",
            "host_verified", "blocked", "not_implemented")
ENGINE_LESS_LANGUAGES = ("es", "de")


def canonical_bytes(payload: dict) -> bytes:
    return (json.dumps(payload, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode("utf-8")


def validate(matrix: dict, bindings: dict, repo_root: Path = REPO_ROOT) -> None:
    """All rules in one place so every negative mutation exercises real logic."""
    if matrix.get("format") != 1:
        raise ValueError("matrix format must be 1")
    if matrix.get("owner") != "epocanva-capability-matrix-v1":
        raise ValueError("matrix owner mismatch")
    if tuple(matrix.get("statuses", ())) != STATUSES:
        raise ValueError("status vocabulary changed without a format bump")
    if tuple(matrix.get("dimensions", ())) != DIMENSIONS:
        raise ValueError("dimension vocabulary changed without a format bump")
    rows = matrix.get("rows")
    if not isinstance(rows, list) or not rows:
        raise ValueError("matrix has no rows")

    binding_by_id = {binding["numeric_id"]: binding for binding in bindings["bindings"]}
    binding_languages = {binding["language_tag"] for binding in bindings["bindings"]}
    language_level_languages = {row["language"] for row in rows if row.get("binding_id") is None}
    if not language_level_languages <= set(ENGINE_LESS_LANGUAGES):
        raise ValueError("language-level rows are only allowed for engine-less declared languages")
    if language_level_languages != set(ENGINE_LESS_LANGUAGES):
        raise ValueError("every engine-less declared language needs language-level rows")

    seen: set[tuple] = set()
    for row in rows:
        binding_id = row.get("binding_id")
        language = row.get("language")
        dimension = row.get("dimension")
        status = row.get("status")
        note = row.get("note")
        if dimension not in DIMENSIONS:
            raise ValueError(f"unknown dimension {dimension!r}")
        if status not in STATUSES:
            raise ValueError(f"unknown status {status!r}")
        if (language, binding_id, dimension) in seen:
            raise ValueError(f"duplicate (language, binding_id, dimension) pair: "
                             f"{(language, binding_id, dimension)}")
        seen.add((language, binding_id, dimension))
        if binding_id is not None:
            if binding_id not in binding_by_id:
                raise ValueError(f"binding_id {binding_id} does not exist in the backend binding manifest")
            if language != binding_by_id[binding_id]["language_tag"]:
                raise ValueError(f"binding_id {binding_id} language {language!r} does not match the manifest")
            binding = binding_by_id[binding_id]
            if binding["scheme_implementation"] != "implemented" and "zhuyin-unverified" not in (note or ""):
                raise ValueError("a row for a binding whose scheme is not implemented must carry the "
                                 "zhuyin-unverified note")
        if status == "host_verified":
            raise ValueError("format 1 forbids host_verified claims; record a real registered host "
                             "acceptance and bump the format first")
        if dimension == "host_acceptance" and status != "not_implemented":
            raise ValueError("format 1 keeps every host_acceptance row not_implemented")
        if language in ENGINE_LESS_LANGUAGES and status != "not_implemented":
            raise ValueError(f"language {language} has no engine; only not_implemented is honest")

        evidence = row.get("evidence_ref")
        if status != "not_implemented" and not (isinstance(evidence, str) and evidence.strip()):
            raise ValueError(f"a {status} row needs a committed evidence_ref")
        if isinstance(evidence, str) and evidence.strip():
            if "\\" in evidence or evidence.startswith("/") or ".." in evidence.split("/"):
                raise ValueError(f"evidence_ref must be a repo-relative forward-slash path: {evidence}")
            evidence_path = repo_root / evidence
            if not evidence_path.is_file():
                raise ValueError(f"evidence_ref does not exist in the repository: {evidence}")

    expected_pairs = ({(binding_by_id[binding_id]["language_tag"], binding_id, dimension)
                       for binding_id in binding_by_id for dimension in DIMENSIONS} |
                      {(engine_less, None, dimension) for engine_less in ENGINE_LESS_LANGUAGES
                       for dimension in DIMENSIONS})
    if seen != expected_pairs:
        raise ValueError("matrix coverage is incomplete: every binding needs all four dimensions "
                         "and each engine-less language needs its language-level rows")


class MatrixTests(unittest.TestCase):
    def setUp(self) -> None:
        self.raw = MATRIX_PATH.read_bytes()
        self.matrix = json.loads(self.raw.decode("utf-8"))
        self.bindings = json.loads(BINDINGS_PATH.read_text(encoding="utf-8"))

    def test_committed_bytes_are_canonical(self) -> None:
        self.assertEqual(self.raw, canonical_bytes(self.matrix),
                         "capability-matrix.json must be canonical sorted-key JSON with a trailing LF")

    def test_matrix_is_valid(self) -> None:
        validate(self.matrix, self.bindings)

    def test_binding_count_matches_the_manifest(self) -> None:
        binding_ids = {row["binding_id"] for row in self.matrix["rows"] if row["binding_id"] is not None}
        self.assertEqual(binding_ids,
                         {binding["numeric_id"] for binding in self.bindings["bindings"]})
        self.assertEqual(len(binding_ids), 13)

    def test_positive_statuses_carry_real_evidence_paths(self) -> None:
        for row in self.matrix["rows"]:
            if row["status"] == "not_implemented":
                continue
            self.assertTrue((REPO_ROOT / row["evidence_ref"]).is_file(),
                            f"missing evidence {row['evidence_ref']}")

    def test_nothing_claims_host_verification(self) -> None:
        for row in self.matrix["rows"]:
            self.assertNotEqual(row["status"], "host_verified")
            if row["dimension"] == "host_acceptance":
                self.assertEqual(row["status"], "not_implemented")

    def test_engine_less_declared_languages_stay_not_implemented(self) -> None:
        rows = [row for row in self.matrix["rows"] if row["binding_id"] is None]
        self.assertEqual(len(rows), 4 * len(ENGINE_LESS_LANGUAGES))
        for row in rows:
            self.assertEqual(row["status"], "not_implemented")

    def test_unverified_scheme_binding_is_annotated(self) -> None:
        unverified = [binding for binding in self.bindings["bindings"]
                      if binding["scheme_implementation"] != "implemented"]
        self.assertEqual([binding["numeric_id"] for binding in unverified], [6])
        for row in self.matrix["rows"]:
            if row["binding_id"] == 6 and row["dimension"] != "host_acceptance":
                self.assertIn("zhuyin-unverified", row["note"])

    # ---- negative mutations: every rule must be able to fail ----

    def test_negative_unknown_binding_id(self) -> None:
        matrix = copy.deepcopy(self.matrix)
        matrix["rows"].append({"binding_id": 99, "dimension": "engine", "evidence_ref": "README.md",
                               "language": "en", "note": "", "status": "unit_tested"})
        with self.assertRaises(ValueError):
            validate(matrix, self.bindings)

    def test_negative_language_mismatch(self) -> None:
        matrix = copy.deepcopy(self.matrix)
        matrix["rows"][0]["language"] = "zh"
        with self.assertRaises(ValueError):
            validate(matrix, self.bindings)

    def test_negative_duplicate_pair(self) -> None:
        matrix = copy.deepcopy(self.matrix)
        matrix["rows"].append(copy.deepcopy(matrix["rows"][0]))
        with self.assertRaises(ValueError):
            validate(matrix, self.bindings)

    def test_negative_missing_dimension(self) -> None:
        matrix = copy.deepcopy(self.matrix)
        matrix["rows"] = [row for row in matrix["rows"]
                          if not (row["binding_id"] == 1 and row["dimension"] == "engine")]
        with self.assertRaises(ValueError):
            validate(matrix, self.bindings)

    def test_negative_unknown_status(self) -> None:
        matrix = copy.deepcopy(self.matrix)
        matrix["rows"][0]["status"] = "verified_by_vibes"
        with self.assertRaises(ValueError):
            validate(matrix, self.bindings)

    def test_negative_positive_status_without_evidence(self) -> None:
        matrix = copy.deepcopy(self.matrix)
        matrix["rows"][0]["evidence_ref"] = ""
        with self.assertRaises(ValueError):
            validate(matrix, self.bindings)

    def test_negative_missing_evidence_file(self) -> None:
        matrix = copy.deepcopy(self.matrix)
        matrix["rows"][0]["evidence_ref"] = "docs/reports/this-report-does-not-exist.md"
        with self.assertRaises(ValueError):
            validate(matrix, self.bindings)

    def test_negative_traversal_evidence(self) -> None:
        matrix = copy.deepcopy(self.matrix)
        matrix["rows"][0]["evidence_ref"] = "../../etc/passwd"
        with self.assertRaises(ValueError):
            validate(matrix, self.bindings)

    def test_negative_host_verified_claim(self) -> None:
        matrix = copy.deepcopy(self.matrix)
        matrix["rows"][0]["status"] = "host_verified"
        with self.assertRaises(ValueError):
            validate(matrix, self.bindings)

    def test_negative_positive_host_acceptance_row(self) -> None:
        matrix = copy.deepcopy(self.matrix)
        for row in matrix["rows"]:
            if row["dimension"] == "host_acceptance":
                row["status"] = "blocked"
                break
        with self.assertRaises(ValueError):
            validate(matrix, self.bindings)

    def test_negative_engine_less_language_claim(self) -> None:
        matrix = copy.deepcopy(self.matrix)
        for row in matrix["rows"]:
            if row["binding_id"] is None and row["language"] == "es" and row["dimension"] == "engine":
                row["status"] = "source_verified"
                break
        with self.assertRaises(ValueError):
            validate(matrix, self.bindings)

    def test_negative_unannotated_unverified_binding(self) -> None:
        matrix = copy.deepcopy(self.matrix)
        for row in matrix["rows"]:
            if row["binding_id"] == 6 and row["dimension"] == "engine":
                row["note"] = "looks fine"
                break
        with self.assertRaises(ValueError):
            validate(matrix, self.bindings)

    def test_negative_language_level_row_for_a_bound_language(self) -> None:
        matrix = copy.deepcopy(self.matrix)
        matrix["rows"].append({"binding_id": None, "dimension": "engine",
                               "evidence_ref": "README.md", "language": "en",
                               "note": "", "status": "not_implemented"})
        with self.assertRaises(ValueError):
            validate(matrix, self.bindings)


if __name__ == "__main__":
    unittest.main()
