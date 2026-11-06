#!/usr/bin/env python3
"""Stage reviewed, local backend resources into an exact build/<preset>/... root.

Standard library only. No downloads, subprocesses, installs, runtime changes, or
implicit output-root selection. See docs/testing/backend-resource-staging.md.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import re
import shutil
import stat
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

LANGUAGES = ("en", "zh", "fr", "ja", "ko")
RESOURCE_MAGIC = "epocanva-backend-resources\t1"
# The only authored profile/backend catalogue is engine/locale/locale_profile.cc.
# That registry exports this deterministic manifest; this script never keeps a
# second copy of profile ids, enum values, variants or schemes.
BACKEND_BINDINGS_PATH = "data/locale/backend-bindings.json"
BACKEND_BINDINGS_OWNER = "epocanva-locale-backend-bindings-v1"
BACKEND_BINDINGS_FORMAT = 1
BACKEND_BINDING_FIELDS = frozenset((
    "numeric_id", "locale_profile", "language_tag", "language", "language_value",
    "input_mode", "input_mode_value", "output_mode", "output_mode_value",
    "keyboard_layout", "keyboard_layout_value", "variant_id", "scheme_id",
    "scheme_implementation"))
RESOURCE_COLUMNS = (
    "profile_id language input_mode output_mode keyboard_layout variant_id "
    "model_path model_sha256 conversion_path conversion_sha256 auxiliary_path "
    "auxiliary_sha256 abbreviation_path abbreviation_sha256"
).split()
MAX_RESOURCE_ROWS = 32
MAX_RESOURCE_UTF8_BYTES = 64 * 1024
MAX_ABBREVIATION_UTF8_BYTES = 8 * 1024 * 1024
MAX_ABBREVIATION_RECORDS = 16384
MAX_ABBREVIATION_TRIE_NODES = 131072
SOURCE_PRESETS = frozenset(("backend-training-budget", "backend-japanese-data", "backend-chinese-ranked",
                            "backend-data-acquire", "backend-corpus-prepared", "backend-french-dependency"))
MANIFEST_PATH = "data/backend/staging-manifest.json"
OWNER = "epocanva-local-backend-resource-staging-v1"

# Anchors are hashes of the existing verified experiment REPORTS, not hashes
# learned from whichever resource happens to be present at staging time. A new
# experiment requires explicit review and an update here, never --trust-current.
REVIEWED_REPORTS = {
    "build/backend-training-budget/summary.json": "2f9911319c4ba65bb2f4f79d683eb5f3da6e4d5aad8e6e33dd723ceb8f4684d2",
    "build/backend-training-budget/en/report.json": "97f7c84f1651fe4434b635ae64b6d7cb692ce2170f2173948a675eefe6a37c73",
    "build/backend-training-budget/zh/report.json": "fcd5d70e27af89d356a17291afeaf550630e38a4390177e41327c15ccbd850ec",
    "build/backend-training-budget/fr/report.json": "dae4bf76765b2080a55439a1f150c841e72d5b17592431b63d376e414cc08d4a",
    "build/backend-training-budget/ja/report.json": "9501fb2acd388b0a11e7fa9b981e80c2cf52dd221669e8c80fc752b91277b564",
    "build/backend-training-budget/ko/report.json": "b42d751fe7f1ee08f0d6c017dfb969de73a123d1438ca43b701247ca62ce88cf",
    "build/backend-japanese-data/lexicon.manifest.json": "29dd7b503ea602847e62e949e21faafba48792e188423db2d6d6866094bbfd86",
    "build/backend-chinese-ranked/manifest.json": "91b3a11bcab8164694a66f3b3c5979580034705faf84a6eb5969dcdacef61a23",
}
BLOCKERS = [
    "Internal local staging only; public distribution is not approved by the source records or this tool.",
    "Terra dictionary CC-BY-SA-3.0 attribution/share-alike compatibility and mixed recipe/data obligations require artifact-specific review.",
    "Tatoeba author/URL/license attribution is retained; upstream authorship and rights are not independently authenticated. Final attribution and license delivery require review.",
    "Mozc dictionary includes IPAdic/NAIST, ICOT and Okinawa notices; preserve its mixed data terms and full source notices.",
    "French data remains MPL-2.0. Static Hunspell needs corresponding source and relinkable application materials in the final source release.",
    "Product executable/DLL inventory, package policy, Rime deployment, host tests, installation and source-release assembly are separate integration gates.",
    "Models retain corpus-baseline limits: Chinese character tokens, Japanese approximate dictionary segmentation, and limited Korean corpus size. No en-GB or Hans/Hant variant data is supplied.",
]


class StagingError(ValueError):
    """An admission, provenance, or output conflict; no automatic repair."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise StagingError(message)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def json_bytes(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode("utf-8")


