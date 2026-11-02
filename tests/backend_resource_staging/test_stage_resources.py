"""Boundary, corruption and publication tests for local backend staging.

All temporary fixtures stay in build/backend-resource-staging/tests/. The real
resource integration check is opt-in through EPOCANVA_STAGING_REAL_TESTS=1.
"""
from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).absolute().parents[2]
TOOL = ROOT / "tools/backend/stage_resources.py"
spec = importlib.util.spec_from_file_location("epocanva_stage_resources_tests", TOOL)
assert spec and spec.loader
stage = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = stage
spec.loader.exec_module(stage)


def definitions(language="en", rows=None):
    if rows is None:
        rows = [(language, "gm", "good morning", "project-authored-test")]
    return ("EPOCANVA_ABBREVIATION_DEFINITIONS_TSV\t1\n"
            "language\tkey\texpansion\tevidence\n" +
            "".join("\t".join(row) + "\n" for row in rows)).encode("utf-8")


def model_and_report():
    payload = (b"L\t27\ten\tword\tgood\nL\t91\ten\tword\tmorning\n"
               b"L\t109\ten\tpunct\t.\nU\t0\t1\nU\t27\t1\nU\t91\t1\n"
               b"U\t109\t1\nB\t27\t91\t1\nT\t27\t91\t109\t1\n")
    payload_sha = hashlib.sha256(payload).hexdigest()
    content = ("EPOCANVA_PREDICTION_TSV\t1\nid_scheme\tfnv1a64-language-normalized-v1\n"
               "normalization\tascii-lower-utf8-v1\nsource\tproduction-corpus\n"
               f"source_sha256\t{'a' * 64}\npayload_sha256\t{payload_sha}\n"
               "lexemes\t3\nunigram_total\t4\nbigram_total\t1\ntrigram_total\t1\ndata\n").encode() + payload
    return content, {"source_kind": "production-corpus", "source_sha256": "a" * 64,
                     "model": {"model_bytes": len(content), "payload_sha256": payload_sha,
                               "lexemes": 3, "unigram_total": 4, "bigram_total": 1,
                               "trigram_total": 1}}


class FixtureTest(unittest.TestCase):
    def setUp(self):
        parent = ROOT / "build/backend-resource-staging/tests"
        parent.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="case-", dir=parent)
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        self.root = self.repo / "build/acceptance/product"

    def write(self, relative, content):
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path


