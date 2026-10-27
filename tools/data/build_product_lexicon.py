#!/usr/bin/env python3
"""EpoCanva W1 -- build the product lexicon from the pinned public-domain source.

WHAT THIS IS, AND WHAT IT IS NOT
    This tool produces the word list that ships in the internal-alpha package. It runs
    the SAME deterministic conversion as the P2.4 pilot tool (imported, not copied) over
    the SAME pinned ESDB Alt12Dicts source, but it writes a product artifact with its own
    provenance file and its own output identity.

    It is NOT a copy of build/english-pilot/epocanva_en_pilot.tsv. The pilot output is
    test-only. This tool never reads it, never trusts it, and never packages it.

IDENTITY
    The source checkout must be a git worktree whose HEAD equals the commit recorded in
    data/manifests/english-pilot.sources.lock.json and whose status is clean. The input
    file is then hash-checked again by the shared converter before a single row is
    parsed. A missing git, a different HEAD or a dirty tree is a hard failure.

OFFLINE
    This tool never fetches. It reads an existing checkout supplied with --source-root.
    Network access is not needed and is not attempted.

EXIT CODES
    0 success
    2 usage, or --check without an existing output
    3 manifest unreadable
    4 source checkout missing, not a git worktree, or git unavailable
    5 source identity (HEAD / clean tree / file hash / line endings) mismatch
    6 --check found different bytes
    7 the conversion accepted no word
"""

from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import build_english_pilot as pilot  # noqa: E402