def canonical_attribution(value: Any) -> bytes:
    require(isinstance(value, dict) and all(isinstance(value.get(k), str) and value[k]
            for k in ("author", "sentence_url", "license_url")), "missing corpus attribution")
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def relative_path(value: str) -> str:
    """Portable product/repository path; reject Windows aliases on every OS."""
    require(isinstance(value, str) and bool(value), "empty relative path")
    require("\\" not in value and not value.startswith("/"), "path must be relative with forward slashes")
    parts = value.split("/")
    for part in parts:
        require(part not in ("", ".", "..") and not part.endswith((".", " ")),
                "non-canonical path component")
        require(not re.search(r'[\x00-\x1f\x7f:<>"|?*]', part), "invalid path component")
        require(not re.fullmatch(r"(?i:con|prn|aux|nul|com[0-9]|lpt[0-9])(?:\..*)?", part),
                "reserved Windows path component")
    return value


def local_absolute(value: str | Path, base: Path) -> Path:
    text = str(value).replace("\\", "/")
    require(not text.startswith("//") and not text.startswith("/??/"), "network/device path refused")
    require(not re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*://", text), "URL refused")
    if re.match(r"^[A-Za-z]:/", text):
        require(os.name == "nt", "Windows drive path unavailable on this platform")
        relative_path(text[3:])
    elif text.startswith("/"):
        require(os.name != "nt", "drive-qualified local path required")
        relative_path(text[1:])
    else:
        relative_path(text)
    path = Path(text)
    return path if path.is_absolute() else base / path


def check_local_path(path: Path) -> None:
    """Inspect each existing ancestor before any content read or child creation.

    Resident OneDrive cloud tags are allowed; symlinks, junctions, other reparses
    and recall-on-access files are refused. This is preparation-time admission,
    not an OS sandbox or protection from a hostile ancestor-replacement race.
    """
    require(path.is_absolute(), "absolute local path required")
    if os.name == "nt":
        require(bool(re.fullmatch(r"[A-Za-z]:\\", path.anchor)), "ordinary local drive required")
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        query = kernel.QueryDosDeviceW
        query.argtypes = (ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32)
        query.restype = ctypes.c_uint32
        buffer = ctypes.create_unicode_buffer(32768)
        require(bool(query(path.drive, buffer, len(buffer))) and
                bool(re.fullmatch(r"\\Device\\HarddiskVolume[0-9]+", buffer.value)),
                "mapped/network/substituted drive refused")
    current = Path(path.anchor)
    for part in (None, *path.parts[1:]):
        if part is not None:
            current /= part
        try:
            info = current.lstat()
        except FileNotFoundError:
            break
        require(not stat.S_ISLNK(info.st_mode), f"symlink refused: {current}")
        attrs = getattr(info, "st_file_attributes", 0)
        tag = getattr(info, "st_reparse_tag", 0)
        if attrs & 0x400:  # FILE_ATTRIBUTE_REPARSE_POINT
            cloud = (tag & ~0x0000F000) == 0x9000001A  # IO_REPARSE_TAG_CLOUD[_n]
            require(cloud and not tag & 0x20000000, f"junction/reparse path refused: {current}")
        require(not attrs & (0x1000 | 0x40000 | 0x400000), f"cloud recall/offline path refused: {current}")
        require(stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode), f"non-file path refused: {current}")
        if current != path:
            require(stat.S_ISDIR(info.st_mode), f"non-directory ancestor: {current}")


def validate_product_root(repo: Path, value: str | Path) -> Path:
    target = local_absolute(value, repo)
    try:
        parts = target.relative_to(repo).parts
    except ValueError as exc:
        raise StagingError("product root must be an exact descendant of repository build/<preset>/") from exc
    require(len(parts) >= 3 and parts[0] == "build", "product root must be below build/<preset>/, not that directory itself")
    require(parts[1].lower() not in SOURCE_PRESETS, "product root cannot be inside a resource source tree")
    check_local_path(target)
    require(not target.exists() or target.is_dir(), "product root is not a directory")
    return target


def read_local(path: Path, maximum_bytes: int | None = None) -> bytes:
    check_local_path(path)
    require(path.is_file(), f"required local file missing: {path}")
    if maximum_bytes is not None:
        require(path.stat().st_size <= maximum_bytes, f"file exceeds {maximum_bytes} bytes: {path}")
    return path.read_bytes()


def file_digest(path: Path) -> tuple[str, int]:
    check_local_path(path)
    require(path.is_file(), f"required local file missing: {path}")
    digest = hashlib.sha256()
    size_bytes = 0
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
            size_bytes += len(chunk)
    return digest.hexdigest(), size_bytes


@dataclass(frozen=True)
class Payload:
    sha256: str
    size_bytes: int
    source: Path | None = None
    content: bytes | None = None