class PathAdmissionTests(FixtureTest):
    def test_exact_root_relative_and_absolute(self):
        self.assertEqual(stage.validate_product_root(self.repo, "build/acceptance/product"), self.root)
        self.assertEqual(stage.validate_product_root(self.repo, self.root), self.root)
        self.assertFalse(self.root.exists())

    def test_reject_source_third_party_profile_system_and_preset_roots(self):
        for target in (".", "build", "build/acceptance", "data/product", "third_party/product",
                       str(self.repo.parent / "outside"), "C:/Windows/System32/product"):
            with self.subTest(target=target), self.assertRaises(stage.StagingError):
                stage.validate_product_root(self.repo, target)

    def test_traversal_and_windows_aliases_are_rejected_on_all_platforms(self):
        for path in ("build/acceptance/../product", "build//product/root", "build/./product/root",
                     "build/acceptance/product/", "build/acceptance/prod.",
                     "build/acceptance/prod ", "build/acceptance/CON.txt", "build/p/NUL",
                     "build/p/LPT1.bin", "build/p/a:stream", "build/p/a?", "build/p/a\n"):
            with self.subTest(path=path), self.assertRaises(stage.StagingError):
                stage.validate_product_root(self.repo, path)

    def test_product_root_never_nests_inside_a_source_preset(self):
        for preset in stage.SOURCE_PRESETS:
            with self.subTest(preset=preset), self.assertRaisesRegex(stage.StagingError, "source tree"):
                stage.validate_product_root(self.repo, f"build/{preset}/new-product")

    def test_network_url_and_device_paths_are_rejected(self):
        for path in ("//server/share/product", r"\\server\share\product", r"\\?\C:\product",
                     r"\\.\pipe\name", "https://example.invalid/product", "file:///build/p/product",
                     "C:relative", "/??/C:/product"):
            with self.subTest(path=path), self.assertRaises(stage.StagingError):
                stage.validate_product_root(self.repo, path)

    def test_portable_relative_paths(self):
        self.assertEqual(stage.relative_path("data/backend/fr/école.tsv"), "data/backend/fr/école.tsv")
        for path in ("/data/a", "../data/a", r"data\a", "data//a", "data/a:alt", "data/a\x00"):
            with self.subTest(path=path), self.assertRaises(stage.StagingError):
                stage.relative_path(path)

    def test_symlink_ancestor_refused(self):
        destination = self.repo / "outside"
        destination.mkdir()
        (self.repo / "build").mkdir()
        link = self.repo / "build/acceptance"
        try:
            link.symlink_to(destination, target_is_directory=True)
        except OSError as exc:
            self.skipTest(f"creating symlinks is not permitted by this host: {exc}")
        with self.assertRaises(stage.StagingError):
            stage.validate_product_root(self.repo, "build/acceptance/product")
        self.assertEqual(list(destination.iterdir()), [])

    def test_recall_and_junction_metadata_refused_before_read(self):
        info = self.repo.stat()
        for attrs, tag in ((0x1000, 0), (0x400000, 0), (0x40000, 0),
                           (0x400, 0xA0000003), (0x400, 0xA000000C)):
            fake = mock.Mock(st_mode=info.st_mode, st_file_attributes=attrs, st_reparse_tag=tag)
            with self.subTest(attrs=attrs, tag=tag), mock.patch.object(Path, "lstat", return_value=fake):
                with self.assertRaises(stage.StagingError):
                    stage.check_local_path(self.repo)

    def test_resident_cloud_metadata_is_admitted(self):
        info = self.repo.stat()
        fake = mock.Mock(st_mode=info.st_mode, st_file_attributes=0x400, st_reparse_tag=0x9000301A)
        with mock.patch.object(Path, "lstat", return_value=fake):
            stage.check_local_path(self.repo)