MANIFEST_DEFAULT = os.path.join("data", "manifests", "english-pilot.sources.lock.json")
LEXICON_NAME = "epocanva_en_alpha.tsv"
PROVENANCE_NAME = "epocanva_en_alpha.provenance.txt"
GENERATOR_NAME = "tools/data/build_product_lexicon.py"


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def git_output(root: str, args: list[str]) -> str:
    try:
        completed = subprocess.run(
            ["git", "-C", root] + args,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as exc:
        sys.stderr.write("error: git is unavailable: %s\n" % exc)
        raise SystemExit(4)
    if completed.returncode != 0:
        sys.stderr.write(
            "error: git %s failed in the source checkout: %s\n"
            % (" ".join(args), completed.stderr.decode("utf-8", "replace").strip())
        )
        raise SystemExit(4)
    return completed.stdout.decode("utf-8", "strict").strip()


def verify_checkout(source_root: str, manifest: dict) -> str:
    if not os.path.isdir(source_root):
        sys.stderr.write("error: source checkout does not exist: %s\n" % source_root)
        raise SystemExit(4)
    head = git_output(source_root, ["rev-parse", "HEAD"])
    if head != manifest["pinned_commit"]:
        sys.stderr.write(
            "error: source HEAD is %s, manifest pins %s\n"
            % (head, manifest["pinned_commit"])
        )
        raise SystemExit(5)
    dirty = git_output(source_root, ["status", "--porcelain"])
    if dirty:
        sys.stderr.write(
            "error: the source checkout is not clean; refusing to build product data "
            "from a modified tree\n"
        )
        raise SystemExit(5)
    return head


def provenance_text(manifest: dict, manifest_path: str, manifest_sha256: str,
                    source_head: str, source_file: str, source_sha256: str,
                    source_bytes: int, source_rows: int, report: dict,
                    output_sha256: str, output_bytes: int, output_rows: int) -> str:
    input_info = manifest["inputs"][0]
    licence = manifest["licence"]
    lines = []
    lines.append("# EpoCanva product lexicon provenance (W1 internal alpha).")
    lines.append("# Deterministic output; no timestamp, hostname or absolute path.")
    lines.append("# This is not the P2.4 pilot artifact and is not derived from it.")
    lines.append("epocanva-product-lexicon-provenance 1")
    lines.append("artifact=%s" % LEXICON_NAME)
    lines.append("artifact_sha256=%s" % output_sha256)
    lines.append("artifact_bytes=%d" % output_bytes)
    lines.append("artifact_rows=%d" % output_rows)
    lines.append("generator=%s" % GENERATOR_NAME)
    lines.append("source_manifest=%s" % manifest_path.replace(os.sep, "/"))
    lines.append("source_manifest_sha256=%s" % manifest_sha256)
    lines.append("source_commit=%s" % source_head)
    lines.append("source_tag=%s" % manifest.get("pinned_tag", ""))
    lines.append("source_file=%s" % source_file.replace(os.sep, "/"))
    lines.append("source_sha256=%s" % source_sha256)
    lines.append("source_bytes=%d" % source_bytes)
    lines.append("source_rows=%d" % source_rows)
    lines.append("accepted_rows=%d" % report["accepted_rows"])
    lines.append("excluded_rows=%d" % report["excluded_rows"])
    for reason, count in sorted(report["exclusions_by_reason"].items()):
        lines.append("excluded.%s=%d" % (reason, count))
    for form, count in sorted(report["accepted_forms"].items()):
        lines.append("accepted.%s=%d" % (form, count))
    lines.append("weight_values=%s" % ",".join(report["weight_histogram"].keys()))
    lines.append("weight_meaning=coarse-attestation-rank-not-corpus-frequency")
    lines.append("licence_basis=public-domain-with-acknowledgment-request")
    lines.append("licence_file=%s" % licence["reviewed_file"].replace(os.sep, "/"))
    lines.append("licence_lines=%s" % licence["statement_lines"])
    lines.append("licence_sha256=%s" % licence["reviewed_file_sha256"])
    lines.append("agid_carve_out=honoured-no-agid-derived-input")
    lines.append("open_decision=redistribution-outside-internal-alpha-needs-owner-legal-review")
    lines.append("pilot_output_used=no")
    lines.append("")
    lines.append("# Source statement (verbatim, %s:%s):" %
                 (licence["reviewed_file"].replace(os.sep, "/"), licence["statement_lines"]))
    for wrapped in licence["statement_verbatim"].splitlines():
        lines.append("#   %s" % wrapped)
    lines.append("")
    return "\n".join(lines)


def write_atomic(path: str, data: bytes) -> None:
    directory = os.path.dirname(path)
    if directory:
        os.makedirs(directory, exist_ok=True)
    handle, temp = tempfile.mkstemp(prefix=".epocanva-product-", dir=directory or None)
    try:
        with os.fdopen(handle, "wb") as stream:
            stream.write(data)
        os.replace(temp, path)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="build_product_lexicon.py")
    parser.add_argument("--manifest", default=MANIFEST_DEFAULT)
    parser.add_argument("--source-root", default="")
    parser.add_argument("--out-dir", default=os.path.join("build", "product-data"))
    parser.add_argument("--check", action="store_true")
    opts = parser.parse_args(argv[1:])

    if not opts.source_root:
        sys.stderr.write(
            "error: --source-root is required. This tool never fetches the source.\n"
            "       Use the existing pinned checkout, for example "
            "build/third-party-fetch/esdb.\n"
        )
        return 2

    manifest_path = os.path.abspath(opts.manifest)
    if not os.path.isfile(manifest_path):
        sys.stderr.write("error: manifest not found: %s\n" % opts.manifest)
        return 3
    manifest_bytes = open(manifest_path, "rb").read()
    manifest_sha256 = sha256_bytes(manifest_bytes)
    manifest = pilot.load_manifest(manifest_path)

    source_root = os.path.abspath(opts.source_root)
    source_head = verify_checkout(source_root, manifest)
    source_rel = manifest["inputs"][0]["repo_path"]
    source_file = os.path.join(source_root, source_rel.replace("/", os.sep))
    if not os.path.isfile(source_file):
        sys.stderr.write("error: pinned source file is missing: %s\n" % source_file)
        return 4
    source_sha256 = pilot.sha256_file(source_file)
    source_bytes = os.path.getsize(source_file)

    rows, report = pilot.convert(source_file, manifest)
    body = "".join("%s\t%d\n" % (word, weight) for word, weight in rows)
    body_bytes = body.encode("ascii")
    output_sha256 = sha256_bytes(body_bytes)
    provenance = provenance_text(
        manifest,
        os.path.relpath(manifest_path, REPO_ROOT).replace(os.sep, "/"),
        manifest_sha256,
        source_head,
        source_rel,
        source_sha256,
        source_bytes,
        report["source_rows"],
        report,
        output_sha256,
        len(body_bytes),
        len(rows),
    ).encode("utf-8")

    lexicon_path = os.path.join(opts.out_dir, LEXICON_NAME)
    provenance_path = os.path.join(opts.out_dir, PROVENANCE_NAME)

    if opts.check:
        if not os.path.isfile(lexicon_path) or not os.path.isfile(provenance_path):
            sys.stderr.write(
                "error: --check compares against stored output, and one of these does "
                "not exist:\n  %s\n  %s\n" % (lexicon_path, provenance_path)
            )
            return 2
        if open(lexicon_path, "rb").read() != body_bytes:
            sys.stderr.write("error: stored lexicon is not byte-identical to this build\n")
            return 6
        if open(provenance_path, "rb").read() != provenance:
            sys.stderr.write(
                "error: stored provenance is not byte-identical to this build\n"
            )
            return 6
        sys.stdout.write("check: product lexicon is byte-identical (%d rows)\n" % len(rows))
        return 0

    write_atomic(lexicon_path, body_bytes)
    write_atomic(provenance_path, provenance)
    sys.stdout.write(
        "wrote %s (%d rows, %d bytes, sha256 %s)\n"
        % (lexicon_path, len(rows), len(body_bytes), output_sha256)
    )
    sys.stdout.write(
        "source %s at %s; accepted %d, excluded %d\n"
        % (source_rel, source_head, report["accepted_rows"], report["excluded_rows"])
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