class Inventory:
    def __init__(self, repo: Path):
        self.repo = repo
        self.inputs: dict[str, dict[str, Any]] = {}
        self.files: dict[str, Payload] = {}

    def source(self, path: str, digest: str | None, size_bytes: int | None, authority: str) -> None:
        relative_path(path)
        source = self.repo / path
        actual, size = file_digest(source)
        if digest is not None:
            require(bool(re.fullmatch("[0-9a-f]{64}", digest)), f"invalid expected SHA-256: {path}")
            require(actual == digest, f"SHA-256 mismatch: {path} (expected {digest}, got {actual})")
        if size_bytes is not None:
            require(size == size_bytes, f"byte-count mismatch: {path}")
        record = {"path": path, "sha256": actual, "bytes": size, "authority": authority}
        if path in self.inputs:
            require(self.inputs[path]["sha256"] == actual, f"source changed during verification: {path}")
        else:
            self.inputs[path] = record
        return None

    def authored(self, path: str) -> bytes:
        self.source(path, None, None, "repository-authored input; digest records reproducibility, not upstream authentication")
        data = read_local(self.repo / path, 8 * 1024 * 1024)
        require(sha256(data) == self.inputs[path]["sha256"], f"source changed: {path}")
        return data

    def checked_json(self, path: str, digest: str | None = None) -> dict[str, Any]:
        if digest is None:
            data = self.authored(path)
        else:
            self.source(path, digest, None, "reviewed report SHA-256 pinned in tools/backend/stage_resources.py")
            data = read_local(self.repo / path, 4 * 1024 * 1024)
            require(sha256(data) == digest, f"report changed: {path}")
        result = json.loads(data)
        require(isinstance(result, dict), f"JSON object required: {path}")
        return result

    def generated(self, destination: str, data: bytes) -> None:
        relative_path(destination)
        require(destination not in self.files, f"duplicate staging path: {destination}")
        self.files[destination] = Payload(sha256(data), len(data), content=data)

    def copy(self, source: str, destination: str) -> None:
        relative_path(destination)
        require(source in self.inputs, f"unverified copy source: {source}")
        record = self.inputs[source]
        require(destination not in self.files, f"duplicate staging path: {destination}")
        self.files[destination] = Payload(record["sha256"], record["bytes"], self.repo / source)

    def evidence(self, source: str) -> None:
        self.copy(source, "data/backend/provenance/" + source)


def strict_lines(data: bytes) -> list[str]:
    require(data.endswith(b"\n") and not data.startswith(b"\xef\xbb\xbf") and
            b"\r" not in data and b"\0" not in data, "strict UTF-8/LF text required (no BOM, CR, NUL)")
    return data.decode("utf-8", errors="strict")[:-1].split("\n")


def inspect_model(data: bytes, language: str, report: dict[str, Any]) -> dict[str, int]:
    require(len(data) == report["model"]["model_bytes"], "model byte count differs from report")
    # Units below count UTF-8 code units (bytes); no slicing of decoded tokens.
    header_bytes, separator, payload = data.partition(b"data\n")
    require(bool(separator), "missing model payload")
    headers = strict_lines(header_bytes)
    require(len(headers) == 10 and headers[0] == "EPOCANVA_PREDICTION_TSV\t1", "invalid model header")
    values = dict(line.split("\t") for line in headers)
    require(values["id_scheme"] == "fnv1a64-language-normalized-v1" and
            values["normalization"] == "ascii-lower-utf8-v1" and
            values["source"] == report["source_kind"] == "production-corpus", "production corpus model required")
    require(values["source_sha256"] == report["source_sha256"], "model source/report mismatch")
    require(values["payload_sha256"] == report["model"]["payload_sha256"] == sha256(payload), "model payload/report mismatch")
    for name in ("unigram_total", "bigram_total", "trigram_total"):
        require(int(values[name]) == report["model"][name], f"model {name} differs from report")
    words: dict[str, int] = {}
    seen_ids: set[int] = set()
    for raw in payload.splitlines():
        if not raw.startswith(b"L\t"):
            break
        fields = raw.decode("utf-8").split("\t")
        require(len(fields) == 5 and fields[2] == language and fields[3] in ("word", "punct"), "invalid model lexeme row")
        require(bool(re.fullmatch(r"[1-9][0-9]*", fields[1])), "non-canonical model ID")
        identifier = int(fields[1])
        require(identifier <= (1 << 64) - 1 and identifier not in seen_ids, "invalid/duplicate model ID")
        seen_ids.add(identifier)
        if fields[3] == "word":
            require(fields[4] not in words, "duplicate model word")
            words[fields[4]] = identifier
    require(len(seen_ids) == int(values["lexemes"]) == report["model"]["lexemes"], "model lexical-record count mismatch")
    require(bool(words), "model has no words")
    return words


def key_components(text: str, *, key: bool) -> list[str]:
    components = text.split(" ")
    require(1 <= len(components) <= 8 and all(components), "expected 1..8 space-separated nonempty components")
    require(not re.search(r"[A-Z]", text), "ASCII-lowercase canonical definitions required")
    for token in components:
        require(len(token.encode("utf-8")) <= 256 and
                not any(ch.isspace() or ord(ch) < 32 or 127 <= ord(ch) <= 159 for ch in token),
                "invalid component or component exceeds 256 UTF-8 bytes")
    require(len(text.encode("utf-8")) <= (512 if key else 4096), "definition exceeds UTF-8 byte bound")
    return components