class ProvenanceTests(FixtureTest):
    def test_missing_source_is_an_error(self):
        inv = stage.Inventory(self.repo)
        with self.assertRaises(stage.StagingError):
            inv.source("build/input/file", "a" * 64, 10, "reviewed report")
        self.assertEqual(inv.inputs, {})

    def test_resource_digest_is_checked_against_an_independent_expected_digest(self):
        trusted = b"reviewed resource\n"
        self.write("build/input/resource", trusted)
        inv = stage.Inventory(self.repo)
        inv.source("build/input/resource", hashlib.sha256(trusted).hexdigest(), len(trusted), "pinned test report")
        self.write("build/input/resource", b"changed resource!\n")
        with self.assertRaisesRegex(stage.StagingError, "SHA-256 mismatch"):
            stage.Inventory(self.repo).source("build/input/resource", hashlib.sha256(trusted).hexdigest(), len(trusted), "pinned test report")

    def test_byte_count_is_independently_checked(self):
        self.write("build/input/resource", b"bytes")
        with self.assertRaisesRegex(stage.StagingError, "byte-count"):
            stage.Inventory(self.repo).source("build/input/resource", hashlib.sha256(b"bytes").hexdigest(), 4, "pin")

    def test_changed_report_refused_even_if_it_rebinds_resource_digest(self):
        original = stage.json_bytes({"model_sha256": "a" * 64})
        self.write("build/input/report.json", stage.json_bytes({"model_sha256": "b" * 64}))
        with self.assertRaisesRegex(stage.StagingError, "SHA-256 mismatch"):
            stage.Inventory(self.repo).checked_json("build/input/report.json", hashlib.sha256(original).hexdigest())

    def test_unverified_file_cannot_be_copied(self):
        with self.assertRaisesRegex(stage.StagingError, "unverified copy"):
            stage.Inventory(self.repo).copy("build/input/file", "data/backend/file")

    def test_same_source_change_is_detected_during_inventory(self):
        self.write("build/input/resource", b"first")
        inv = stage.Inventory(self.repo)
        inv.authored("build/input/resource")
        self.write("build/input/resource", b"second")
        with self.assertRaisesRegex(stage.StagingError, "changed during verification"):
            inv.authored("build/input/resource")

    def test_model_report_binding_and_word_only_id_lookup(self):
        content, report = model_and_report()
        self.assertEqual(stage.inspect_model(content, "en", report), {"good": 27, "morning": 91})
        bad = content.replace(b"morning", b"warning")
        with self.assertRaises(stage.StagingError):
            stage.inspect_model(bad, "en", report)
        for changed in ("source_sha256", "payload_sha256", "unigram_total", "lexemes"):
            modified = copy.deepcopy(report)
            if changed == "source_sha256":
                modified[changed] = "b" * 64
            elif changed.endswith("sha256"):
                modified["model"][changed] = "b" * 64
            else:
                modified["model"][changed] += 1
            with self.subTest(field=changed), self.assertRaises(stage.StagingError):
                stage.inspect_model(content, "en", modified)
        with self.assertRaises(stage.StagingError):
            stage.inspect_model(content, "fr", report)

    def test_all_splits_and_attribution_are_bound_to_report(self):
        author = {"author": "known-author", "sentence_url": "https://tatoeba.org/sentences/show/1",
                  "license_url": "https://creativecommons.org/licenses/by/2.0/fr/"}
        line = stage.json_bytes({"language": "en", "source": "tatoeba-detailed-snapshot",
                                "license": "CC-BY-2.0-FR", "attribution": author})
        # A corpus record is one physical JSONL line, not indented JSON.
        line = (json.dumps(json.loads(line), sort_keys=True) + "\n").encode()
        report = {"splits": {}, "provenance": {"distinct_attribution_count": 1,
                  "attribution_sha256": hashlib.sha256(stage.canonical_attribution(author) + b"\n").hexdigest()}}
        for split in ("train", "dev", "test"):
            self.write(f"build/backend-training-budget/en/model-splits/{split}.jsonl", line)
            report["splits"][split] = {"sha256": hashlib.sha256(line).hexdigest(), "records": 1}
        inv = stage.Inventory(self.repo)
        self.assertEqual(stage.attribution_from_splits(inv, "en", report), stage.canonical_attribution(author) + b"\n")
        self.assertEqual(len(inv.inputs), 3)
        report["provenance"]["attribution_sha256"] = "0" * 64
        with self.assertRaises(stage.StagingError):
            stage.attribution_from_splits(stage.Inventory(self.repo), "en", report)


