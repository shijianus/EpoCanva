"""Generate clearly labelled, deterministic test-only TSV model fixtures.

Uses Python hashlib as an independent SHA-256 oracle. Never reads a corpus,
user input, settings, profile or network; outputs only to the supplied build path.
This is NOT the production training tool or a production model.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import sys


def stable_id(language: str, text: str) -> int:
    value = 14695981039346656037
    for byte in ("epocanva.prediction.lexeme/v1/" + language + "/" + text).encode("utf-8"):
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value or 1


def main() -> int:
    if len(sys.argv) != 2:
        return 2
    out = Path(sys.argv[1]).resolve()
    build_root = Path(__file__).resolve().parents[2] / "build"
    if not out.is_relative_to(build_root) or out == build_root:
        raise ValueError("fixture output must stay under this repository's build/<preset>")
    out.mkdir(parents=True, exist_ok=True)
    source = hashlib.sha256(b"EpoCanva prediction-core isolated TEST FIXTURE v1\n").hexdigest()
    tokens = [("en", "hello", "word", 40), ("en", "world", "word", 30),
              ("en", "!", "punct", 20), ("fr", "café", "word", 10),
              ("zh", "你好", "word", 5), ("ja", "世界", "word", 4),
              ("ko", "안녕", "word", 3), ("en", "unseen", "word", 0)]
    rows = sorted((stable_id(language, text), language, kind, text, count)
                  for language, text, kind, count in tokens)
    hello, world, punct = [stable_id("en", text) for text in ("hello", "world", "!")]
    unigrams = sorted([(0, 20)] + [(row[0], row[4]) for row in rows if row[4]])
    bigrams = sorted([(hello, world, 25), (world, punct, 15), (punct, 0, 10)])
    trigrams = [(hello, world, punct, 12)]
    payload = "".join(f"L\t{i}\t{language}\t{kind}\t{text}\n" for i, language, kind, text, _ in rows)
    payload += "".join(f"U\t{i}\t{count}\n" for i, count in unigrams)
    payload += "".join(f"B\t{a}\t{b}\t{count}\n" for a, b, count in bigrams)
    payload += "".join(f"T\t{a}\t{b}\t{c}\t{count}\n" for a, b, c, count in trigrams)
    payload_bytes = payload.encode("utf-8")
    headers = ["EPOCANVA_PREDICTION_TSV\t1", "id_scheme\tfnv1a64-language-normalized-v1",
               "normalization\tascii-lower-utf8-v1", "source\ttest-fixture",
               f"source_sha256\t{source}", "payload_sha256\tunused", f"lexemes\t{len(rows)}",
               f"unigram_total\t{sum(count for _, count in unigrams)}",
               f"bigram_total\t{sum(row[-1] for row in bigrams)}",
               f"trigram_total\t{sum(row[-1] for row in trigrams)}", "data"]

    def encode(body: bytes = payload_bytes, replacements: dict[int, str] | None = None) -> bytes:
        lines = headers.copy()
        lines[5] = "payload_sha256\t" + hashlib.sha256(body).hexdigest()
        if replacements:
            for ordinal, value in replacements.items():
                lines[ordinal] = value
        return ("\n".join(lines) + "\n").encode("utf-8") + body

    def write(name: str, data: bytes) -> None:
        (out / (name + ".tsv")).write_bytes(data)

    valid = encode()
    write("valid-test-only", valid)
    write("bad-version", encode(replacements={0: "EPOCANVA_PREDICTION_TSV\t2"}))
    write("bad-id-version", encode(replacements={1: "id_scheme\tfnv1a64-v2"}))
    write("bad-normalizer", encode(replacements={2: "normalization\tnfc-casefold"}))
    write("bad-source-kind", encode(replacements={3: "source\tunknown"}))
    write("bad-source-fingerprint", encode(replacements={4: "source_sha256\tnot-a-hash"}))
    write("bad-payload-fingerprint", encode(replacements={5: "payload_sha256\t" + "0" * 64}))
    write("bad-count-total", encode(replacements={7: "unigram_total\t999"}))
    write("bad-lexeme-total", encode(replacements={6: "lexemes\t999"}))
    write("bad-integer-overflow", encode(replacements={7: "unigram_total\t18446744073709551616"}))
    write("bad-leading-zero", encode(replacements={6: "lexemes\t08"}))
    write("bad-bom", b"\xef\xbb\xbf" + valid)
    write("bad-crlf", valid.replace(b"\n", b"\r\n"))
    write("bad-final-lf", valid[:-1])
    write("bad-binary", b"EPOCANVA_PREDICTION_BIN\t1\n\x00\x01")
    write("bad-nul", valid.replace(b"hello", b"he\x00lo"))
    write("bad-utf8", encode(payload_bytes.replace("café".encode(), b"caf\xc0\xaf")))
    write("bad-normalized", encode(payload_bytes.replace(b"hello", b"Hello")))
    write("bad-empty-line", encode(payload_bytes + b"\n"))
    payload_lines = payload.splitlines(keepends=True)
    duplicate = payload_lines.copy()
    duplicate.insert(1, duplicate[0])
    write("bad-duplicate", encode("".join(duplicate).encode()))
    unsorted = payload_lines.copy()
    unsorted[0], unsorted[1] = unsorted[1], unsorted[0]
    write("bad-order", encode("".join(unsorted).encode()))
    write("bad-unknown-id", encode((payload + "T\t18446744073709551615\t" + str(world) + "\t0\t1\n").encode(),
                                  {9: "trigram_total\t13"}))
    write("bad-zero-count", encode(payload_bytes.replace(f"U\t0\t20\n".encode(), b"U\t0\t0\n")))
    write("bad-zero-history", encode(payload_bytes.replace(f"B\t{hello}\t{world}\t25".encode(),
                                                          f"B\t0\t{world}\t25".encode())))
    write("bad-columns", encode(payload_bytes.replace(b"\tword\t", b"\tword\textra\t", 1)))
    # Source and stable-ID answers are computed independently of the C++ library.
    answers = ["# isolated test-only identity answers", source]
    answers += [f"{language}\t{text}\t{i}" for i, language, _, text, _ in rows]
    (out / "identity-answers.txt").write_text("\n".join(answers) + "\n", encoding="utf-8", newline="\n")
    (out / "fixtures.stamp").write_text("test-only fixtures generated\n", encoding="ascii", newline="\n")
    print(f"Generated {len(list(out.glob('*.tsv')))} isolated test-only TSV fixtures")
    return 0


if __name__ == "__main__":
    sys.exit(main())