def compile_abbreviations(data: bytes, language: str, words: dict[str, int], *,
                          oov: str = "skip") -> tuple[bytes, dict[str, Any]]:
    require(oov in ("skip", "error"), "unknown OOV policy")
    require(language in LANGUAGES, "unsupported abbreviation language")
    require(len(data) <= MAX_ABBREVIATION_UTF8_BYTES, "definition source exceeds byte bound")
    lines = strict_lines(data)
    require(len(lines) >= 2 and lines[:2] == ["EPOCANVA_ABBREVIATION_DEFINITIONS_TSV\t1",
            "language\tkey\texpansion\tevidence"], "invalid abbreviation definition header")
    rows: list[tuple[str, tuple[int, ...]]] = []
    skipped = []
    seen = set()
    for line_ordinal, line in enumerate(lines[2:], 3):
        fields = line.split("\t")
        require(len(fields) == 4 and fields[0] == language and fields[3], f"invalid abbreviation definition at line {line_ordinal}")
        key_components(fields[1], key=True)
        tokens = key_components(fields[2], key=False)
        identity = (fields[1], fields[2])
        require(identity not in seen, "duplicate abbreviation definition")
        seen.add(identity)
        missing = [token for token in tokens if token not in words]
        if missing:
            require(oov != "error", f"OOV abbreviation {language}/{fields[1]}: {' '.join(missing)}")
            skipped.append({"key": fields[1], "expansion": fields[2], "missing_model_words": missing})
            continue
        identifiers = tuple(words[token] for token in tokens)
        require(all(0 < value <= (1 << 64) - 1 for value in identifiers), "invalid model word ID")
        rows.append((fields[1], identifiers))
    require(len(rows) <= MAX_ABBREVIATION_RECORDS, "abbreviation resource exceeds record bound")
    # One language-root node plus one node per distinct Unicode-scalar prefix.
    # Counting prefixes bounds actual native trie capacity, not UTF-8 bytes.
    prefixes = set()
    for key, _identifiers in rows:
        prefix = ""
        for scalar in key:
            prefix += scalar
            prefixes.add(prefix)
            require(len(prefixes) + 1 <= MAX_ABBREVIATION_TRIE_NODES, "abbreviation trie exceeds node bound")
    result = "EPOCANVA_ABBREVIATIONS_TSV\t1\n"
    for key, identifiers in sorted(rows, key=lambda row: (row[0].encode("utf-8"), row[1])):
        record = f"{language}\t{key}\t{' '.join(map(str, identifiers))}\n"
        require(len(record.encode("utf-8")) - 1 <= 4096, "abbreviation record exceeds 4096 UTF-8 bytes")
        result += record
    content = result.encode("utf-8")
    require(len(content) <= MAX_ABBREVIATION_UTF8_BYTES, "abbreviation resource exceeds byte bound")
    return content, {"accepted_records": len(rows), "skipped_oov": skipped,
                     "partial_key_support": "canonical full keys populate the decoder prefix trie"}


def api_enums(data: bytes) -> dict[str, int]:
    values = {}
    for name, number in re.findall(rb"\b(EPOCANVA_BACKEND_[A-Z_]+)\s*=\s*([0-9]+)\b", data):
        key = name.decode("ascii")
        require(key not in values, f"duplicate API enum {key}")
        values[key] = int(number)
    return values


def binding_rows(inv: Inventory) -> list[dict[str, Any]]:
    """Read the locale registry's exported catalogue; never re-declare it.

    The JSON is generated by the C++ locale registry and checked in so this
    build tool works without a prior native build. A CTest target regenerates it
    and fails when the committed bytes are stale.
    """
    return parse_binding_manifest(inv.authored(BACKEND_BINDINGS_PATH))


def parse_binding_manifest(data: bytes) -> list[dict[str, Any]]:
    try:
        manifest = json.loads(data)
    except json.JSONDecodeError as exc:
        raise StagingError(f"invalid backend binding manifest: {exc}") from exc
    require(isinstance(manifest, dict), "backend binding manifest must be an object")
    require(manifest.get("format") == BACKEND_BINDINGS_FORMAT, "unsupported backend binding manifest format")
    require(manifest.get("owner") == BACKEND_BINDINGS_OWNER, "unexpected backend binding manifest owner")
    bindings = manifest.get("bindings")
    require(isinstance(bindings, list) and 1 <= len(bindings) <= MAX_RESOURCE_ROWS,
            "backend binding manifest requires 1..32 rows")

    ids: set[int] = set()
    configurations: set[tuple[int, int, int, int, str]] = set()
    result = []
    for ordinal, row in enumerate(bindings, 1):
        require(isinstance(row, dict) and set(row) == BACKEND_BINDING_FIELDS,
                f"backend binding row {ordinal} has unexpected fields")
        for field in ("locale_profile", "language_tag", "language", "input_mode", "output_mode",
                      "keyboard_layout", "variant_id", "scheme_id", "scheme_implementation"):
            require(isinstance(row[field], str) and bool(row[field]),
                    f"backend binding row {ordinal} field {field} must be a nonempty string")
        for field in ("numeric_id", "language_value", "input_mode_value", "output_mode_value",
                      "keyboard_layout_value"):
            value = row[field]
            require(isinstance(value, int) and not isinstance(value, bool) and
                    0 <= value <= 0xFFFFFFFF, f"backend binding row {ordinal} field {field} is not uint32")
        require(row["numeric_id"] != 0, "backend binding numeric id must be nonzero")
        require(row["numeric_id"] not in ids, "backend binding numeric id must be unique")
        ids.add(row["numeric_id"])
        require(row["language_tag"] in LANGUAGES,
                f"backend binding language tag is not staged: {row['language_tag']}")
        require(row["scheme_implementation"] in ("implemented", "unverified"),
                "backend binding for an absent scheme is not admissible")
        configuration = (row["language_value"], row["input_mode_value"], row["output_mode_value"],
                         row["keyboard_layout_value"], row["variant_id"])
        require(configuration not in configurations,
                "backend binding duplicates another language/input/output/keyboard/variant tuple")
        configurations.add(configuration)
        result.append(row)
    return result