class AbbreviationTests(unittest.TestCase):
    def test_real_ids_canonical_resource_and_partial_keys(self):
        output, info = stage.compile_abbreviations(definitions(), "en", {"good": 27, "morning": 91})
        self.assertEqual(output, b"EPOCANVA_ABBREVIATIONS_TSV\t1\nen\tgm\t27 91\n")
        self.assertEqual(info["accepted_records"], 1)
        self.assertEqual(info["skipped_oov"], [])
        # The native decoder's trie consumes full keys and matches any prefix.
        # No generated prefix aliases or preselected candidate text is needed.
        rows = [line.split("\t") for line in output.decode().splitlines()[1:]]
        self.assertEqual([row[2] for row in rows if row[1].startswith("g")], ["27 91"])

    def test_oov_is_reported_or_refused_never_fabricated(self):
        output, info = stage.compile_abbreviations(definitions(), "en", {"good": 27})
        self.assertEqual(output, b"EPOCANVA_ABBREVIATIONS_TSV\t1\n")
        self.assertEqual(info["skipped_oov"], [{"key": "gm", "expansion": "good morning", "missing_model_words": ["morning"]}])
        with self.assertRaisesRegex(stage.StagingError, "OOV abbreviation"):
            stage.compile_abbreviations(definitions(), "en", {"good": 27}, oov="error")

    def test_strict_tsv_no_bom_cr_nul_blank_extra_columns_or_non_scalar(self):
        valid = definitions()
        invalid = [b"\xef\xbb\xbf" + valid, valid.replace(b"\n", b"\r\n"),
                   valid[:-1], valid + b"\n", valid.replace(b"good", b"good\0"),
                   valid + b"en\tk\tgood\tevidence\textra\n", valid.replace(b"good", b"\xed\xa0\x80")]
        for data in invalid:
            with self.subTest(data=data[-60:]), self.assertRaises((stage.StagingError, UnicodeError)):
                stage.compile_abbreviations(data, "en", {"good": 27, "morning": 91})

    def test_key_and_output_component_bounds_are_utf8_bytes(self):
        for key in ("", " uppercase", "UPPER", "a  b", "a ", "a\tb", "a" * 257,
                    "é" * 129, " ".join(["a"] * 9), " ".join(["a" * 200] * 3)):
            with self.subTest(key=key[:30]), self.assertRaises(stage.StagingError):
                stage.compile_abbreviations(definitions(rows=[("en", key, "good", "test")]), "en", {"good": 27})
        key = "é" * 128  # exactly 256 UTF-8 bytes
        output, _ = stage.compile_abbreviations(definitions(rows=[("en", key, "good", "test")]), "en", {"good": 27})
        self.assertIn(key.encode(), output)
        with self.assertRaises(stage.StagingError):
            stage.compile_abbreviations(definitions(rows=[("en", "k", " ".join(["good"] * 9), "test")]), "en", {"good": 27})

    def test_language_duplicates_and_invalid_ids_are_rejected(self):
        for rows in ([('fr', 'gm', 'good morning', 'test')],
                     [('en', 'gm', 'good morning', 'test')] * 2):
            with self.assertRaises(stage.StagingError):
                stage.compile_abbreviations(definitions(rows=rows), "en", {"good": 27, "morning": 91})
        for identifier in (0, -1, 1 << 64):
            with self.subTest(identifier=identifier), self.assertRaises(stage.StagingError):
                stage.compile_abbreviations(definitions(), "en", {"good": identifier, "morning": 91})

    def test_generated_resource_record_byte_and_trie_bounds(self):
        source = definitions(rows=[("en", "ab", "good", "test"), ("en", "ac", "good", "test")])
        with mock.patch.object(stage, "MAX_ABBREVIATION_RECORDS", 1):
            with self.assertRaisesRegex(stage.StagingError, "record bound"):
                stage.compile_abbreviations(source, "en", {"good": 27})
        with mock.patch.object(stage, "MAX_ABBREVIATION_TRIE_NODES", 3):
            with self.assertRaisesRegex(stage.StagingError, "node bound"):
                stage.compile_abbreviations(source, "en", {"good": 27})
        with mock.patch.object(stage, "MAX_ABBREVIATION_TRIE_NODES", 4):
            stage.compile_abbreviations(source, "en", {"good": 27})
        with mock.patch.object(stage, "MAX_ABBREVIATION_UTF8_BYTES", len(source) - 1):
            with self.assertRaisesRegex(stage.StagingError, "byte bound"):
                stage.compile_abbreviations(source, "en", {"good": 27})

    def test_deterministic_sort_and_all_english_rules_migrated(self):
        english = (ROOT / "data/backend/abbreviations/en.tsv").read_bytes()
        original = (ROOT / "engine/languages/en/language_support.cc").read_text(encoding="utf-8")
        block = original.split("constexpr ExpansionRule kExpansions[] = {", 1)[1].split("};", 1)[0]
        rules = set(re.findall(r'\{"([^"]+)", "([^"]+)", CandidateType::k\w+\}', block))
        migrated = {(row.split("\t")[1], row.split("\t")[2]) for row in english.decode().splitlines()[2:]}
        self.assertEqual(migrated, rules)
        self.assertEqual(len(migrated), 36)
        source = definitions(rows=[("en", "z", "good", "test"), ("en", "a", "morning", "test")])
        output, _ = stage.compile_abbreviations(source, "en", {"good": 27, "morning": 91})
        self.assertEqual(output.splitlines()[1:], [b"en\ta\t91", b"en\tz\t27"])


