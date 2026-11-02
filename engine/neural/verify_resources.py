"""Offline verification for the acquired Qwen and llama.cpp resources.

This verifier only reads files under build/backend-model-acquire. It never invokes
network clients, executes source, installs packages, or reads user text.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import sys
import tarfile

MODEL_REVISION = "060db6499f32faf8b98477b0a26969ef7d8b9987"
MODEL_REPO = "Qwen/Qwen2.5-0.5B"
LLAMA_COMMIT = "ec7f3ac9ab33e46b136eb5ab6a76c4d81f57c7f1"
LLAMA_TAR_SHA256 = "21e88c79dbbec249a91b093b56f375a3248a9bbe80c5fb19d42c0849f148aa66"
MODEL_FILES = {
    ".gitattributes": (1519, "11ad7efa24975ee4b0c3c3a38ed18737f0658a5f75a0a96787b576a78a023361", None),
    "LICENSE": (11343, "832dd9e00a68dd83b3c3fb9f5588dad7dcf337a0db50f7d9483f310cd292e92e", None),
    "README.md": (3850, "3c93cbc9f60417337fcbf093da872007586b9d886b0f0187f21aec35b869616a", None),
    "config.json": (681, "479dcf0c5286339e41ad3992cd08ae88a467c4187587936248e2b7c96283484b", None),
    "generation_config.json": (138, "8c970692323e3ea0e9b8b0a4dca79388d31226e41f83c9fd6014804280ebf6e8", None),
    "merges.txt": (1671839, "599bab54075088774b1733fde865d5bd747cbcc7a547c5bc12610e874e26f5e3", None),
    "model.safetensors": (988097824, None, "88c142557820ccad55bb59756bfcfcf891de9cc6202816bd346445188a0ed342"),
    "tokenizer.json": (7031645, "c0382117ea329cdf097041132f6d735924b697924d6f6fc3945713e96ce87539", None),
    "tokenizer_config.json": (7228, "c91efca15ceff6e9ee9424db58a6f59cd41294e550a86cbd07e3c1fb500b34f9", None),
    "vocab.json": (2776833, "ca10d7e9fb3ed18575dd1e277a2579c16d108e32f27439684afa0e10b1440910", None),
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(4 * 1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def git_blob_sha1(data: bytes) -> str:
    header = f"blob {len(data)}\0".encode("ascii")
    return hashlib.sha1(header + data).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def verify_model(root: Path) -> dict[str, object]:
    files = root / "qwen2.5-0.5b-base" / "files"
    metadata_path = root / "qwen2.5-0.5b-base" / "metadata" / "hf-model-revision.json"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    require(metadata.get("id") == MODEL_REPO, f"model id mismatch: {metadata.get('id')}")
    require(metadata.get("sha") == MODEL_REVISION, f"model revision mismatch: {metadata.get('sha')}")
    require(metadata.get("gated") is False and metadata.get("private") is False, "model must be public and ungated")
    siblings = {item["rfilename"]: item for item in metadata["siblings"]}
    observations = {}
    for name, (expected_size, expected_sha1, expected_lfs_sha256) in MODEL_FILES.items():
        path = files / name
        require(path.is_file(), f"missing model file: {path}")
        item = siblings.get(name)
        require(item is not None, f"file absent from HF metadata: {name}")
        require(item["size"] == expected_size, f"metadata byte mismatch: {name}")
        actual_size = path.stat().st_size
        require(actual_size == expected_size, f"download byte mismatch: {name}: {actual_size}")
        actual_sha256 = sha256(path)
        actual_sha1 = None
        if expected_lfs_sha256 is not None:
            require(actual_sha256 == expected_lfs_sha256, f"LFS digest mismatch: {name}")
            require(item.get("lfs", {}).get("sha256") == expected_lfs_sha256, f"HF LFS metadata mismatch: {name}")
        else:
            actual_sha1 = git_blob_sha1(path.read_bytes())
            require(actual_sha256 == expected_sha1, f"pinned SHA-256 mismatch: {name}")
            require(actual_sha1 == item.get("blobId"), f"HF Git blob mismatch: {name}")
        observations[name] = {"bytes": actual_size, "sha256": actual_sha256, "git_blob_sha1": actual_sha1}
    after = json.loads((root / "qwen2.5-0.5b-base" / "metadata" / "hf-model-revision-after.json").read_text(encoding="utf-8"))
    require(after.get("sha") == MODEL_REVISION and after["siblings"] == metadata["siblings"], "HF before/after file metadata changed")
    pointer = (root / "qwen2.5-0.5b-base" / "metadata" / "model.safetensors.lfs-pointer.txt").read_bytes()
    require(git_blob_sha1(pointer) == siblings["model.safetensors"]["blobId"], "LFS pointer Git blob mismatch")
    require(b"oid sha256:88c142557820ccad55bb59756bfcfcf891de9cc6202816bd346445188a0ed342\n" in pointer,
            "LFS pointer digest mismatch")
    require({p.name for p in files.iterdir()} == set(MODEL_FILES), "unexpected executable or additional model file")
    config = json.loads((files / "config.json").read_text(encoding="utf-8"))
    tokenizer_config = json.loads((files / "tokenizer_config.json").read_text(encoding="utf-8"))
    require(config.get("architectures") == ["Qwen2ForCausalLM"], "unexpected model architecture")
    require(config.get("model_type") == "qwen2", "unexpected model type")
    require(config.get("torch_dtype") == "bfloat16", "unexpected source dtype")
    require(config.get("vocab_size") == 151936, "unexpected vocabulary size")
    require(tokenizer_config.get("tokenizer_class") == "Qwen2Tokenizer", "unexpected tokenizer class")
    require("auto_map" not in tokenizer_config and "trust_remote_code" not in tokenizer_config, "remote-code marker present")
    return {"revision": MODEL_REVISION, "files": observations, "architecture": config["architectures"][0], "model_type": config["model_type"], "vocab_size": config["vocab_size"], "trust_remote_code": False}


def verify_source(root: Path) -> dict[str, object]:
    llama = root / "llama.cpp-b4514"
    archive = llama / f"llama.cpp-{LLAMA_COMMIT}.tar.gz"
    require(sha256(archive) == LLAMA_TAR_SHA256, "llama.cpp archive SHA-256 mismatch")
    commit = json.loads((llama / "metadata" / "github-commit.json").read_text(encoding="utf-8"))
    require(commit.get("sha") == LLAMA_COMMIT, "GitHub commit metadata mismatch")
    tree_doc = json.loads((llama / "metadata" / "github-tree.json").read_text(encoding="utf-8"))
    require(tree_doc.get("truncated") is False, "GitHub tree was truncated")
    source = llama / "source"
    expected_paths = set()
    ordinary = 0
    symlinks = 0
    for item in tree_doc["tree"]:
        if item["type"] == "blob":
            expected_paths.add(item["path"])
            path = source / item["path"]
            require(path.is_file(), f"missing extracted source file: {item['path']}")
            if item["mode"] == "120000":
                symlinks += 1
                continue
            ordinary += 1
            require(git_blob_sha1(path.read_bytes()) == item["sha"], f"source blob mismatch: {item['path']}")
        elif item["type"] == "commit":
            require(item["path"] == "ggml/src/ggml-kompute/kompute", "unexpected submodule")
            require(item["sha"] == "4565194ed7c32d1d2efa32ceab4d3c6cae006306", "Kompute submodule mismatch")
    actual_paths = {path.relative_to(source).as_posix() for path in source.rglob("*") if path.is_file()}
    require(actual_paths == expected_paths, f"source file set differs; extra={sorted(actual_paths - expected_paths)[:5]} missing={sorted(expected_paths - actual_paths)[:5]}")
    require(not list(source.rglob(".git")), "nested git directory found in acquired source")
    # Tie the saved tree metadata to bytes in the SHA-256-pinned archive as well.
    # GNU tar on Windows materializes these seven symlink targets as regular files.
    with tarfile.open(archive, "r:gz") as tar:
        for member in tar.getmembers():
            if not member.isfile() and not member.issym():
                continue
            relative = PurePosixPath(member.name).relative_to(f"llama.cpp-{LLAMA_COMMIT}")
            if member.issym():
                target = (source / relative.parent / member.linkname).resolve()
                require(target.is_relative_to(source.resolve()), "archive symlink escapes source")
                require((source / relative).read_bytes() == target.read_bytes(), f"materialized symlink differs: {relative}")
            else:
                stream = tar.extractfile(member)
                require(stream is not None, f"unreadable archive member: {relative}")
                require(hashlib.sha256(stream.read()).hexdigest() == sha256(source / relative),
                        f"source/archive bytes differ: {relative}")
    return {"commit": LLAMA_COMMIT, "archive_sha256": LLAMA_TAR_SHA256, "ordinary_blobs_verified": ordinary, "symlink_blobs_in_archive": symlinks, "tree_entries": len(tree_doc["tree"]), "submodule_not_initialized": True}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    args = parser.parse_args()
    repo = Path(args.repo).resolve()
    root = repo / "build" / "backend-model-acquire"
    result = {"offline": True, "network_access": False, "model": verify_model(root), "llama_cpp": verify_source(root)}
    out = repo / "build" / "backend-neural" / "metadata" / "resource-verification.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"resource verification failed: {error}", file=sys.stderr)
        raise SystemExit(1)
