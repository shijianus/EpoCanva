# Tests for tools/ci/contract_inventory.py, the exact CTest inventory
# verifier. The comparison core is pinned on synthetic ctest json-v1
# documents anchored to the committed expected inventory: a deleted expected
# test, a duplicate name, and an unregistered new contract test must each
# fail; the exact committed set must pass. The real build tree is checked by
# the CI gate itself, which runs this verifier against
# tools/ci/contract-tests.json before executing the labelled suite.
from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "ci"))

import contract_inventory as ci  # noqa: E402

EXPECTED_PATH = REPO_ROOT / "tools" / "ci" / "contract-tests.json"


def expected_names() -> list[str]:
    document = json.loads(EXPECTED_PATH.read_text(encoding="utf-8"))
    return list(document["tests"])


def ctest_document(names_with_labels: list[tuple[str, list[str]]]) -> list[dict]:
    # The property shape is the one CMake 3.31 actually emits ("value"); the
    # verifier also accepts the older "values" spelling.
    return [
        {"name": name,
         "properties": [{"name": "LABELS", "value": labels}]}
        for name, labels in names_with_labels
    ]


class InventoryVerifierTests(unittest.TestCase):
    def test_exact_committed_set_passes(self) -> None:
        expected = expected_names()
        tests = ctest_document([(name, ["contract-gate"]) for name in expected])
        tests.append({"name": "some_suite.not_gated", "properties": []})
        observed = ci.gate_test_names(tests)
        ci.compare_inventories(observed, expected)  # must not raise

    def test_deleted_expected_test_fails(self) -> None:
        expected = expected_names()
        reduced = expected[:-1]
        with self.assertRaises(ci.InventoryError):
            ci.compare_inventories(reduced, expected)

    def test_unregistered_new_contract_test_fails(self) -> None:
        expected = expected_names()
        with self.assertRaises(ci.InventoryError):
            ci.compare_inventories(expected + ["new_suite.contract"], expected)

    def test_duplicate_names_fail(self) -> None:
        expected = expected_names()
        with self.assertRaises(ci.InventoryError):
            ci.compare_inventories(expected + [expected[0]], expected)

    def test_duplicate_in_the_expected_file_itself_fails(self) -> None:
        expected = expected_names()
        with self.assertRaises(ci.InventoryError):
            ci.compare_inventories(list(expected), expected + [expected[0]])

    def test_label_selection_is_exact(self) -> None:
        tests = ctest_document([
            ("gated.one", ["contract-gate", "unit"]),
            ("ungated.one", ["unit", "negative"]),
            ("gated.two", ["negative", "contract-gate"]),
        ])
        self.assertEqual(ci.gate_test_names(tests), ["gated.one", "gated.two"])

    def test_labels_accepts_legacy_values_spelling(self) -> None:
        tests = [{"name": "legacy.test",
                  "properties": [{"name": "LABELS", "values": ["contract-gate"]}]}]
        self.assertEqual(ci.gate_test_names(tests), ["legacy.test"])

    def test_missing_labels_property_is_not_gated(self) -> None:
        tests = [{"name": "naked.test"}]
        self.assertEqual(ci.gate_test_names(tests), [])

    def test_malformed_ctest_document_fails(self) -> None:
        with self.assertRaises(ci.InventoryError):
            ci.gate_test_names(["not-a-dict"])

    def test_committed_expected_inventory_is_sorted_and_deduplicated(self) -> None:
        expected = expected_names()
        self.assertTrue(expected, "the expected inventory must not be empty")
        self.assertEqual(sorted(expected), expected, "expected inventory must be sorted")
        self.assertEqual(len(expected), len(set(expected)))
        self.assertIn("ci_gate.contract", expected)


if __name__ == "__main__":
    unittest.main()