class ResourceIndexTests(unittest.TestCase):
    def row(self, identifier=1):
        return [str(identifier), "1", "1", "1", "1", "en-US", "data/backend/en/model.tsv",
                "a" * 64, "-", "-", "-", "-", "-", "-"]

    def test_exact_header_14_columns_lf_and_shared_model_bytes(self):
        data = stage.resource_index([self.row(1), self.row(11)])
        lines = data.decode().splitlines()
        self.assertEqual(lines[0], "epocanva-backend-resources\t1")
        self.assertEqual(lines[1].split("\t"), stage.RESOURCE_COLUMNS)
        self.assertTrue(all(len(line.split("\t")) == 14 for line in lines[1:]))
        self.assertNotIn(b"\xef\xbb\xbf", data)
        self.assertNotIn(b"\r", data)
        self.assertTrue(data.endswith(b"\n"))

    def test_32_row_boundary_unique_ids_optional_pairs_and_mandatory_model(self):
        stage.resource_index([self.row(i) for i in range(1, 33)])
        invalid = [[], [self.row(i) for i in range(1, 34)], [self.row(), self.row()], [self.row(0)]]
        row = self.row()
        row[9] = "a" * 64
        invalid.append([row])
        row = self.row()
        row[6:8] = ["-", "-"]
        invalid.append([row])
        row = self.row()
        row[6] = "../outside"
        invalid.append([row])
        row = self.row()
        row[7] = "unknown"
        invalid.append([row])
        for rows in invalid:
            with self.subTest(rows=len(rows)), self.assertRaises(stage.StagingError):
                stage.resource_index(rows)

    def test_64kib_boundary_is_inclusive_and_counts_utf8_bytes(self):
        rows = [self.row(i) for i in range(1, 33)]
        used_bytes = len(stage.resource_index(rows))
        remaining_bytes = 65536 - used_bytes
        # Extend one valid relative resource path to exactly the format bound.
        rows[0][6] += "x" * remaining_bytes
        self.assertEqual(len(stage.resource_index(rows)), 65536)
        rows[0][6] += "x"
        with self.assertRaises(stage.StagingError):
            stage.resource_index(rows)
        rows[0][6] = rows[0][6][:-2] + "é"
        with self.assertRaises(stage.StagingError):
            stage.resource_index(rows)

    def test_profiles_use_actual_header_enums_and_requested_variants(self):
        enums = stage.api_enums((ROOT / "engine/include/epocanva_backend.h").read_bytes())
        files = {}
        for lang in stage.LANGUAGES:
            for name in ("model.tsv", "fr.aff", "fr.dic", "lexicon.bin", "abbreviations.tsv"):
                files[f"data/backend/{lang}/{name}"] = stage.Payload("a" * 64, 1, content=b"x")
        rows = stage.profile_rows(enums, files)
        self.assertEqual([int(row[0]) for row in rows], list(range(1, 14)))
        self.assertEqual([row[5] for row in rows], ["en-US", "zh-pinyin", "fr-FR", "ja-JP", "ko-KR",
                         "zh-zhuyin", "ja-JP", "ja-JP", "ja-JP", "ko-KR", "en-US", "en-US", "en-US"])
        self.assertEqual(int(rows[8][2]), enums["EPOCANVA_BACKEND_KANA"])
        self.assertEqual(int(rows[9][2]), enums["EPOCANVA_BACKEND_LATIN"])
        self.assertEqual([int(rows[i][4]) for i in (10, 11, 12)], [enums["EPOCANVA_BACKEND_KEYBOARD_" + key]
                         for key in ("GERMAN_QWERTZ", "DVORAK", "FRENCH_AZERTY")])
        self.assertEqual(rows[0][6:8], rows[12][6:8])
        del enums["EPOCANVA_BACKEND_KANA"]
        with self.assertRaises(stage.StagingError):
            stage.profile_rows(enums, files)