def profile_rows(enums: dict[str, int], bindings: list[dict[str, Any]],
                 files: dict[str, Payload]) -> list[list[str]]:
    # Every profile field comes from the C++ registry export above. The enum
    # names in that export are checked against the public header here, so the
    # numbers cannot silently drift from the ABI.
    def enum(row: dict[str, Any], field: str, value_field: str) -> str:
        name = row[field]
        value = row[value_field]
        require(name in enums, f"required API enum missing: {name}")
        require(enums[name] == value,
                f"manifest enum {name}={value} differs from ABI value {enums[name]}")
        return str(value)

    def pair(path: str | None) -> list[str]:
        if path is None:
            return ["-", "-"]
        require(path in files, f"profile resource missing from stage plan: {path}")
        return [path, files[path].sha256]

    rows = []
    for bindings_row in sorted(bindings, key=lambda item: item["numeric_id"]):
        lang = bindings_row["language_tag"]
        base = f"data/backend/{lang}/"
        if lang == "zh":
            # The initialism decoder and Rime consume the same verified Terra
            # dictionary. Binding it here makes the decoder hash-pinned instead
            # of discovering an unverified file under the Rime data directory.
            conversion = "data/epocanva_backend_terra.dict.yaml"
        elif lang in ("fr", "ja"):
            conversion = base + ("fr.aff" if lang == "fr" else "lexicon.bin")
        else:
            conversion = None
        rows.append([str(bindings_row["numeric_id"]), enum(bindings_row, "language", "language_value"),
                     enum(bindings_row, "input_mode", "input_mode_value"),
                     enum(bindings_row, "output_mode", "output_mode_value"),
                     enum(bindings_row, "keyboard_layout", "keyboard_layout_value"),
                     bindings_row["variant_id"]] +
                    pair(base + "model.tsv") + pair(conversion) +
                    pair(base + "fr.dic" if lang == "fr" else None) +
                    pair(base + "abbreviations.tsv" if lang in ("en", "fr") else None))
    return rows


def resource_index(rows: list[list[str]]) -> bytes:
    require(1 <= len(rows) <= MAX_RESOURCE_ROWS, "resource index requires 1..32 profile rows")
    ids = set()
    for row in rows:
        require(len(row) == 14 and all(isinstance(value, str) and value and
                not re.search(r"[\t\r\n\0]", value) for value in row), "resource row requires 14 nonempty TSV columns")
        for field in row[:5]:
            require(bool(re.fullmatch(r"[1-9][0-9]*|0", field)) and int(field) <= 0xFFFFFFFF, "invalid numeric API field")
        require(row[0] != "0" and row[0] not in ids, "profile IDs must be nonzero and unique")
        ids.add(row[0])
        require(len(row[5].encode("utf-8")) <= 64, "variant exceeds 64 UTF-8 bytes")
        for ordinal in (6, 8, 10, 12):
            path, digest = row[ordinal:ordinal + 2]
            if path == "-" or digest == "-":
                require(ordinal != 6 and path == digest == "-", "incomplete optional path/hash pair")
            else:
                relative_path(path)
                require(bool(re.fullmatch(r"[0-9a-f]{64}", digest)), "invalid resource SHA-256")
    data = (RESOURCE_MAGIC + "\n" + "\t".join(RESOURCE_COLUMNS) + "\n" +
            "".join("\t".join(row) + "\n" for row in rows)).encode("utf-8")
    require(len(data) <= MAX_RESOURCE_UTF8_BYTES, "resource index exceeds 65536 UTF-8 bytes")
    return data


def attribution_from_splits(inv: Inventory, language: str, report: dict[str, Any]) -> bytes:
    attributes: set[bytes] = set()
    for split in ("train", "dev", "test"):
        path = f"build/backend-training-budget/{language}/model-splits/{split}.jsonl"
        expected = report["splits"][split]
        inv.source(path, expected["sha256"], None, f"reviewed {language}/report.json splits.{split}")
        lines = read_local(inv.repo / path, 128 * 1024 * 1024).splitlines()
        require(len(lines) == expected["records"], f"split record-count mismatch: {path}")
        for line in lines:
            record = json.loads(line)
            require(record["language"] == language and record["license"] == "CC-BY-2.0-FR" and
                    record["source"] == "tatoeba-detailed-snapshot", "split provenance mismatch")
            attributes.add(canonical_attribution(record["attribution"]))
    data = b"".join(value + b"\n" for value in sorted(attributes))
    provenance = report["provenance"]
    require(sha256(data) == provenance["attribution_sha256"] and
            len(attributes) == provenance["distinct_attribution_count"], "training author attribution differs from reviewed report")
    return data


