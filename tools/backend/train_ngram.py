#!/usr/bin/env python3
"""Deterministically train the offline EpoCanva prediction TSV from local JSONL.

The tool is data preparation only. It never downloads corpora, reads user input,
or participates in runtime prediction. See docs/testing/backend-prediction-quality.md
for the corpus contract and reproducible commands.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import unicodedata
from collections import Counter
from pathlib import Path
from typing import Any, Iterable

MODEL_MAGIC = "EPOCANVA_PREDICTION_TSV\t1"
ID_SCHEME = "fnv1a64-language-normalized-v1"
NORMALIZATION = "ascii-lower-utf8-v1"
HASH_DOMAIN = b"epocanva.prediction.lexeme/v1/"
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
UINT64_MAX = (1 << 64) - 1
MAX_LEXEME_BYTES = 256
MAX_MODEL_BYTES = 64 * 1024 * 1024
SUPPORTED_LANGUAGES = frozenset(("en", "zh", "fr", "ja", "ko"))
LANGUAGE_TOKENIZER = {
    "en": "unicode-word-apostrophe-hyphen-v1",
    "fr": "unicode-word-apostrophe-hyphen-v1",
    "ko": "unicode-spacing-words-v1",
    "zh": "catalog-longest-match-or-codepoint-baseline-v1",
    "ja": "catalog-longest-match-or-codepoint-baseline-v1",
}
DEFAULT_MAX_INPUT_BYTES = 512 * 1024 * 1024
DEFAULT_MAX_RECORDS = 1_000_000
DEFAULT_MAX_TOKENS = 20_000_000
DEFAULT_MAX_UNIQUE_LEXEMES = 500_000
DEFAULT_MAX_UNIQUE_NGRAMS = 5_000_000


class TrainingError(ValueError):
    """An input, consistency, or resource-bound failure safe to report."""


def canonical_json(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True,
                       separators=(",", ":")) + "\n").encode("utf-8")


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def ascii_lower(text: str) -> str:
    """Match the core contract: lower ASCII A-Z only; preserve all other scalars."""
    return "".join(chr(ord(ch) + 32) if "A" <= ch <= "Z" else ch for ch in text)


def validate_lexeme(token: str) -> str:
    normalized = ascii_lower(token)
    try:
        encoded = normalized.encode("utf-8", errors="strict")
    except UnicodeEncodeError as exc:
        raise TrainingError("token contains a non-scalar Unicode value") from exc
    if not encoded or len(encoded) > MAX_LEXEME_BYTES:
        raise TrainingError("token is empty or exceeds the model's 256 UTF-8-byte limit")
    for ch in normalized:
        codepoint = ord(ch)
        if ch.isspace() or codepoint <= 0x1F or 0x7F <= codepoint <= 0x9F:
            raise TrainingError("token contains Unicode whitespace or a C0/C1 control")
    return normalized


def stable_lexeme_id(language: str, normalized: str) -> int:
    """FNV-1a-64 over domain + language + slash + normalized UTF-8 bytes."""
    if language not in SUPPORTED_LANGUAGES:
        raise TrainingError("unsupported language ID")
    if normalized != validate_lexeme(normalized):
        raise TrainingError("stable ID input is not normalized")
    payload = HASH_DOMAIN + language.encode("ascii") + b"/" + normalized.encode("utf-8")
    result = FNV_OFFSET
    for byte in payload:
        result ^= byte
        result = (result * FNV_PRIME) & UINT64_MAX
    return result or 1


def _is_mark(ch: str) -> bool:
    return unicodedata.category(ch).startswith("M")


def _is_word_char(ch: str) -> bool:
    return ch.isalpha() or ch.isdigit() or _is_mark(ch)


def _is_connector(ch: str) -> bool:
    return ch in ("'", "’", "-", "‐", "‑")


def _units_with_marks(text: str) -> list[str]:
    """Return scalar-plus-combining-mark units, not a claim of full graphemes."""
    units: list[str] = []
    for ch in text:
        if _is_mark(ch) and units:
            units[-1] += ch
        else:
            units.append(ch)
    return units


def _tokenize_spaced(text: str, *, letters_only: bool = False) -> list[tuple[str, str]]:
    """Unicode word runs and one-scalar punctuation; no casefold/NFC.

    English/French use letters plus combining marks. Korean uses all non-space
    word runs because spacing, not a Latin regex, is its explicit boundary.
    """
    tokens: list[tuple[str, str]] = []
    word: list[str] = []

    def is_word(ch: str) -> bool:
        return (ch.isalpha() or _is_mark(ch)) if letters_only else _is_word_char(ch)

    def flush() -> None:
        if word:
            tokens.append((validate_lexeme("".join(word)), "word"))
            word.clear()

    chars = list(text)
    for index, ch in enumerate(chars):
        if ch.isspace():
            flush()
            continue
        if is_word(ch):
            word.append(ch)
            continue
        if _is_connector(ch):
            previous_is_word = bool(word) and is_word(word[-1][-1])
            next_is_word = index + 1 < len(chars) and is_word(chars[index + 1])
            if previous_is_word and next_is_word:
                word.append(ch)
            else:
                flush()
                tokens.append((validate_lexeme(ch), "punct"))
            continue
        category = unicodedata.category(ch)
        if category.startswith("C"):
            raise TrainingError("text contains a non-whitespace Unicode control/format scalar")
        flush()
        tokens.append((validate_lexeme(ch), "punct"))
    flush()
    return tokens


def _segment_catalog_run(run: str, entries: set[str]) -> list[str]:
    """Greedy longest catalog match; unmatched text falls back by scalar+marks."""
    units = _units_with_marks(run)
    if not entries:
        return [validate_lexeme(unit) for unit in units]
    out: list[str] = []
    cursor = 0
    while cursor < len(units):
        matched_end = cursor
        # Every unit consumes at least one byte, and the model limits lexemes to
        # 256 UTF-8 bytes; keep pathological long unbroken inputs bounded.
        upper = min(len(units), cursor + MAX_LEXEME_BYTES)
        for end in range(upper, cursor, -1):
            candidate = "".join(units[cursor:end])
            if len(candidate.encode("utf-8")) <= MAX_LEXEME_BYTES and ascii_lower(candidate) in entries:
                matched_end = end
                break
        if matched_end == cursor:
            matched_end = cursor + 1
        out.append(validate_lexeme("".join(units[cursor:matched_end])))
        cursor = matched_end
    return out


def tokenize(text: str, language: str,
             catalog: dict[str, set[str]] | None = None) -> list[tuple[str, str]]:
    """Tokenize a corpus sentence under explicit per-language rules.

    English/French preserve Unicode letters and internal apostrophes/hyphens.
    Korean uses whitespace-delimited Unicode runs. Chinese/Japanese use the
    optional legal catalog's longest match, with a reported codepoint baseline
    for unmatched spans and when no catalog is supplied.
    """
    if language not in SUPPORTED_LANGUAGES:
        raise TrainingError("unsupported language ID: " + language)
    if language not in ("zh", "ja"):
        if language in ("en", "fr"):
            return _tokenize_spaced(text, letters_only=True)
        return _tokenize_spaced(text, letters_only=False)

    entries = (catalog or {}).get(language, set())
    result: list[tuple[str, str]] = []
    run: list[str] = []

    def flush_run() -> None:
        if run:
            for token in _segment_catalog_run("".join(run), entries):
                result.append((token, "word"))
            run.clear()

    for ch in text:
        if ch.isspace():
            flush_run()
        elif unicodedata.category(ch).startswith("P"):
            flush_run()
            result.append((validate_lexeme(ch), "punct"))
        elif unicodedata.category(ch).startswith("C"):
            raise TrainingError("text contains a non-whitespace Unicode control/format scalar")
        else:
            run.append(ch)
    flush_run()
    return result


def load_catalog(path: Path | None) -> tuple[dict[str, set[str]], str | None, int]:
    catalog: dict[str, set[str]] = {"zh": set(), "ja": set()}
    if path is None:
        return catalog, None, 0
    if not path.is_file():
        raise TrainingError("segmentation catalog is not a regular file")
    digest = sha256_file(path)
    entries = 0
    with path.open("rb") as stream:
        for line_number, raw in enumerate(stream, 1):
            if not raw.strip():
                continue
            try:
                item = json.loads(raw.decode("utf-8", errors="strict"))
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                raise TrainingError(f"catalog line {line_number} is not UTF-8 JSON") from exc
            if not isinstance(item, dict) or item.get("language") not in ("zh", "ja"):
                raise TrainingError(f"catalog line {line_number} needs language zh or ja")
            word = item.get("word")
            if not isinstance(word, str) or not word or any(ch.isspace() for ch in word):
                raise TrainingError(f"catalog line {line_number} needs a nonempty word without whitespace")
            normalized = validate_lexeme(word)
            catalog[item["language"]].add(normalized)
            entries += 1
    return catalog, digest, entries


def _required_text(record: dict[str, Any], name: str, line_number: int) -> str:
    value = record.get(name)
    if not isinstance(value, str) or not value.strip():
        raise TrainingError(f"corpus line {line_number} needs nonempty string field {name!r}")
    if any(unicodedata.category(ch) in ("Cc", "Cs") for ch in value):
        raise TrainingError(f"corpus line {line_number} field {name!r} contains a control")
    return value


def read_corpus(paths: Iterable[Path], catalog: dict[str, set[str]], *, max_bytes: int,
                max_records: int, max_tokens: int, max_lexemes: int,
                skip_invalid: bool = False) -> tuple[list[dict[str, Any]], str, dict[str, int]]:
    corpus_paths = list(paths)
    if not corpus_paths:
        raise TrainingError("at least one local corpus input is required")
    records: list[dict[str, Any]] = []
    seen_ids: set[tuple[str, str]] = set()
    vocab: set[tuple[str, str]] = set()
    digest = hashlib.sha256()
    input_manifest: list[dict[str, Any]] = []
    total_bytes = 0
    token_total = 0
    rejected_format: Counter = Counter()
    for input_ordinal, path in enumerate(corpus_paths):
        if not path.is_file():
            raise TrainingError("corpus input is not a regular local file: " + str(path))
        size = path.stat().st_size
        total_bytes += size
        if total_bytes > max_bytes:
            raise TrainingError(f"corpus inputs exceed --max-input-bytes ({max_bytes})")
        with path.open("rb") as stream:
            file_digest = hashlib.sha256()
            for line_number, raw in enumerate(stream, 1):
                file_digest.update(raw)
                digest.update(raw)
                if not raw.strip():
                    continue
                try:
                    item = json.loads(raw.decode("utf-8", errors="strict"))
                except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                    raise TrainingError(f"corpus {path} line {line_number} is not UTF-8 JSON") from exc
                if not isinstance(item, dict):
                    raise TrainingError(f"corpus {path} line {line_number} must be a JSON object")
                record_id = _required_text(item, "id", line_number)
                language = _required_text(item, "language", line_number)
                if language not in SUPPORTED_LANGUAGES:
                    raise TrainingError(f"corpus {path} line {line_number} has unsupported language ID")
                source = _required_text(item, "source", line_number)
                license_name = _required_text(item, "license", line_number)
                text = item.get("text")
                if not isinstance(text, str):
                    raise TrainingError(f"corpus {path} line {line_number} needs string field 'text'")
                if any(unicodedata.category(ch) in ("Cc", "Cs", "Cf") and not ch.isspace() for ch in text):
                    if skip_invalid:
                        rejected_format["non_whitespace_control_or_format"] += 1
                        continue
                    raise TrainingError(f"corpus {path} line {line_number} text contains a non-whitespace control/format scalar")
                attribution = item.get("attribution")
                if attribution is not None and not isinstance(attribution, (str, dict)):
                    raise TrainingError(f"corpus {path} line {line_number} attribution must be a string or object")
                if isinstance(attribution, dict) and any(not isinstance(key, str) or not isinstance(value, str)
                                                         for key, value in attribution.items()):
                    raise TrainingError(f"corpus {path} line {line_number} attribution object must contain string values")
                document = item.get("document_id", item.get("group_id", item.get("group")))
                if document is None:
                    document = item.get("author_id", item.get("username"))
                if document is not None and (not isinstance(document, str) or not document.strip()
                                             or any(unicodedata.category(ch) in ("Cc", "Cs") for ch in document)):
                    raise TrainingError(f"corpus {path} line {line_number} document/group/author must be a nonempty control-free string")
                id_key = (language, record_id)
                if id_key in seen_ids:
                    raise TrainingError(f"corpus {path} line {line_number} duplicates a language/id pair")
                seen_ids.add(id_key)
                try:
                    token_pairs = tokenize(text, language, catalog)
                except TrainingError as exc:
                    raise TrainingError(f"corpus {path} line {line_number}: {exc}") from exc
                token_total += len(token_pairs)
                if token_total > max_tokens:
                    raise TrainingError(f"token count exceeds --max-tokens ({max_tokens})")
                for token, _kind in token_pairs:
                    vocab.add((language, token))
                    if len(vocab) > max_lexemes:
                        raise TrainingError(f"vocabulary exceeds --max-unique-lexemes ({max_lexemes})")
                # Preserve the agreed fields and explicit grouping key for held-out outputs.
                clean: dict[str, Any] = {
                    "id": record_id,
                    "language": language,
                    "text": text,
                    "source": source,
                    "license": license_name,
                }
                if attribution is not None:
                    clean["attribution"] = attribution
                if document is not None:
                    clean["document_id"] = document
                records.append({
                    "id": record_id, "language": language, "text": text,
                    "source": source, "license": license_name,
                    "attribution": attribution,
                    "group": document if document is not None else source,
                    "tokens": token_pairs,
                    "canonical": clean,
                })
                if len(records) > max_records:
                    raise TrainingError(f"record count exceeds --max-records ({max_records})")
            # A NUL delimiter makes concatenated file bytes unambiguous while the
            # exact per-file input bytes remain available in the report's digest.
            digest.update(b"\0")
    if not records:
        raise TrainingError("corpus contains no nonblank JSONL records")
    return records, digest.hexdigest(), dict(sorted(rejected_format.items()))


def deduplicate_records(records: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], int]:
    """Keep one deterministic representative per language + normalized token sequence."""
    representatives: dict[tuple[str, tuple[tuple[str, str], ...]], dict[str, Any]] = {}
    duplicate_count = 0
    for record in records:
        key = (record["language"], tuple(record["tokens"]))
        previous = representatives.get(key)
        if previous is None:
            representatives[key] = record
            continue
        duplicate_count += 1
        current_rank = tuple(str(record[k]).encode("utf-8") for k in ("source", "license", "group", "id"))
        previous_rank = tuple(str(previous[k]).encode("utf-8") for k in ("source", "license", "group", "id"))
        if current_rank < previous_rank:
            representatives[key] = record
    return sorted(representatives.values(), key=lambda r: (r["language"].encode("ascii"), r["id"].encode("utf-8"))), duplicate_count


def split_for_group(group: str, seed: str, train_percent: int,
                    dev_percent: int) -> str:
    if not (1 <= train_percent <= 98 and 1 <= dev_percent <= 98
            and train_percent + dev_percent < 100):
        raise TrainingError("split percentages must leave nonzero train, dev, and test portions")
    bucket = int.from_bytes(hashlib.sha256(seed.encode("utf-8") + b"\0" + group.encode("utf-8")).digest()[:8], "big") % 10000
    if bucket < train_percent * 100:
        return "train"
    if bucket < (train_percent + dev_percent) * 100:
        return "dev"
    return "test"


def assign_splits(records: list[dict[str, Any]], seed: str,
                  train_percent: int = 80, dev_percent: int = 10) -> dict[str, list[dict[str, Any]]]:
    groups: dict[str, str] = {}
    result: dict[str, list[dict[str, Any]]] = {"train": [], "dev": [], "test": []}
    for record in records:
        # The prepared pipeline owns group namespaces and any cross-language
        # translation-family unions. Never split those units by language here.
        group = record["group"]
        if group not in groups:
            groups[group] = split_for_group(group, seed, train_percent, dev_percent)
        split = groups[group]
        record["split"] = split
        result[split].append(record)
    return result


def _checked_add(counter: Counter, key: Any, amount: int = 1) -> None:
    value = counter.get(key, 0) + amount
    if value > UINT64_MAX:
        raise TrainingError("an n-gram count exceeds uint64")
    counter[key] = value


def count_ngrams(records: Iterable[dict[str, Any]], max_unique_lexemes: int,
                 max_unique_ngrams: int) -> tuple[dict[int, tuple[str, str, str]], Counter,
                                                   Counter, Counter, dict[str, int]]:
    lexemes: dict[int, tuple[str, str, str]] = {}
    unigram: Counter = Counter()
    bigram: Counter = Counter()
    trigram: Counter = Counter()
    token_counts: Counter = Counter()
    for record in records:
        lang = record["language"]
        ids: list[int] = []
        for token, kind in record["tokens"]:
            lexeme_id = stable_lexeme_id(lang, token)
            existing = lexemes.get(lexeme_id)
            candidate = (lang, kind, token)
            if existing is not None and existing != candidate:
                raise TrainingError("FNV-1a-64 stable ID collision between distinct lexemes")
            lexemes[lexeme_id] = candidate
            ids.append(lexeme_id)
            _checked_add(unigram, lexeme_id)
            token_counts[lang] += 1
        _checked_add(unigram, 0)  # EOS is an observed target, never a lexicon entry.
        sequence = ids + [0]
        for index in range(1, len(sequence)):
            previous, target = sequence[index - 1], sequence[index]
            if previous == 0:
                raise TrainingError("EOS must never occur in n-gram history")
            _checked_add(bigram, (previous, target))
            if index >= 2:
                previous2 = sequence[index - 2]
                if previous2 == 0:
                    raise TrainingError("EOS must never occur in trigram history")
                _checked_add(trigram, (previous2, previous, target))
        if len(lexemes) > max_unique_lexemes:
            raise TrainingError(f"training vocabulary exceeds --max-unique-lexemes ({max_unique_lexemes})")
        if len(unigram) + len(bigram) + len(trigram) > max_unique_ngrams:
            raise TrainingError(f"distinct n-grams exceed --max-unique-ngrams ({max_unique_ngrams})")
    if not unigram or sum(unigram.values()) == 0:
        raise TrainingError("training split produced no positive unigram counts")
    return lexemes, unigram, bigram, trigram, dict(token_counts)


def prune_min_ngram_count(bigram: Counter, trigram: Counter,
                          minimum_count: int) -> tuple[Counter, Counter]:
    """Apply the frozen memory policy to B/T only, retaining all U/L rows.

    A T row is retained only when its own observed count and both denominator
    B rows meet the threshold. This is a data-independent admission rule,
    applied before the fixed held-out test is ever queried.
    """
    if minimum_count < 1:
        raise TrainingError("--min-ngram-count must be at least 1")
    kept_b = Counter({key: count for key, count in bigram.items() if count >= minimum_count})
    kept_t = Counter({key: count for key, count in trigram.items()
                      if count >= minimum_count
                      and (key[0], key[1]) in kept_b
                      and (key[1], key[2]) in kept_b})
    return kept_b, kept_t


def _validate_count_consistency(lexemes: dict[int, tuple[str, str, str]],
                                unigram: Counter, bigram: Counter,
                                trigram: Counter) -> None:
    if any(not 0 < count <= UINT64_MAX for count in unigram.values()):
        raise TrainingError("unigram rows must have positive uint64 counts")
    if any(not 0 < count <= UINT64_MAX for count in bigram.values()):
        raise TrainingError("bigram rows must have positive uint64 counts")
    if any(not 0 < count <= UINT64_MAX for count in trigram.values()):
        raise TrainingError("trigram rows must have positive uint64 counts")
    if not lexemes:
        raise TrainingError("model requires at least one lexical record")
    for table in (unigram, bigram, trigram):
        if sum(table.values()) > UINT64_MAX:
            raise TrainingError("aggregate n-gram total exceeds uint64")
    if 0 in lexemes:
        raise TrainingError("EOS id 0 must not have a lexicon row")
    for lexeme_id, (language, kind, text) in lexemes.items():
        if kind not in ("word", "punct") or lexeme_id != stable_lexeme_id(language, validate_lexeme(text)):
            raise TrainingError("invalid lexicon stable ID, normalized text, or token kind")
        if text != ascii_lower(text):
            raise TrainingError("lexicon text is not ASCII-lower normalized")
    if any(lexeme_id != 0 and lexeme_id not in lexemes for lexeme_id in unigram):
        raise TrainingError("unigram references an unknown lexicon ID")
    if 0 not in unigram:
        raise TrainingError("EOS must have a positive unigram target count")
    for (previous, target), count in bigram.items():
        if previous == 0 or previous not in unigram or target not in unigram:
            raise TrainingError("bigram has EOS/unknown history or a missing unigram denominator")
    outgoing: Counter = Counter()
    incoming: Counter = Counter()
    for (previous, target), count in bigram.items():
        outgoing[previous] += count
        incoming[target] += count
    for previous, total in outgoing.items():
        if total > unigram[previous]:
            raise TrainingError("aggregate outgoing bigrams exceed the previous unigram count")
    for target, total in incoming.items():
        if total > unigram[target]:
            raise TrainingError("aggregate incoming bigrams exceed the target unigram count")
    for (previous2, previous1, target), count in trigram.items():
        if previous2 == 0 or previous1 == 0 or (previous2, previous1) not in bigram:
            raise TrainingError("trigram has invalid history or missing prefix bigram denominator")
        if (previous1, target) not in bigram:
            raise TrainingError("trigram has missing suffix bigram denominator")
    prefix_totals: Counter = Counter()
    suffix_totals: Counter = Counter()
    for (previous2, previous1, target), count in trigram.items():
        prefix_totals[(previous2, previous1)] += count
        suffix_totals[(previous1, target)] += count
    for pair, total in prefix_totals.items():
        if total > bigram[pair]:
            raise TrainingError("aggregate trigram prefixes exceed the prefix bigram count")
    for pair, total in suffix_totals.items():
        if total > bigram[pair]:
            raise TrainingError("aggregate trigram suffixes exceed the suffix bigram count")


def _lexeme_row(item: tuple[int, tuple[str, str, str]]) -> bytes:
    lexeme_id, (language, kind, text) = item
    return f"L\t{lexeme_id}\t{language}\t{kind}\t{text}\n".encode("utf-8")


def _unigram_row(item: tuple[int, int]) -> bytes:
    lexeme_id, count = item
    return f"U\t{lexeme_id}\t{count}\n".encode("ascii")


def _bigram_row(item: tuple[tuple[int, int], int]) -> bytes:
    (previous, target), count = item
    return f"B\t{previous}\t{target}\t{count}\n".encode("ascii")


def _trigram_row(item: tuple[tuple[int, int, int], int]) -> bytes:
    (previous2, previous1, target), count = item
    return f"T\t{previous2}\t{previous1}\t{target}\t{count}\n".encode("ascii")


def serialize_model(lexemes: dict[int, tuple[str, str, str]], unigram: Counter,
                    bigram: Counter, trigram: Counter, source_sha256: str,
                    max_bytes: int = MAX_MODEL_BYTES) -> tuple[bytes, dict[str, Any]]:
    """Serialize canonical v1 TSV; prune deterministically if the file exceeds budget."""
    if not re.fullmatch(r"[0-9a-f]{64}", source_sha256):
        raise TrainingError("source_sha256 must be 64 lowercase hexadecimal characters")
    if max_bytes <= 0 or max_bytes > MAX_MODEL_BYTES:
        raise TrainingError("model byte budget must be between 1 and 64 MiB")
    _validate_count_consistency(lexemes, unigram, bigram, trigram)

    selected_u = set(unigram)
    selected_b = set(bigram)
    selected_t = set(trigram)
    pruned = {"lexemes": 0, "unigrams": 0, "bigrams": 0, "trigrams": 0}

    def rows_for(u_ids: set[int], b_keys: set[tuple[int, int]],
                 t_keys: set[tuple[int, int, int]]) -> tuple[list[bytes], list[bytes], list[bytes], list[bytes]]:
        # IDs are the canonical sort key in every table, matching model.cc's
        # strict tuple ordering. U(0) is the sole EOS boundary target row.
        lrows = [_lexeme_row((i, lexemes[i])) for i in sorted(u_ids) if i != 0]
        urows = [_unigram_row((i, unigram[i])) for i in sorted(u_ids)]
        brows = [_bigram_row((k, bigram[k])) for k in sorted(b_keys)]
        trows = [_trigram_row((k, trigram[k])) for k in sorted(t_keys)]
        return lrows, urows, brows, trows

    # Header hash has a fixed byte width; budget against exact UTF-8 row lengths.
    reserve = 1024
    payload_budget = max_bytes - reserve
    if payload_budget < 256:
        raise TrainingError("model byte budget is too small for a valid v1 model")
    all_rows = rows_for(selected_u, selected_b, selected_t)
    full_payload_size = sum(len(row) for group in all_rows for row in group)
    if full_payload_size > payload_budget:
        # Protect denominator tables in dependency order: choose U then B, and
        # only then T. Top-count rows win; numeric IDs break all ties.
        u_budget = payload_budget * 50 // 100
        b_budget = payload_budget * 30 // 100
        t_budget = payload_budget - u_budget - b_budget
        selected_u = set()
        eos = 0
        eos_bytes = len(_unigram_row((0, unigram[0])))
        selected_u.add(eos)
        used_u = eos_bytes
        lexical = sorted((i for i in unigram if i != 0), key=lambda i: (-unigram[i], i))
        for lexeme_id in lexical:
            cost = len(_lexeme_row((lexeme_id, lexemes[lexeme_id]))) + len(_unigram_row((lexeme_id, unigram[lexeme_id])))
            if used_u + cost <= u_budget:
                selected_u.add(lexeme_id)
                used_u += cost
        # Even if the reservation was smaller than one lexical record, keep the
        # highest-frequency lexical row when the total model budget allows it.
        if len(selected_u) == 1 and lexical:
            selected_u.add(lexical[0])
        selected_b = set()
        used_b = 0
        b_candidates = sorted(
            (key for key in bigram if key[0] in selected_u and key[1] in selected_u),
            key=lambda key: (-bigram[key], key[0], key[1]))
        for key in b_candidates:
            cost = len(_bigram_row((key, bigram[key])))
            if used_b + cost <= b_budget:
                selected_b.add(key)
                used_b += cost
        selected_t = set()
        used_t = 0
        t_candidates = sorted(
            (key for key in trigram
             if (key[0], key[1]) in selected_b and (key[1], key[2]) in selected_b),
            key=lambda key: (-trigram[key], key[0], key[1], key[2]))
        for key in t_candidates:
            cost = len(_trigram_row((key, trigram[key])))
            if used_t + cost <= t_budget:
                selected_t.add(key)
                used_t += cost
        # Fill unused reservations without changing table dependencies.
        remaining = payload_budget - sum(len(row) for group in rows_for(selected_u, selected_b, selected_t) for row in group)
        for key in b_candidates:
            if key in selected_b:
                continue
            cost = len(_bigram_row((key, bigram[key])))
            if cost <= remaining:
                selected_b.add(key)
                remaining -= cost
        b_set = selected_b
        for key in t_candidates:
            if key in selected_t or (key[0], key[1]) not in b_set or (key[1], key[2]) not in b_set:
                continue
            cost = len(_trigram_row((key, trigram[key])))
            if cost <= remaining:
                selected_t.add(key)
                remaining -= cost
        # Spend remaining space on unigram rows/lexemes, then try dependent rows
        # once more. EOS stays present and is always a target only.
        for lexeme_id in lexical:
            if lexeme_id in selected_u:
                continue
            cost = len(_lexeme_row((lexeme_id, lexemes[lexeme_id]))) + len(_unigram_row((lexeme_id, unigram[lexeme_id])))
            if cost <= remaining:
                selected_u.add(lexeme_id)
                remaining -= cost
        allowed_b = sorted(
            (key for key in bigram if key[0] in selected_u and key[1] in selected_u),
            key=lambda key: (-bigram[key], key[0], key[1]))
        for key in allowed_b:
            if key in selected_b:
                continue
            cost = len(_bigram_row((key, bigram[key])))
            if cost <= remaining:
                selected_b.add(key)
                remaining -= cost
        allowed_t = sorted(
            (key for key in trigram
             if (key[0], key[1]) in selected_b and (key[1], key[2]) in selected_b),
            key=lambda key: (-trigram[key], key[0], key[1], key[2]))
        for key in allowed_t:
            if key in selected_t:
                continue
            cost = len(_trigram_row((key, trigram[key])))
            if cost <= remaining:
                selected_t.add(key)
                remaining -= cost

    # Remove dependent n-grams before writing. Pruning selection above only adds
    # rows whose denominator rows survived; verify that invariant independently.
    selected_b = {k for k in selected_b if k[0] in selected_u and k[1] in selected_u}
    selected_t = {k for k in selected_t
                  if (k[0], k[1]) in selected_b and (k[1], k[2]) in selected_b}
    kept_lexemes = {i: lexemes[i] for i in selected_u if i != 0}
    kept_u = Counter({i: unigram[i] for i in selected_u})
    kept_b = Counter({k: bigram[k] for k in selected_b})
    kept_t = Counter({k: trigram[k] for k in selected_t})
    _validate_count_consistency(kept_lexemes, kept_u, kept_b, kept_t)
    pruned = {
        "lexemes": len(lexemes) - len(kept_lexemes),
        "unigrams": len(unigram) - len(kept_u),
        "bigrams": len(bigram) - len(kept_b),
        "trigrams": len(trigram) - len(kept_t),
    }
    lrows, urows, brows, trows = rows_for(set(kept_u), set(kept_b), set(kept_t))
    payload = b"".join(lrows + urows + brows + trows)
    metadata = {
        "lexemes": len(lrows),
        "unigram_total": sum(kept_u.values()),
        "bigram_total": sum(kept_b.values()),
        "trigram_total": sum(kept_t.values()),
    }
    metadata_lines = [
        MODEL_MAGIC,
        f"id_scheme\t{ID_SCHEME}",
        f"normalization\t{NORMALIZATION}",
        "source\tproduction-corpus",  # replaced by caller before hash/serialization
        f"source_sha256\t{source_sha256}",
        f"payload_sha256\t{sha256_bytes(payload)}",
        f"lexemes\t{metadata['lexemes']}",
        f"unigram_total\t{metadata['unigram_total']}",
        f"bigram_total\t{metadata['bigram_total']}",
        f"trigram_total\t{metadata['trigram_total']}",
        "data",
    ]
    header = ("\n".join(metadata_lines) + "\n").encode("ascii")
    model = header + payload
    if len(model) > max_bytes or len(model) > MAX_MODEL_BYTES:
        raise TrainingError("deterministic pruning could not fit the model byte budget")
    stats = {
        **metadata,
        "payload_sha256": sha256_bytes(payload),
        "payload_bytes": len(payload),
        "model_bytes": len(model),
        "pruned": pruned,
    }
    return model, stats


def model_bytes_for_source(lexemes: dict[int, tuple[str, str, str]], unigram: Counter,
                           bigram: Counter, trigram: Counter, source_kind: str,
                           source_sha256: str, max_bytes: int) -> tuple[bytes, dict[str, Any]]:
    model, stats = serialize_model(lexemes, unigram, bigram, trigram, source_sha256, max_bytes)
    # The source row is part of the fixed header but not the payload digest.
    model = model.replace(b"source\tproduction-corpus\n", f"source\t{source_kind}\n".encode("ascii"), 1)
    stats["model_bytes"] = len(model)
    if len(model) > max_bytes:
        raise TrainingError("source kind unexpectedly exceeded the model byte budget")
    return model, stats


def split_hash(records: list[dict[str, Any]]) -> str:
    digest = hashlib.sha256()
    for record in records:
        digest.update(canonical_json(record["canonical"]))
    return digest.hexdigest()


def eval_cases(test_records: list[dict[str, Any]]) -> list[dict[str, Any]]:
    cases: list[dict[str, Any]] = []
    for record in test_records:
        token_pairs = record["tokens"]
        for target_index, (target, _kind) in enumerate(token_pairs):
            context = [token for token, _ in token_pairs[max(0, target_index - 2):target_index]]
            previous2 = context[-2] if len(context) == 2 else ""
            previous1 = context[-1] if context else ""
            case_id = f"{record['language']}:{record['id']}:{target_index}"
            cases.append({
                "id": case_id,
                "language": record["language"],
                "context": context,
                "target_text": target,
                "prefix": "",
                "split": "test",
                "group_id": record["group"],
            })
    return cases


def eval_requests(cases: list[dict[str, Any]]) -> bytes:
    """Backend CLI request lines: id/lang/prev2/prev1/prefix as UTF-8 TSV."""
    lines: list[bytes] = []
    for case in cases:
        context = case["context"]
        previous2 = context[-2] if len(context) == 2 else ""
        previous1 = context[-1] if context else ""
        fields = (case["id"], case["language"], previous2, previous1, "")
        if any("\t" in field or "\r" in field or "\n" in field for field in fields):
            raise TrainingError("evaluation request field contains a TSV delimiter")
        lines.append(("\t".join(fields) + "\n").encode("utf-8"))
    return b"".join(lines)


def _atomic_write(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    try:
        with temporary.open("wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _split_jsonl(records: list[dict[str, Any]]) -> bytes:
    return b"".join(canonical_json(record["canonical"]) for record in records)


def train(args: argparse.Namespace) -> dict[str, Any]:
    corpus_paths = [Path(path) for path in args.input]
    catalog, catalog_sha256, catalog_rows = load_catalog(Path(args.catalog) if args.catalog else None)
    records, input_sha256, exclusions = read_corpus(
        corpus_paths, catalog, max_bytes=args.max_input_bytes,
        max_records=args.max_records, max_tokens=args.max_tokens,
        max_lexemes=args.max_unique_lexemes, skip_invalid=args.skip_invalid_records)
    deduped, duplicate_count = deduplicate_records(records)
    splits = assign_splits(deduped, args.seed, args.train_percent, args.dev_percent)
    if not splits["train"]:
        raise TrainingError("deterministic grouping assigned no records to train; use more groups or adjust split percentages")
    training_languages = {r["language"] for r in splits["train"]}
    if not training_languages:
        raise TrainingError("training split has no supported language")
    if args.source_kind == "production-corpus":
        for language in sorted(training_languages):
            count = sum(1 for record in splits["train"] if record["language"] == language)
            if count < 1001:
                raise TrainingError(
                    f"production-corpus requires at least 1001 deduplicated train records per language; {language} has {count}. Use test-fixture for fixtures."
                )
    lexemes, unigram, bigram, trigram, train_token_counts = count_ngrams(
        splits["train"], args.max_unique_lexemes, args.max_unique_ngrams)
    pre_threshold_rows = {"unigrams": len(unigram), "bigrams": len(bigram), "trigrams": len(trigram)}
    bigram, trigram = prune_min_ngram_count(bigram, trigram, args.min_ngram_count)
    threshold_pruned = {"unigrams": 0, "bigrams": pre_threshold_rows["bigrams"] - len(bigram),
                        "trigrams": pre_threshold_rows["trigrams"] - len(trigram)}
    provenance_sources = sorted({(r["source"], r["license"]) for r in deduped}, key=lambda pair: (pair[0].encode("utf-8"), pair[1].encode("utf-8")))
    attribution_digest = hashlib.sha256()
    attribution_values = {json.dumps(r["attribution"], ensure_ascii=False, sort_keys=True, separators=(",", ":"))
                          for r in deduped if r["attribution"] is not None}
    for attribution in sorted(attribution_values, key=lambda value: value.encode("utf-8")):
        attribution_digest.update(attribution.encode("utf-8") + b"\n")
    training_manifest = {
        "contract": "EPOCANVA_PREDICTION_TSV/1",
        "input_sha256": input_sha256,
        "input_files": [{"file": path.name, "sha256": sha256_file(path), "utf8_bytes": path.stat().st_size}
                        for path in corpus_paths],
        "exclusions": exclusions,
        "skip_invalid_records": args.skip_invalid_records,
        "min_ngram_count": args.min_ngram_count,
        "threshold_pruned_rows": threshold_pruned,
        "catalog_sha256": catalog_sha256,
        "source_kind": args.source_kind,
        "normalization": NORMALIZATION,
        "stable_id_scheme": ID_SCHEME,
        "seed": args.seed,
        "split_percentages": {"train": args.train_percent, "dev": args.dev_percent,
                              "test": 100 - args.train_percent - args.dev_percent},
        "train_split_sha256": split_hash(splits["train"]),
        "tokenizers": {lang: LANGUAGE_TOKENIZER[lang] for lang in sorted(training_languages)},
        "provenance": [{"source": source, "license": license_name}
                       for source, license_name in provenance_sources],
    }
    source_sha256 = sha256_bytes(canonical_json(training_manifest))
    model, model_stats = model_bytes_for_source(
        lexemes, unigram, bigram, trigram, args.source_kind,
        source_sha256, args.max_model_bytes)

    output_path = Path(args.output)
    _atomic_write(output_path, model)
    splits_dir = Path(args.splits_dir) if args.splits_dir else output_path.parent / (output_path.stem + "-splits")
    split_info: dict[str, Any] = {}
    for name in ("train", "dev", "test"):
        body = _split_jsonl(splits[name])
        split_info[name] = {
            "records": len(splits[name]),
            "groups": len({r["group"] for r in splits[name]}),
            "sha256": sha256_bytes(body),
            "input_token_count": sum(len(r["tokens"]) for r in splits[name]),
        }
        _atomic_write(splits_dir / f"{name}.jsonl", body)
    cases = eval_cases(splits["test"])
    eval_cases_path = Path(args.eval_cases_out) if args.eval_cases_out else splits_dir / "test-cases.jsonl"
    evaluation_requests_path = Path(args.evaluation_requests) if args.evaluation_requests else splits_dir / "evaluation-requests.tsv"
    _atomic_write(eval_cases_path, b"".join(canonical_json(case) for case in cases))
    _atomic_write(evaluation_requests_path, eval_requests(cases))

    retained_ids = {int(field) for field in re.findall(rb"^L\t([0-9]+)\t", model.split(b"data\n", 1)[1], re.M)}
    oov_by_split: dict[str, dict[str, Any]] = {}
    for name in ("train", "dev", "test"):
        per_language: dict[str, dict[str, int]] = {}
        for record in splits[name]:
            lang = record["language"]
            stats = per_language.setdefault(lang, {"tokens": 0, "oov_tokens": 0, "distinct_oov": set()})
            for token, _kind in record["tokens"]:
                stats["tokens"] += 1
                if stable_lexeme_id(lang, token) not in retained_ids:
                    stats["oov_tokens"] += 1
                    stats["distinct_oov"].add(token)
        oov_by_split[name] = {
            lang: {**vals, "distinct_oov": len(vals["distinct_oov"]),
                   "oov_rate": (vals["oov_tokens"] / vals["tokens"] if vals["tokens"] else 0.0)}
            for lang, vals in sorted(per_language.items())
        }
    report: dict[str, Any] = {
        "tool": "train_ngram.py",
        "tool_source_sha256": sha256_file(Path(__file__)),
        "python_unicode_version": unicodedata.unidata_version,
        "training_manifest": training_manifest,
        "source_kind": args.source_kind,
        "source_sha256": source_sha256,
        "input_sha256": input_sha256,
        "input_files": [{"file": path.name, "sha256": sha256_file(path), "utf8_bytes": path.stat().st_size}
                        for path in corpus_paths],
        "exclusions": exclusions,
        "skip_invalid_records": args.skip_invalid_records,
        "min_ngram_count": args.min_ngram_count,
        "threshold_pruned_rows": threshold_pruned,
        "catalog_sha256": catalog_sha256,
        "catalog_rows": catalog_rows,
        "tokenizer": {lang: LANGUAGE_TOKENIZER[lang] for lang in sorted(training_languages)},
        "segmentation_quality": {
            lang: ("dictionary-assisted longest-match; unmatched spans use codepoint baseline"
                   if lang in ("zh", "ja") and catalog[lang]
                   else "partial codepoint baseline; not full language tokenization"
                   if lang in ("zh", "ja")
                   else "explicit Unicode word/spacing tokenizer")
            for lang in sorted(training_languages)
        },
        "provenance": {
            "source_license_pairs": [{"source": source, "license": license_name}
                                      for source, license_name in provenance_sources],
            "distinct_attribution_count": len(attribution_values),
            "attribution_sha256": attribution_digest.hexdigest(),
            "authenticity_claim": False,
        },
        "splits": split_info,
        "deduplication": {"input_records": len(records), "retained_records": len(deduped),
                           "duplicate_records_removed": duplicate_count,
                           "dedup_key": "language + normalized token sequence"},
        "train_token_counts_by_language": dict(sorted(train_token_counts.items())),
        "model": model_stats,
        "oov_by_split": oov_by_split,
        "evaluation_cases": {"path": str(eval_cases_path), "count": len(cases),
                              "format": "id/language/context/target_text/group_id JSONL"},
        "evaluation_requests": {"path": str(evaluation_requests_path), "count": len(cases),
                                "format": "id\\tlanguage\\tprevious2_text\\tprevious1_text\\tprefix TSV; max_words=1/max_suggestions=5"},
        "limits": {"max_input_bytes": args.max_input_bytes, "max_records": args.max_records,
                   "max_tokens": args.max_tokens, "max_unique_lexemes": args.max_unique_lexemes,
                   "max_unique_ngrams": args.max_unique_ngrams,
                   "max_model_bytes": args.max_model_bytes},
    }
    report_path = Path(args.report) if args.report else output_path.with_suffix(".report.json")
    _atomic_write(report_path, (json.dumps(report, ensure_ascii=False, sort_keys=True,
                                           indent=2) + "\n").encode("utf-8"))
    print(f"trained {args.source_kind}: {model_stats['lexemes']} lexemes, "
          f"{model_stats['unigram_total']} unigram count, "
          f"{model_stats['bigram_total']} bigram count, "
          f"{model_stats['trigram_total']} trigram count; {model_stats['model_bytes']} bytes")
    print(f"splits: train={split_info['train']['records']} dev={split_info['dev']['records']} test={split_info['test']['records']}; report={report_path}")
    if any(lang in ("zh", "ja") and not catalog[lang] for lang in training_languages):
        print("quality note: Chinese/Japanese codepoint fallback is partial, not full lexical segmentation", file=sys.stderr)
    return report


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", action="append", required=True, help="local UTF-8 JSONL corpus; repeat for multiple files; no network access")
    parser.add_argument("--output", default="build/backend-training/model.tsv", help="TSV output under build/<preset>")
    parser.add_argument("--report", help="JSON provenance/quality report (default: sibling .report.json)")
    parser.add_argument("--splits-dir", help="directory for deterministic train/dev/test JSONL outputs")
    parser.add_argument("--eval-cases-out", help="held-out next-token evaluation cases JSONL")
    parser.add_argument("--evaluation-requests", help="5-column TSV requests for the actual backend CLI")
    parser.add_argument("--catalog", help="optional legal zh/ja JSONL catalog with {language,word} rows")
    parser.add_argument("--source-kind", required=True, choices=("production-corpus", "test-fixture"))
    parser.add_argument("--skip-invalid-records", action="store_true", help="exclude whole records with control/format scalars, with report counts; never repair text")
    parser.add_argument("--seed", default="epocanva-ngram-v1")
    parser.add_argument("--train-percent", type=int, default=80)
    parser.add_argument("--dev-percent", type=int, default=10)
    parser.add_argument("--max-input-bytes", type=int, default=DEFAULT_MAX_INPUT_BYTES)
    parser.add_argument("--max-records", type=int, default=DEFAULT_MAX_RECORDS)
    parser.add_argument("--max-tokens", type=int, default=DEFAULT_MAX_TOKENS)
    parser.add_argument("--max-unique-lexemes", type=int, default=DEFAULT_MAX_UNIQUE_LEXEMES)
    parser.add_argument("--max-unique-ngrams", type=int, default=DEFAULT_MAX_UNIQUE_NGRAMS)
    parser.add_argument("--max-model-bytes", type=int, default=MAX_MODEL_BYTES)
    parser.add_argument("--min-ngram-count", type=int, default=1,
                        help="minimum observed count for B/T rows; U/L are never threshold-pruned")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    for field in ("max_input_bytes", "max_records", "max_tokens", "max_unique_lexemes", "max_unique_ngrams", "min_ngram_count"):
        if getattr(args, field) <= 0:
            parser.error(f"--{field.replace('_', '-')} must be positive")
    try:
        train(args)
    except (OSError, TrainingError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