class PublicationTests(FixtureTest):
    def inventory(self):
        inv = stage.Inventory(self.repo)
        self.write("build/input/model", b"source-model\n")
        inv.source("build/input/model", hashlib.sha256(b"source-model\n").hexdigest(), 13, "test report")
        inv.copy("build/input/model", "data/backend/en/model.tsv")
        inv.generated("data/backend/resources.tsv", b"generated-index\n")
        inv.generated(stage.MANIFEST_PATH, stage.json_bytes({"owner": stage.OWNER}))
        return inv

    def test_identical_rerun_preserves_files_and_product_default(self):
        inv = self.inventory()
        self.root.mkdir(parents=True)
        (self.root / "data").mkdir()
        default = self.root / "data/default.yaml"
        default.write_bytes(b"English existing default\n")
        stage.publish(self.repo, self.root, inv)
        before = {str(path.relative_to(self.root)): (path.read_bytes(), path.stat().st_mtime_ns)
                  for path in self.root.rglob("*") if path.is_file()}
        stage.publish(self.repo, self.root, inv)
        after = {str(path.relative_to(self.root)): (path.read_bytes(), path.stat().st_mtime_ns)
                 for path in self.root.rglob("*") if path.is_file()}
        self.assertEqual(before, after)
        self.assertEqual(default.read_bytes(), b"English existing default\n")
        source = self.repo / "build/input/model"
        target = self.root / "data/backend/en/model.tsv"
        self.assertFalse(os.path.samefile(source, target))

    def test_foreign_changed_target_preflight_writes_nothing(self):
        inv = self.inventory()
        conflict = self.root / "data/backend/resources.tsv"
        conflict.parent.mkdir(parents=True)
        conflict.write_bytes(b"foreign file\n")
        with self.assertRaisesRegex(stage.StagingError, "changed existing target"):
            stage.publish(self.repo, self.root, inv)
        self.assertEqual(conflict.read_bytes(), b"foreign file\n")
        self.assertFalse((self.root / "data/backend/en/model.tsv").exists())
        self.assertEqual(sorted(path.name for path in self.root.iterdir()), ["data"])

    def test_owned_modified_target_and_owned_update_are_also_refused(self):
        inv = self.inventory()
        stage.publish(self.repo, self.root, inv)
        path = self.root / "data/backend/en/model.tsv"
        path.write_bytes(b"edited owned resource\n")
        with self.assertRaises(stage.StagingError):
            stage.publish(self.repo, self.root, inv)
        path.write_bytes(b"source-model\n")
        inv.files["data/backend/en/model.tsv"] = stage.Payload(hashlib.sha256(b"new").hexdigest(), 3, content=b"new")
        with self.assertRaises(stage.StagingError):
            stage.publish(self.repo, self.root, inv)
        self.assertEqual(path.read_bytes(), b"source-model\n")

    def test_input_changes_during_copy_refused_and_no_outputs_published(self):
        inv = self.inventory()
        self.write("build/input/model", b"corrupt copy!\n")
        with self.assertRaisesRegex(stage.StagingError, "changed while copying"):
            stage.publish(self.repo, self.root, inv)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_leaf_created_after_preflight_cannot_be_overwritten(self):
        inv = self.inventory()
        real_link = os.link
        def raced_link(source, destination):
            if str(destination).endswith("resources.tsv"):
                Path(destination).write_bytes(b"concurrent foreign content\n")
            return real_link(source, destination)
        with mock.patch.object(stage.os, "link", side_effect=raced_link):
            with self.assertRaisesRegex(stage.StagingError, "changed existing target"):
                stage.publish(self.repo, self.root, inv)
        self.assertEqual((self.root / "data/backend/resources.tsv").read_bytes(), b"concurrent foreign content\n")
        self.assertFalse((self.root / "data/backend/en/model.tsv").exists())
        self.assertFalse((self.root / stage.MANIFEST_PATH).exists())
        self.assertFalse(any(path.name.startswith(".backend-staging-") for path in self.root.iterdir()))

    def test_check_only_does_not_create_product_root(self):
        inv = self.inventory()
        with mock.patch.object(stage, "prepare_plan", return_value=(inv, {"owner": stage.OWNER})):
            result = stage.stage_resources("build/acceptance/product", repo_root=self.repo, check_only=True)
        self.assertEqual(result["owner"], stage.OWNER)
        self.assertFalse(self.root.exists())

    def test_overlapping_input_tree_is_refused(self):
        inv = self.inventory()
        inv.inputs["build/acceptance/product/input"] = {"path": "build/acceptance/product/input"}
        with self.assertRaisesRegex(stage.StagingError, "overlaps"):
            stage.publish(self.repo, self.root, inv)
        self.assertFalse(self.root.exists())

    def test_cli_failure_exit_is_nonzero_and_output_root_is_required(self):
        with mock.patch.object(stage, "stage_resources", side_effect=stage.StagingError("test rejection")):
            with mock.patch.object(sys, "stderr"):
                self.assertEqual(stage.main(["--product-root", "build/p/product"]), 1)
        with mock.patch.object(sys, "stderr"):
            with self.assertRaises(SystemExit) as result:
                stage.main([])
        self.assertEqual(result.exception.code, 2)