def prepare_plan(repo: Path, *, oov: str = "skip") -> tuple[Inventory, dict[str, Any]]:
    inv = Inventory(repo)
    reports = {path: inv.checked_json(path, digest) for path, digest in REVIEWED_REPORTS.items()}
    for path in REVIEWED_REPORTS:
        inv.evidence(path)
    manifests = {}
    for name in ("backend-ja-corpus.sources.json", "backend-zh-fr.sources.json", "backend-hunspell.sources.json", "license-decision.json"):
        path = "data/manifests/" + name
        manifests[name] = inv.checked_json(path)
        inv.evidence(path)
    ja_source = manifests["backend-ja-corpus.sources.json"]
    zhfr_source = manifests["backend-zh-fr.sources.json"]
    license_record = manifests["license-decision.json"]
    root_license = license_record["license_text"]
    inv.source(root_license["path"], root_license["sha256"], root_license["bytes"], "repository license-decision.json")
    inv.copy(root_license["path"], "data/backend/notices/project/LICENSE")
    tool_path = "tools/backend/stage_resources.py"
    inv.authored(tool_path)
    header_path = "engine/include/epocanva_backend.h"
    enums = api_enums(inv.authored(header_path))

    corpus = ja_source["derived_bounded_corpus"]
    inv.source(corpus["path"], corpus["sha256"], None, "repository backend-ja-corpus.sources.json derived_bounded_corpus")
    inv.evidence(corpus["path"])
    for row in ja_source["tatoeba"]["snapshots"] + [ja_source["tatoeba"]["grouping_export"]]:
        inv.source(row["path"], row["sha256"], row["bytes"], "repository backend-ja-corpus.sources.json Tatoeba input")
    for row in ja_source["tatoeba"]["official_evidence_snapshots"]:
        inv.source(row["path"], row["sha256"], row["bytes"], "repository Tatoeba license/source evidence")
        inv.copy(row["path"], "data/backend/notices/tatoeba/" + Path(row["path"]).name)
    corpus_rows = {row["language"]: row for row in corpus["outputs"]}
    summary = reports["build/backend-training-budget/summary.json"]
    abbreviation_info = {}
    for language in LANGUAGES:
        report = reports[f"build/backend-training-budget/{language}/report.json"]
        row = summary["languages"][language]
        path = f"build/backend-training-budget/{language}/model.tsv"
        require(row["model_path"] == path and row["report_path"] == f"build/backend-training-budget/{language}/report.json", "unexpected reviewed model location")
        inv.source(path, row["model_sha256"], row["model_bytes"], "reviewed training summary model_sha256")
        words = inspect_model(read_local(repo / path, 64 * 1024 * 1024), language, report)
        inv.copy(path, f"data/backend/{language}/model.tsv")
        if report["catalog_sha256"] is not None:
            require(language == "ja", "unexpected model segmentation dependency")
            inv.source("build/backend-japanese-data/segmentation-catalog.jsonl", report["catalog_sha256"],
                       None, "reviewed Japanese report catalog_sha256")
        prepared = corpus_rows[language]
        inv.source(prepared["path"], prepared["sha256"], prepared["bytes"], "repository derived corpus pin")
        require(report["input_files"] == [{"file": Path(prepared["path"]).name,
                "sha256": prepared["sha256"], "utf8_bytes": prepared["bytes"]}], "training input differs from independently pinned corpus")
        inv.generated(f"data/backend/notices/tatoeba/{language}-model-attribution.jsonl",
                      attribution_from_splits(inv, language, report))
        if language in ("en", "fr"):
            source = f"data/backend/abbreviations/{language}.tsv"
            content, info = compile_abbreviations(inv.authored(source), language, words, oov=oov)
            inv.generated(f"data/backend/{language}/abbreviations.tsv", content)
            inv.evidence(source)
            abbreviation_info[language] = info

    mozc = ja_source["mozc"]
    lexicon = reports["build/backend-japanese-data/lexicon.manifest.json"]
    require(lexicon["source_revision"] == mozc["revision"] and lexicon["output"]["path"] == "lexicon.bin", "Mozc revision/output mismatch")
    mozc_rows = {Path(row["path"]).name: row for row in mozc["payload"]}
    for row in mozc["payload"]:
        inv.source(row["path"], row["sha256"], row["bytes"], "repository Mozc payload pin")
    expected_names = {"connection_single_column.txt"} | {f"dictionary{i:02}.txt" for i in range(10)}
    require(len(lexicon["sources"]) == len(expected_names) and {row["path"] for row in lexicon["sources"]} == expected_names, "incomplete Mozc source closure")
    for row in lexicon["sources"]:
        pinned = mozc_rows[row["path"]]
        require(row["sha256"] == pinned["sha256"] and row["bytes"] == pinned["bytes"], "Mozc derived/source hash mismatch")
    inv.source("build/backend-japanese-data/lexicon.bin", lexicon["output"]["sha256"], lexicon["output"]["bytes"], "reviewed lexicon.manifest.json output")
    inv.copy("build/backend-japanese-data/lexicon.bin", "data/backend/ja/lexicon.bin")
    evidence = mozc["dictionary_license_evidence"]
    for prefix, destination in (("upstream_root_license", "LICENSE"), ("dictionary_notice", "dictionary-README.txt")):
        path = evidence[prefix + "_path"]
        inv.source(path, evidence[prefix + "_sha256"], evidence[prefix + "_bytes"], "repository Mozc mixed dictionary license evidence")
        inv.copy(path, "data/backend/notices/mozc/" + destination)

    upstreams = {row["id"]: row for row in zhfr_source["upstreams"]}
    acquisition = zhfr_source["acquisition_root"]
    chinese_inputs = {}
    for name in ("rime-terra-pinyin", "rime-bopomofo", "libreoffice-dictionaries-french"):
        for row in upstreams[name]["files"]:
            path = acquisition + "/" + relative_path(row["path"])
            inv.source(path, row["sha256"], row["bytes"], "repository backend-zh-fr.sources.json " + name)
            if name == "libreoffice-dictionaries-french":
                if path.endswith(("fr.aff", "fr.dic")):
                    inv.copy(path, "data/backend/fr/" + Path(path).name)
                else:
                    inv.copy(path, "data/backend/notices/french/" + Path(path).name)
            elif row["path"].endswith("LICENSE"):
                chinese_inputs[("terra" if name == "rime-terra-pinyin" else "bopomofo") + "/LICENSE"] = row["sha256"]
            elif row["path"].endswith(("terra_pinyin.dict.yaml", "zhuyin.yaml")):
                chinese_inputs[("terra/terra_pinyin.dict.yaml" if name == "rime-terra-pinyin" else "bopomofo/zhuyin.yaml")] = row["sha256"]
    chinese = reports["build/backend-chinese-ranked/manifest.json"]
    require(chinese["input_sha256"] == chinese_inputs and chinese["corpus_sha256"] == corpus_rows["zh"]["sha256"], "Chinese source/corpus provenance mismatch")
    selected = {"epocanva_backend_pinyin.schema.yaml", "epocanva_backend_zhuyin.schema.yaml", "epocanva_backend_terra.dict.yaml", "zhuyin.yaml"}
    found = set()
    for row in chinese["files"]:
        path = "build/backend-chinese-ranked/" + relative_path(row["path"])
        inv.source(path, row["sha256"], row["bytes"], "reviewed ranked Chinese manifest files")
        name = Path(row["path"]).name
        if row["path"].startswith("notices/"):
            inv.copy(path, "data/backend/notices/chinese/" + name)
        elif name in selected:
            inv.copy(path, "data/" + name)
            found.add(name)
            if name.endswith(".schema.yaml"):
                inv.source("data/backend/zh/" + name, row["sha256"], row["bytes"], "reviewed Chinese schema agrees with repository source")
        else:
            require(row["path"] == "shared/default.yaml", "unexpected Chinese dependency")
    french_pair = manifests["backend-hunspell.sources.json"]["french_pair"]
    require(french_pair["revision"] == upstreams["libreoffice-dictionaries-french"]["pinned_commit"],
            "French provider/acquisition revision mismatch")
    for kind in ("aff", "dic", "notice"):
        inv.source(french_pair[kind + "_path"], french_pair[kind + "_sha256"],
                   french_pair.get(kind + "_bytes"), "repository French provider pair pin")
    require(found == selected, "Chinese schema/dictionary/Zhuyin closure missing")
    # Weights used the entire prepared Mandarin corpus, including records later
    # excluded from model training. Preserve those additional author references.
    weighting_attrs = {canonical_attribution(json.loads(line)["attribution"])
                       for line in read_local(repo / corpus_rows["zh"]["path"]).splitlines()}
    inv.generated("data/backend/notices/tatoeba/zh-weighting-attribution.jsonl",
                  b"".join(value + b"\n" for value in sorted(weighting_attrs)))

    hunspell = manifests["backend-hunspell.sources.json"]["hunspell"]
    dependency = hunspell["preparation_root"]
    archive = dependency + "/downloads/" + hunspell["archive_root"] + ".tar.gz"
    inv.source(archive, hunspell["archive_sha256"], hunspell["archive_bytes"], "repository Hunspell archive pin")
    for row in hunspell["library_files"] + hunspell["license_files"]:
        path = dependency + "/" + hunspell["archive_root"] + "/" + relative_path(row["path"])
        inv.source(path, row["sha256"], None, "repository Hunspell library/notice pin")
        if row in hunspell["license_files"]:
            inv.copy(path, "data/backend/notices/hunspell/" + Path(row["path"]).name)

    rows = profile_rows(enums, binding_rows(inv), inv.files)
    inv.generated("data/backend/resources.tsv", resource_index(rows))
    notice = ("EpoCanva backend resources: internal local staging; no public distribution approval.\n"
              "Project-owned tooling, schemas and shorthand definitions: AGPL-3.0-only.\n"
              "Tatoeba corpus-derived models/Chinese weights: CC-BY-2.0-FR; author and sentence URL JSONL files accompany this notice.\n"
              "Chinese Terra dictionary: declared CC-BY-SA-3.0, CC-CEDICT/MDBG attribution; recipe/Zhuyin licenses retained separately.\n"
              "Mozc OSS dictionary: mixed notices; read both mozc/LICENSE and mozc/dictionary-README.txt.\n"
              "French Classique data: MPL-2.0. Hunspell: selected LGPL-2.1-or-later option, original notices preserved.\n\n" +
              "\n".join("- " + item for item in BLOCKERS) + "\n")
    inv.generated("data/backend/notices/NOTICE.txt", notice.encode("utf-8"))
    manifest = {
        "format": 1, "owner": OWNER, "public_distribution_authorized": False,
        "resource_index": "data/backend/resources.tsv", "profile_count": len(rows),
        "source_path_base": "repository root", "staged_path_base": "product root",
        "inputs": [inv.inputs[key] for key in sorted(inv.inputs)],
        "files": [{"path": key, "sha256": value.sha256, "bytes": value.size_bytes}
                  for key, value in sorted(inv.files.items())],
        "abbreviations": abbreviation_info, "distribution_blockers": BLOCKERS,
        "existing_file_policy": "identical bytes accepted; every changed existing target refused, including previously owned files",
    }
    inv.generated(MANIFEST_PATH, json_bytes(manifest))
    return inv, manifest


