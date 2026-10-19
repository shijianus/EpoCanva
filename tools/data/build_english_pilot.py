#!/usr/bin/env python3
"""EpoCanva P2.4 -- build the English pilot lexicon from the pinned ESDB source.

WHAT THIS IS
    A one-shot, deterministic conversion from the official English Speller
    Database (SourceForge project `wordlist`, repo mount `code`) Alt12Dicts
    package into the `word<TAB>weight` format the compiled-in
    EpoCanvaEnglishTranslator loads. It is a data-preparation tool, not part of
    any runtime: nothing in engine/ imports it and the engine never runs Python.

DETERMINISM (P2.4 brief boundary 5, and the reason `--check` exists)
    Two runs over the same pinned inputs must produce byte-identical output.
    To keep that promise: no timestamp, no absolute path and no hostname is
    written into the TSV or the report; rows are emitted in ascending byte order
    of the canonical word (which is what the engine's index sorts by anyway, so
    a shuffle in the source file cannot reorder the output); every integer is
    formatted in one agreed way; and the file is written with LF endings.

OFFLINE BY DEFAULT (brief boundary 4 and 5)
    Reading a source tree is the default and touches no network. Fetching is
    opt-in (`--fetch`), performs a read-only `git clone`/`git checkout` of the
    commit named in the manifest, and NEVER sends local content anywhere: the
    only outbound bytes are the repo's own ref negotiation and object download.
    Without `--fetch` a missing checkout is a hard, explained failure -- the
    tool will not quietly produce a different word list.

NO CLEANING INTO THE INDEX (brief boundary 8)
    A row whose word is outside the accepted form is EXCLUDED and counted under
    the reason that excludes it. Nothing is lowercased, de-punctuated or
    truncated to make it fit. `Aachen` stays out; it is not turned into
    `aachen`. The exclusion tally is part of the output, because "how much of
    the source did you throw away, and why" is the only way this file can be
    audited after the fact.

WEIGHT MEANING (brief boundary 9)
    weight = the number of Alan Beale's source dictionaries that list the word
    (2..12 in the accepted rows of this release; the leading integer of each
    2of12full.txt row -- the file is defined as words appearing in MORE THAN ONE
    source dictionary, so a weight of 1 does not occur and that is a cross-check
    that the right field was read). It
    is a COARSE PROVENANCE RANK: how widely a word is attested across the
    compiled dictionaries. It is NOT a corpus word frequency, NOT an n-gram
    probability and NOT tuned to any input-method usage data. The generated
    header comment... this format has no comment syntax, so the statement lives
    in docs/compliance/english-pilot-data.md and the manifest instead.

EXIT CODES
    0  success
    2  usage / bad arguments, or --check with no stored output to compare against
    3  manifest missing or unreadable
    4  source checkout missing and --fetch not given
    5  the pinned source is not what the manifest describes: SHA-256, byte count,
       line count, CRLF pair count, a bare CR surviving CRLF removal, or a row
       that does not match the pinned file format
    6  network use was requested or needed but is unavailable, or the fetch
       produced a commit that is not the pinned one
    7  the conversion accepted nothing (the exclusion tally is printed on stderr
       so the reason is visible), or --check produced bytes different from the
       stored output
    --self-check returns 1 when a classifier case disagrees with the hand table.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

MANIFEST_DEFAULT = os.path.join("data", "manifests", "english-pilot.sources.lock.json")
ROW_RE = re.compile(r"^ *(\d+): +(\S+) +(\S+) +(\S+) +(\S+) +(.*)$")

# The accepted complete-entry form is implemented below as classify_exclusion,
# which mirrors engine/languages/en/english_token.h IsDictionaryEntryForm. The
# engine's loader is the authority at run time: --self-check holds this file to a
# hand-written table, and the pilot integration test loads the result in the real
# engine, so a drift between the two definitions fails loudly instead of showing
# up as a word list that loads but never completes.
MAX_WORD_BYTES = 64

EXCLUSION_REASONS = (
    "contains-space",
    "digit-or-other-punctuation",
    "capitalised-letter",
    "leading-or-trailing-delimiter",
    "more-than-one-delimiter",
    "non-ascii-byte",
    "word-too-long",
    "duplicate-word",
)


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def classify_exclusion(word: str) -> str | None:
    """None means the word is accepted. Otherwise the one reason it is not.

    Order matters and is documented in the compliance note: a word can look
    like several things at once (`St.-James` is capitalised AND has an odd
    delimiter placement) and the report must be reproducible, so the first
    matching rule in this fixed sequence is the recorded reason.
    """
    if not word.isascii():
        return "non-ascii-byte"
    if len(word.encode("utf-8")) > MAX_WORD_BYTES:
        return "word-too-long"
    if " " in word:
        return "contains-space"
    if any(not (ch.isalpha() or ch in "'-") for ch in word):
        return "digit-or-other-punctuation"
    if not word.islower():
        return "capitalised-letter"
    if word[0] in "'-" or word[-1] in "'-":
        return "leading-or-trailing-delimiter"
    if word.count("'") + word.count("-") > 1:
        return "more-than-one-delimiter"
    return None


def load_manifest(path: str) -> dict:
    if not os.path.isfile(path):
        sys.stderr.write("error: manifest not found: %s\n" % path)
        raise SystemExit(3)
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def verify_source(source_file: str, expectation: dict) -> None:
    """Fail loudly unless the file on disk is the file the manifest describes."""
    if not os.path.isfile(source_file):
        sys.stderr.write(
            "error: pinned source file is missing: %s\n"
            "       run with --fetch to clone the pinned commit, or point\n"
            "       --source-root at an existing ESDB checkout.\n" % source_file)
        raise SystemExit(4)
    digest = sha256_file(source_file)
    if digest != expectation["sha256"]:
        sys.stderr.write(
            "error: %s\n  sha256 %s\n  manifest %s\n"
            "       the checkout is not the pinned release; refusing to convert.\n"
            % (source_file, digest, expectation["sha256"]))
        raise SystemExit(5)
    raw = open(source_file, "rb").read()
    got_lines = raw.count(b"\n")
    if got_lines != expectation["line_count"] or len(raw) != expectation["size_bytes"]:
        sys.stderr.write(
            "error: %s\n  observed %d bytes / %d newlines, manifest %d / %d\n"
            % (source_file, len(raw), got_lines, expectation["size_bytes"],
               expectation["line_count"]))
        raise SystemExit(5)


def fetch_checkout(cache_dir: str, remote: str, commit: str) -> str:
    """Read-only clone of exactly the pinned commit. Sends nothing but requests."""
    repo = os.path.join(cache_dir, "esdb")
    git = os.environ.get("GIT", "git")
    def run(args: list[str], where: str | None = None) -> None:
        sys.stderr.write("fetch: %s\n" % " ".join(args))
        try:
            proc = subprocess.run(args, cwd=where, shell=False)
        except OSError as error:
            sys.stderr.write("error: cannot run %s (%s); no software is installed"
                             " by this tool. %s\n" % (args[0], error,
                                                      "re-run with GIT=<path>"))
            raise SystemExit(6)
        if proc.returncode != 0:
            sys.stderr.write("error: %s exited %d\n" % (args[0], proc.returncode))
            raise SystemExit(6)
    if not os.path.isdir(os.path.join(repo, ".git")):
        os.makedirs(cache_dir, exist_ok=True)
        run([git, "clone", "--no-checkout", remote, repo])
    run([git, "fetch", "--depth", "1", "origin", commit], where=repo)
    run([git, "checkout", "--force", "--detach", commit], where=repo)
    head = subprocess.run([git, "rev-parse", "HEAD"], cwd=repo, capture_output=True,
                          text=True)
    observed = head.stdout.strip()
    if head.returncode != 0 or observed != commit:
        sys.stderr.write("error: checkout HEAD is %r, manifest pins %r\n"
                         % (observed, commit))
        raise SystemExit(6)
    print("fetch: pinned commit present: %s" % observed)
    return repo


def convert(source_file: str, manifest: dict) -> tuple[list[tuple[str, int]], dict]:
    expectations = {item["repo_path"]: item for item in manifest["inputs"]}
    verify_source(source_file, expectations[manifest["inputs"][0]["repo_path"]])
    raw = open(source_file, "rb").read()
    # The pinned release ships CRLF. That pairing is verified against the
    # manifest, and after removing it NO bare CR may remain: a lone CR would be
    # an unexpected terminator that silently shortens a row, which is exactly the
    # kind of drift this tool refuses instead of absorbing.
    expected_crlf = manifest["inputs"][0]["crlf_pairs"]
    crlf_pairs = raw.count(b"\r\n")
    if crlf_pairs != expected_crlf:
        sys.stderr.write("error: %d CRLF pairs observed, manifest records %d\n"
                         % (crlf_pairs, expected_crlf))
        raise SystemExit(5)
    if b"\r" in raw.replace(b"\r\n", b""):
        sys.stderr.write("error: a bare CR survives CRLF removal; the line-ending\n"
                         "       contract of the pinned file has changed.\n")
        raise SystemExit(5)
    text = raw.replace(b"\r\n", b"\n").decode("ascii", errors="strict")
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()

    rows: dict[str, int] = {}
    tally = dict((reason, 0) for reason in EXCLUSION_REASONS)
    forms = {"plain-letter-run": 0, "with-apostrophe": 0, "with-hyphen": 0}
    unparsed: list[str] = []
    weight_hist: dict[str, int] = {}
    for line in lines:
        match = ROW_RE.match(line)
        if not match:
            unparsed.append(line)
            continue
        weight = int(match.group(1))
        word = match.group(6)
        reason = classify_exclusion(word)
        if reason is not None:
            tally[reason] += 1
            continue
        if "'" in word:
            forms["with-apostrophe"] += 1
        elif "-" in word:
            forms["with-hyphen"] += 1
        else:
            forms["plain-letter-run"] += 1
        if word in rows:
            # Same word twice in the source: keep the stronger attestation and
            # count the event rather than silently overwriting either way.
            tally["duplicate-word"] += 1
            rows[word] = max(rows[word], weight)
        else:
            rows[word] = weight
        weight_hist[str(weight)] = weight_hist.get(str(weight), 0) + 1
    if unparsed:
        sys.stderr.write("error: %d row(s) did not match the pinned file format, "
                         "first: %r\n       the parser and the source have drifted; "
                         "refusing to guess.\n" % (len(unparsed), unparsed[0][:80]))
        raise SystemExit(5)

    accepted = len(rows)
    if accepted == 0:
        # An empty result is almost always a contract drift rather than an empty
        # source, so the tally that produced it is printed instead of hidden.
        sys.stderr.write("error: nothing was accepted. exclusion tally: %s\n"
                         % json.dumps(tally, sort_keys=True))
        raise SystemExit(7)
    report = {
        "source_rows": len(lines),
        "source_rows_unparsed": 0,
        "accepted_rows": accepted,
        "excluded_rows": len(lines) - accepted,
        "exclusions_by_reason": tally,
        "accepted_forms": forms,
        "weight_histogram": dict(sorted(weight_hist.items(), key=lambda kv: int(kv[0]))),
        "min_word_bytes": min(len(w) for w in rows),
        "max_word_bytes": max(len(w) for w in rows),
        "weights_are_corpus_frequency": False,
    }
    out = sorted(rows.items(), key=lambda kv: kv[0].encode("utf-8"))
    return out, report


def self_check() -> int:
    """Compare this file's classifier against a table derived by hand."""
    cases = [
        ("hello", None), ("don't", None), ("co-operate", None), ("a", None),
        ("'hello", "leading-or-trailing-delimiter"),
        ("hello'", "leading-or-trailing-delimiter"),
        ("word-", "leading-or-trailing-delimiter"),
        ("rock'n'roll", "more-than-one-delimiter"),
        ("a-b-c", "more-than-one-delimiter"),
        ("don't-x", "more-than-one-delimiter"),
        ("Aachen", "capitalised-letter"),
        ("Co-Operate", "capitalised-letter"),
        ("a1b", "digit-or-other-punctuation"),
        ("a.", "digit-or-other-punctuation"),
        ("a b", "contains-space"),
        ("caf\xe9", "non-ascii-byte"),
        ("a" * 65, "word-too-long"),
    ]
    failed = 0
    for word, wanted in cases:
        got = classify_exclusion(word)
        if got != wanted:
            failed += 1
            sys.stdout.write("self-check FAIL %-14r got %r want %r\n"
                             % (word, got, wanted))
    sys.stdout.write("self-check: %d cases, %d failed\n" % (len(cases), failed))
    return 1 if failed else 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="build_english_pilot.py")
    parser.add_argument("--manifest", default=MANIFEST_DEFAULT)
    parser.add_argument("--source-root", default=None,
                        help="root of an existing ESDB checkout")
    parser.add_argument("--cache-dir", default=os.path.join("build", "third-party-fetch"),
                        help="where --fetch clones the pinned commit")
    parser.add_argument("--out-dir", default=os.path.join("build", "english-pilot"),
                        help="where the lexicon TSV and the report are written")
    parser.add_argument("--fetch", action="store_true",
                        help="allow the read-only clone of the pinned commit")
    parser.add_argument("--check", action="store_true",
                        help="re-convert and require byte-identical existing output")
    parser.add_argument("--self-check", action="store_true",
                        help="verify the exclusion classifier against a hand table")
    opts = parser.parse_args(argv[1:])

    if opts.self_check:
        return self_check()

    manifest = load_manifest(opts.manifest)
    source_rel = manifest["inputs"][0]["repo_path"]
    if opts.fetch:
        if opts.source_root:
            sys.stderr.write("error: --fetch and --source-root are mutually "
                             "exclusive: --fetch decides the checkout itself.\n")
            return 2
        root = fetch_checkout(opts.cache_dir, manifest["remote"]["clone_url"],
                              manifest["pinned_commit"])
    else:
        if not opts.source_root:
            sys.stderr.write(
                "error: no --source-root given and --fetch was not requested.\n"
                "       this tool does not touch the network unless asked (P2.4\n"
                "       brief boundary 5). Either point --source-root at the\n"
                "       pinned checkout, or re-run with --fetch.\n")
            return 4
        root = opts.source_root
    source_file = os.path.join(root, source_rel.replace("/", os.sep))

    rows, report = convert(source_file, manifest)
    body = "".join("%s\t%d\n" % (word, weight) for word, weight in rows)
    out_dir = opts.out_dir
    os.makedirs(out_dir, exist_ok=True)
    lexicon_path = os.path.join(out_dir, manifest["output"]["lexicon_file"])
    report_path = os.path.join(out_dir, manifest["output"]["report_file"])
    payload = json.dumps({"exclusions": report, "rows": len(rows)},
                         sort_keys=True, indent=2, ensure_ascii=False) + "\n"

    if opts.check:
        if not os.path.isfile(lexicon_path):
            # Reporting this as "not deterministic" would blame the wrong thing:
            # there is nothing to compare against yet.
            sys.stderr.write("error: --check compares against the stored output, and "
                             "%s does not exist. Run the conversion without --check "
                             "first.\n" % lexicon_path)
            return 2
        previous = open(lexicon_path, "rb").read()
        if previous != body.encode("ascii"):
            sys.stderr.write("error: --check found different bytes than the stored "
                             "output; the conversion is not deterministic.\n")
            return 7
        sys.stdout.write("check: output is byte-identical on a second run (%d rows)\n"
                         % len(rows))
        return 0

    with open(lexicon_path, "wb") as handle:
        handle.write(body.encode("ascii"))
    with open(report_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(payload)
    sys.stdout.write("wrote %s (%d rows, %d bytes) and %s\n"
                     % (lexicon_path, len(rows), len(body), report_path))
    sys.stdout.write("source rows %d -> accepted %d, excluded %d\n"
                     % (report["source_rows"], report["accepted_rows"],
                        report["excluded_rows"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
