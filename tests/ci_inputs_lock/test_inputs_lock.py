# Tests for tools/ci/inputs.lock.json and tools/ci/verify_inputs.ps1.
#
# Two layers:
# 1. The committed lock file itself: canonical deterministic bytes, exact entry
#    set, valid digest/commit formats, repo-relative build/third_party paths,
#    and no timestamps or machine-specific values.
# 2. The production verifier invoked as a real subprocess against synthetic
#    fixture trees: it must pass a correct tree, and fail (exit 1) on a corrupted
#    byte, a wrong size, a missing file, or a moved checkout commit. Invalid
#    arguments or an unreadable lock must exit 2, never 0.
#
# The tests never read or verify the real Tier 2 input bytes: this suite must
# pass on a machine without the pinned build tree. Verifying the real tree is
# the verifier's own job, run explicitly where the inputs exist.
from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
LOCK_PATH = REPO_ROOT / "tools" / "ci" / "inputs.lock.json"
VERIFY = REPO_ROOT / "tools" / "ci" / "verify_inputs.ps1"

FILE_SHA256 = "a" * 64
COMMIT_SHA1 = "b" * 40


def canonical_bytes(payload: dict) -> bytes:
    return (json.dumps(payload, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode("utf-8")


def run_verifier(repo_root: Path, lock_path: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
         "-File", str(VERIFY), "-RepoRoot", str(repo_root), "-Lock", str(lock_path)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        encoding="utf-8", errors="replace", timeout=180, check=False)


def file_entry(path: str, sha256: str, size: int, role: str = "fixture") -> dict:
    return {"kind": "file", "path": path, "pin_provenance": "test fixture",
            "role": role, "sha256": sha256, "size_bytes": size}


def commit_entry(path: str, commit: str, role: str = "fixture checkout") -> dict:
    return {"kind": "commit", "path": path, "pin_provenance": "test fixture",
            "role": role, "commit": commit}


def write_lock(directory: Path, entries: list[dict]) -> Path:
    lock = {"format": 1, "owner": "epocanva-ci-tier2-inputs-lock-v1",
            "purpose": "test fixture", "entries": entries}
    path = directory / "inputs.lock.json"
    path.write_bytes(canonical_bytes(lock))
    return path


def init_fixture_checkout(root: Path, rel: str) -> str:
    """Create a real git checkout outside the repository and return its HEAD."""
    checkout = root / rel
    checkout.mkdir(parents=True)
    env = dict(os.environ, GIT_CONFIG_GLOBAL=str(root / "gitconfig"),
               GIT_CONFIG_SYSTEM=str(root / "gitconfig"))
    def git(*arguments: str) -> str:
        completed = subprocess.run(
            ["git", "-C", str(checkout), *arguments], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace",
            timeout=60, check=False, env=env)
        if completed.returncode != 0:
            raise AssertionError(f"fixture git {' '.join(arguments)} failed: {completed.stderr}")
        return completed.stdout.strip()
    (checkout / "placeholder.txt").write_bytes(b"upstream checkout fixture\n")
    git("init")
    git("add", "placeholder.txt")
    git("-c", "user.name=fixture", "-c", "user.email=fixture@example.invalid",
        "commit", "-m", "fixture checkout")
    return git("rev-parse", "HEAD")


class CommittedLockTests(unittest.TestCase):
    def setUp(self) -> None:
        self.raw = LOCK_PATH.read_bytes()
        self.lock = json.loads(self.raw.decode("utf-8"))

    def test_committed_bytes_are_canonical(self) -> None:
        self.assertEqual(self.raw, canonical_bytes(self.lock),
                         "inputs.lock.json must be canonical sorted-key JSON with a trailing LF")

    def test_header_and_entry_schema(self) -> None:
        self.assertEqual(self.lock["format"], 1)
        self.assertEqual(self.lock["owner"], "epocanva-ci-tier2-inputs-lock-v1")
        self.assertIsInstance(self.lock.get("purpose"), str)
        self.assertTrue(self.lock["purpose"].strip())
        self.assertIsInstance(self.lock["entries"], list)
        self.assertGreaterEqual(len(self.lock["entries"]), 20)

    def test_no_timestamp_or_machine_fields(self) -> None:
        forbidden = ("generated_at", "timestamp", "generated_by", "machine", "user")
        def walk(payload: object) -> None:
            if isinstance(payload, dict):
                for key, value in payload.items():
                    lowered = key.lower()
                    self.assertNotIn(lowered, forbidden, f"forbidden volatile key {key!r}")
                    walk(value)
            elif isinstance(payload, list):
                for item in payload:
                    walk(item)
        walk(self.lock)

    def test_paths_are_repo_relative_and_scoped(self) -> None:
        seen: set[str] = set()
        for entry in self.lock["entries"]:
            path = entry["path"]
            self.assertNotIn("\\", path, f"entry path must use forward slashes: {path}")
            self.assertFalse(path.startswith("/"), f"entry path must be relative: {path}")
            self.assertFalse(Path(path).is_absolute(), f"entry path must be relative: {path}")
            self.assertTrue(path.startswith(("build/", "third_party/")),
                            f"Tier 2 entries are build/ or third_party/ scoped: {path}")
            self.assertNotIn("..", path.split("/"), f"entry path must not traverse: {path}")
            self.assertNotIn(path, seen, f"duplicate entry path: {path}")
            seen.add(path)
        self.assertEqual(len(seen), len(self.lock["entries"]))

    def test_digest_and_commit_formats(self) -> None:
        for entry in self.lock["entries"]:
            self.assertIn(entry["kind"], ("file", "commit"))
            if entry["kind"] == "file":
                self.assertRegex(entry["sha256"], r"^[0-9a-f]{64}$")
                self.assertIsInstance(entry["size_bytes"], int)
                self.assertGreater(entry["size_bytes"], 0)
            else:
                self.assertRegex(entry["commit"], r"^[0-9a-f]{40}$")
            self.assertTrue(str(entry["pin_provenance"]).strip())
            self.assertTrue(str(entry["role"]).strip())

    def test_documented_tier2_inputs_are_pinned(self) -> None:
        paths = {entry["path"] for entry in self.lock["entries"]}
        expected = {
            *(f"build/backend-training-budget/{lang}/model.tsv" for lang in ("en", "zh", "fr", "ja", "ko")),
            *(f"build/backend-training/{lang}/model.tsv" for lang in ("en", "zh", "fr", "ja", "ko")),
            *(f"build/backend-chinese-ranked/shared/{name}" for name in (
                "default.yaml", "epocanva_backend_pinyin.schema.yaml",
                "epocanva_backend_terra.dict.yaml", "epocanva_backend_zhuyin.schema.yaml",
                "zhuyin.yaml")),
            "build/backend-data-acquire/zh-fr/upstream/french/fr_FR/fr.aff",
            "build/backend-data-acquire/zh-fr/upstream/french/fr_FR/fr.dic",
            "build/backend-japanese-data/lexicon.bin",
            "build/product-data/epocanva_en_alpha.tsv",
            "build/librime-english/rime/install/lib/rime.dll",
            "build/librime-baseline/rime/install/lib/rime.dll",
            "third_party/librime",
        }
        self.assertEqual(paths, expected,
                         "the pinned Tier 2 set must match docs/testing/clean-worktree-verification.md "
                         "section 1 plus the default-model directory; update both together")

    def test_file_pins_match_the_repo_recorded_pins(self) -> None:
        """The budget-model and Hunspell pins must equal the values recorded in
        tests/backend/profiles.cc, so the lock cannot drift from the reviewed
        resource pins."""
        recorded = {
            "build/backend-training-budget/en/model.tsv":
                "51717a82412b7f82abd4bf81a77cd3dc39c0392e184f211c4cad2fe31021a2ad",
            "build/backend-training-budget/zh/model.tsv":
                "cce13364b25a56940fa1546e738a1571fcd04ed45ee8e91f2ed4ba8ecaa25998",
            "build/backend-training-budget/fr/model.tsv":
                "e5d6550dcf0da3fb1fefbf8bb7ddd6618dcd6bf3bfd0e2bc50f8c7cbe056b792",
            "build/backend-training-budget/ja/model.tsv":
                "b40382f5e977d01c40a3feb9d735ffdb72698610b9a41f53a9a55045303c0c77",
            "build/backend-training-budget/ko/model.tsv":
                "4bb6010cc7145d42361f07e616ceeef3117f1ea7c7a229794442030bd0aa3c31",
            "build/backend-data-acquire/zh-fr/upstream/french/fr_FR/fr.aff":
                "c176610cd5dc4846806a65ddd029f422d87978bf58f224aa44222662a16a2de5",
            "build/backend-data-acquire/zh-fr/upstream/french/fr_FR/fr.dic":
                "b78a868e31dd6e373b6c3217969afb898a9acde828a5e7ef97308da42218c88c",
            "build/backend-japanese-data/lexicon.bin":
                "e0308bb1e09e33915a40ca487ca3c27081d9564876f655a23fb332e2d6b3d011",
        }
        pinned = {entry["path"]: entry["sha256"] for entry in self.lock["entries"]
                  if entry["kind"] == "file"}
        for path, sha256 in recorded.items():
            self.assertEqual(pinned.get(path), sha256,
                             f"lock pin for {path} drifted from tests/backend/profiles.cc")


@unittest.skipUnless(os.name == "nt" and shutil.which("powershell"),
                     "the production verifier is a Windows PowerShell script")
@unittest.skipUnless(shutil.which("git"), "git is required for the commit-entry fixture")
class VerifierBehaviorTests(unittest.TestCase):
    @staticmethod
    def _make_file_tree(root: Path) -> dict:
        (root / "build").mkdir(parents=True)
        one = root / "build" / "one.bin"
        two = root / "build" / "two.bin"
        one.write_bytes(b"0123456789")
        two.write_bytes(b"abcdef")
        return {"one": one, "two": two}

    def test_correct_tree_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            files = self._make_file_tree(root)
            import hashlib
            entries = [
                file_entry("build/one.bin", hashlib.sha256(files["one"].read_bytes()).hexdigest(), 10),
                file_entry("build/two.bin", hashlib.sha256(files["two"].read_bytes()).hexdigest(), 6),
            ]
            completed = run_verifier(root, write_lock(root, entries))
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)

    def test_corrupted_byte_fails_with_the_entry_path(self) -> None:
        import hashlib
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            files = self._make_file_tree(root)
            original = files["one"].read_bytes()
            digest = hashlib.sha256(original).hexdigest()
            files["one"].write_bytes(b"X" + original[1:])
            entries = [file_entry("build/one.bin", digest, 10)]
            completed = run_verifier(root, write_lock(root, entries))
            self.assertEqual(completed.returncode, 1, completed.stdout)
            self.assertIn("build/one.bin", completed.stdout)
            self.assertIn("HASH_MISMATCH", completed.stdout)

    def test_wrong_size_fails(self) -> None:
        import hashlib
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            files = self._make_file_tree(root)
            entries = [file_entry("build/two.bin",
                                  hashlib.sha256(files["two"].read_bytes()).hexdigest(), 999)]
            completed = run_verifier(root, write_lock(root, entries))
            self.assertEqual(completed.returncode, 1, completed.stdout)
            self.assertIn("SIZE_MISMATCH", completed.stdout)

    def test_missing_file_fails(self) -> None:
        import hashlib
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            files = self._make_file_tree(root)
            entries = [file_entry("build/one.bin",
                                  hashlib.sha256(files["one"].read_bytes()).hexdigest(), 10),
                       file_entry("build/absent.bin", "c" * 64, 3)]
            completed = run_verifier(root, write_lock(root, entries))
            self.assertEqual(completed.returncode, 1, completed.stdout)
            self.assertIn("build/absent.bin", completed.stdout)
            self.assertIn("MISSING", completed.stdout)

    def test_pinned_checkout_commit_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._make_file_tree(root)
            head = init_fixture_checkout(root, "third_party/librime")
            entries = [commit_entry("third_party/librime", head)]
            completed = run_verifier(root, write_lock(root, entries))
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)

    def test_moved_checkout_commit_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._make_file_tree(root)
            checkout = root / "third_party" / "librime"
            env = dict(os.environ, GIT_CONFIG_GLOBAL=str(root / "gitconfig"),
                       GIT_CONFIG_SYSTEM=str(root / "gitconfig"))
            def git(*arguments: str) -> str:
                completed = subprocess.run(
                    ["git", "-C", str(checkout), *arguments], stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace",
                    timeout=60, check=False, env=env)
                if completed.returncode != 0:
                    raise AssertionError(f"fixture git failed: {completed.stderr}")
                return completed.stdout.strip()
            head = init_fixture_checkout(root, "third_party/librime")
            (checkout / "placeholder.txt").write_bytes(b"upstream checkout fixture changed\n")
            git("add", "placeholder.txt")
            git("-c", "user.name=fixture", "-c", "user.email=fixture@example.invalid",
                "commit", "-m", "second fixture commit")
            entries = [commit_entry("third_party/librime", head)]
            completed = run_verifier(root, write_lock(root, entries))
            self.assertEqual(completed.returncode, 1, completed.stdout)
            self.assertIn("COMMIT_MISMATCH", completed.stdout)

    def test_missing_checkout_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            entries = [commit_entry("third_party/absent", COMMIT_SHA1)]
            completed = run_verifier(root, write_lock(root, entries))
            self.assertEqual(completed.returncode, 1, completed.stdout)
            self.assertIn("third_party/absent", completed.stdout)

    def test_unknown_entry_kind_is_an_invalid_lock(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            entries = [{"kind": "symlink", "path": "build/one.bin",
                        "pin_provenance": "x", "role": "x"}]
            completed = run_verifier(root, write_lock(root, entries))
            self.assertEqual(completed.returncode, 2, completed.stdout)
            self.assertIn("unknown entry kind", completed.stdout)

    def test_malformed_digest_is_an_invalid_lock(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            entries = [file_entry("build/one.bin", "ZZGG", 10)]
            completed = run_verifier(root, write_lock(root, entries))
            self.assertEqual(completed.returncode, 2, completed.stdout)
            self.assertIn("INVALID_USAGE", completed.stdout)

    def test_missing_lock_file_is_invalid_usage(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            completed = run_verifier(root, root / "absent.lock.json")
            self.assertEqual(completed.returncode, 2, completed.stdout)

    def test_non_json_lock_file_is_invalid_usage(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            broken = root / "inputs.lock.json"
            broken.write_bytes(b"{not json")
            completed = run_verifier(root, broken)
            self.assertEqual(completed.returncode, 2, completed.stdout)


if __name__ == "__main__":
    unittest.main()