def check_target(root: Path, relative: str, payload: Payload) -> bool:
    path = root / relative_path(relative)
    check_local_path(path)
    if not path.exists():
        return False
    digest, size_bytes = file_digest(path)
    require(digest == payload.sha256 and size_bytes == payload.size_bytes,
            f"changed existing target refused: {relative}; choose a new product root or reconcile it explicitly")
    return True


def publish(repo: Path, root: Path, inv: Inventory) -> None:
    # Preflight ALL conflicts before creating/writing any output. Never copy the
    # Chinese shared default.yaml over an existing English product default.yaml.
    for row in inv.inputs.values():
        require(not (repo / row["path"]).is_relative_to(root), "product root overlaps an input resource tree")
    existing = {relative: check_target(root, relative, value) for relative, value in inv.files.items()}
    if all(existing.values()):
        return
    check_local_path(root)
    root.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=".backend-staging-", dir=root))
    published: list[Path] = []
    try:
        for relative, payload in inv.files.items():
            if existing[relative]:
                continue
            path = temporary / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if payload.source is None:
                require(payload.content is not None, "missing generated payload")
                path.write_bytes(payload.content)
            else:
                check_local_path(payload.source)
                shutil.copyfile(payload.source, path)
            require(file_digest(path) == (payload.sha256, payload.size_bytes), f"input changed while copying: {relative}")
        # Manifest is insertion-last and publishes last. Atomic, non-replacing
        # hard links from our private copies avoid os.replace overwriting a file
        # created after preflight. They never link a resource to its source file.
        for relative, payload in inv.files.items():
            if check_target(root, relative, payload):
                continue
            destination = root / relative
            check_local_path(destination.parent)
            destination.parent.mkdir(parents=True, exist_ok=True)
            try:
                os.link(temporary / relative, destination)
                published.append(destination)
            except FileExistsError:
                check_target(root, relative, payload)
    except BaseException:
        for path in reversed(published):
            path.unlink()
        raise
    finally:
        shutil.rmtree(temporary)