@unittest.skipUnless(os.environ.get("EPOCANVA_STAGING_REAL_TESTS") == "1",
                     "opt-in real corpus/resource integration test")
class RealResourceTests(FixtureTest):
    def test_full_local_stage_and_deterministic_rerun(self):
        target = "build/backend-resource-staging/tests/" + self.repo.name + "/real-product"
        root = ROOT / target
        self.assertFalse(root.exists())
        first = stage.stage_resources(target)
        second = stage.stage_resources(target)
        self.assertEqual(first, second)
        self.assertEqual(first["profile_count"], 13)
        self.assertFalse(first["public_distribution_authorized"])
        self.assertTrue(first["distribution_blockers"])
        for row in first["files"]:
            data = (root / row["path"]).read_bytes()
            self.assertEqual(hashlib.sha256(data).hexdigest(), row["sha256"])
            self.assertEqual(len(data), row["bytes"])
        self.assertFalse((root / "data/default.yaml").exists())
        self.assertEqual(len((root / "data/backend/resources.tsv").read_bytes().splitlines()), 15)
        self.assertEqual(first["abbreviations"]["en"]["accepted_records"], 36)
        self.assertEqual(first["abbreviations"]["fr"]["accepted_records"], 4)
        model_bytes = (root / "data/backend/en/model.tsv").read_bytes()
        with (root / "data/backend/en/model.tsv").open("ab") as stream:
            stream.write(b"corruption\n")
        with self.assertRaisesRegex(stage.StagingError, "changed existing target"):
            stage.stage_resources(target)
        self.assertEqual((ROOT / "build/backend-training-budget/en/model.tsv").read_bytes(), model_bytes)


if __name__ == "__main__":
    unittest.main()