def stage_resources(product_root: str | Path, *, repo_root: str | Path | None = None,
                    oov: str = "skip", check_only: bool = False) -> dict[str, Any]:
    """Callable build entrypoint; return the same deterministic staging manifest.

    repo_root defaults to this checkout, independent of the caller's cwd.
    check_only verifies sources and target conflicts without creating any files.
    All mismatches raise StagingError (OSError for filesystem failures).
    """
    default_repo = Path(__file__).absolute().parents[2]
    repo = local_absolute(repo_root, default_repo) if repo_root is not None else default_repo
    check_local_path(repo)
    root = validate_product_root(repo, product_root)
    inv, manifest = prepare_plan(repo, oov=oov)
    if check_only:
        for relative, payload in inv.files.items():
            check_target(root, relative, payload)
    else:
        publish(repo, root, inv)
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--product-root", required=True, help="exact descendant build/<preset>/... (no inferred product directory)")
    parser.add_argument("--repo-root", help="local checkout root; defaults to the script's checkout")
    parser.add_argument("--oov", choices=("skip", "error"), default="skip", help="report and skip OOV expansions, or refuse all staging")
    parser.add_argument("--check-only", action="store_true", help="verify all inputs and target conflicts; write nothing")
    args = parser.parse_args(argv)
    try:
        manifest = stage_resources(args.product_root, repo_root=args.repo_root, oov=args.oov, check_only=args.check_only)
    except (StagingError, OSError, KeyError, TypeError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"backend resource staging failed: {exc}", file=sys.stderr)
        return 1
    for language, info in manifest["abbreviations"].items():
        for row in info["skipped_oov"]:
            print(f"skipped OOV abbreviation {language}/{row['key']}: {' '.join(row['missing_model_words'])}", file=sys.stderr)
    print(f"{'Verified' if args.check_only else 'Staged'} {manifest['profile_count']} backend profiles; "
          f"{len(manifest['inputs'])} local provenance inputs; public distribution remains unapproved.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
