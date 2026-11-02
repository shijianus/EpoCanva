# Offline backend n-gram training and prediction quality

## Scope

`tools/backend/train_ngram.py` trains observed unigram, bigram and trigram counts
from approved **local corpus files**. `tools/backend/evaluate_predictions.py`
compares real native predictions with the same native core's unigram-only
baseline. Neither tool downloads data, uses network APIs, reads application
input, nor learns from raw user typing. Corpus acquisition and licensing review
are separate steps. A source hash is an integrity identity, not proof of license
permission or source authenticity.

This is a next-token corpus experiment, not proof of every English input-method
capability or of production cross-application behavior. It does not implement
morphology, spelling correction, run-together segmentation or personal learning.
The native CLI calls the same prediction `Model` implementation as the backend;
this particular quality experiment does not exercise the entire C ABI/session or
platform input-method path.

## Prepared corpus contract

Input is strict UTF-8 JSONL, one object per line, with these fields:

- `id`: nonempty string, unique with `language` across all supplied files.
- `language`: exactly `en`, `zh`, `fr`, `ja` or `ko`.
- `text`: one corpus sentence/source unit, not a runtime composition buffer.
- `source`, `license`: nonempty provenance strings.
- `attribution`: optional string or object with string-valued author, source URL
  and license fields. Structured attribution is retained losslessly in split
  JSONL, not flattened into a false license claim.
- `document_id` (preferred), `group_id` or `group`: stable grouping unit supplied
  by the prepared-source pipeline. If absent, author/username is accepted; the
  conservative final fallback is the complete `source` string.

Use `document_id` to group related source/translation-family records. Never give
each sentence a fresh group merely to increase test size. The current approved
Tatoeba preparation resolves available base families and preserves sentence
attribution; it does **not** establish that arbitrary paraphrases, unrecorded
translation links or every author style are disjoint.

The trainer fails on invalid records by default. `--skip-invalid-records` permits
whole-record exclusion for non-whitespace Unicode control/format scalars. It
records exclusion counts and still hashes the full source file. It never strips
format characters, repairs sentences, or silently falls back to a different
normalization. Other format/schema failures remain errors. Empty corpus input
and a training split without lexical records fail.

The prepared source files and their acquisition manifest must be retained for
attribution and reproduction. JSONL outputs are build artifacts, not runtime
user logs.

## Tokenization and normalization

The exported `tokenize(text, language, catalog=None)` method is reused when
producing gold cases and backend requests. It returns normalized token/kind
pairs, not UTF-16 offsets.

- **English/French:** Unicode letter/combining-mark runs with internal ASCII or
  typographic apostrophes/hyphens; other scalars form individual punctuation
  outcomes. Accents are preserved. This rule is lexical tokenization, not a
  dictionary-trained tokenizer.
- **Korean:** spacing-delimited Unicode word runs with punctuation separated;
  this is not a morphological analyzer.
- **Chinese/Japanese:** optional dictionary-assisted greedy longest match over
  supplied legal surfaces. Unmatched spans use scalar-plus-combining-mark units.
  Without a catalog this is an explicitly **partial character baseline**, not
  full word segmentation. The Japanese experiment uses actual Mozc dictionary
  surfaces; longest match is still an approximation, not a trained/Viterbi
  morphological tokenizer. The Chinese experiment is a character baseline.

Catalog input is local JSONL `{ "language": "ja", "word": "surface" }` (or
`zh`). Catalog counts and SHA-256 are recorded. Dictionary weights are never
misrepresented as corpus frequencies.

Normalization is exactly `ascii-lower-utf8-v1`: only ASCII `A`–`Z` change to
`a`–`z`. There is no Python `casefold`, general Unicode lowercasing, NFC or NFKC.
A future prepared-source normalization pipeline must be explicitly versioned;
it cannot silently change this model contract. Lexemes are capped at 256 UTF-8
code units (bytes). Scalar-plus-mark fallback is not a claim of full Unicode
grapheme-cluster segmentation.

## Model contract and determinism

The authoritative native contract is `engine/prediction/model.h`; the loader and
marginal checks are in `engine/prediction/model.cc`. The emitted LF-only TSV is:

```text
EPOCANVA_PREDICTION_TSV\t1
id_scheme\tfnv1a64-language-normalized-v1
normalization\tascii-lower-utf8-v1
source\tproduction-corpus|test-fixture
source_sha256\t<64 lowercase hex characters>
payload_sha256\t<64 lowercase hex characters>
lexemes\t<N>
unigram_total\t<N>
bigram_total\t<N>
trigram_total\t<N>
data
L\t<id>\t<language>\tword|punct\t<normalized text>
U\t<id>\t<count>
B\t<previous id>\t<id>\t<count>
T\t<previous2 id>\t<previous1 id>\t<id>\t<count>
```

The `\t` above denotes a literal tab. Payload tables are in L/U/B/T order and
numeric ID/tuple order. `payload_sha256` covers exact payload UTF-8 bytes after
`data\n`. Stable IDs are FNV-1a-64 over
`epocanva.prediction.lexeme/v1/` + language + `/` + normalized UTF-8 text, offset
14695981039346656037, prime 1099511628211; zero maps to one. Distinct lexeme
collisions fail, rather than being accepted as duplicate words.

Each sentence contributes its token sequence followed by EOS target ID 0. There
is no BOS. EOS has no L row and never occurs in history. Positive uint64 counts,
checked totals, aggregate bigram incoming/outgoing bounds and aggregate trigram
prefix/suffix bounds are verified before serialization.

Deduplication precedes splitting and uses language plus normalized token
sequence. One deterministic representative remains. A SHA-256 hash of the
supplied grouping unit and fixed seed assigns train/dev/test (default 80/10/10).
All corpus counting uses train only. Split JSONL hashes, counts, OOV occurrence
counts, distinct OOV counts, source/license pairs and attribution identity are
reported. The same prepared source/group seed must be retained across variants.

Bounds default to 512 MiB input, one million records, 20 million tokens, 500,000
unique lexical records, five million n-gram rows and 64 MiB per emitted model.
Exceeding input/count bounds fails instead of truncating input. File-budget
pruning is deterministic and denominator-aware. A product package's aggregate
budget is a separate gate; per-file compliance does not imply aggregate
compliance.

`--min-ngram-count 2` is the frozen aggregate-budget experiment. It retains U/L
and removes B/T rows observed fewer than twice; a T row also requires both B
rows to remain. This choice was made for physical size, not selected by held-out
quality. All five budget models together occupy **31,377,210 bytes**, below
64 MiB. The original full models occupy **127,597,840 bytes**, exceeding the
aggregate gate. Original models and metrics remain available separately.

## Evaluation protocol

Trainer gold JSONL contains `id`, `language`, `context` (at most two normalized
tokens), `target_text`, `prefix`, `split: "test"`, and `group_id`. The matching
requests file contains five tab-separated fields per line:

```text
id<TAB>language<TAB>previous2_text<TAB>previous1_text<TAB>prefix
```

A single context token is in `previous1_text`; empty fields are retained. Gold
text is not passed to the native process. The native invocation is:

```text
epocanva_backend_cli --model model.tsv --evaluate requests.tsv --output results.jsonl
```

The baseline adds `--baseline-unigram`, using the same core, language filter and
ranking/tie rules with an empty context. Each mode runs **one process**, loading
the model once for all requests, using `subprocess` with `shell=False`. There is
no fabricated output and no Python substitute for native prediction. Captured
actual model/baseline JSONL can be provided instead. The evaluator checks native
exit status, IDs, languages, duplicate results, `status`, `complete`, latency,
and the native executable hash before/after each invoked mode.

Native result JSONL contains `id`, `language`, `ranked` texts, `latency_ms`,
`status`, `complete`, and optional counters. The current CLI uses exactly
`max_words=1`, `max_suggestions=5`. Therefore reported MRR is **MRR@5** (a target
outside the returned five is a miss), not full-vocabulary reciprocal rank.
Missing rows and OOV targets remain misses. Coverage means result-row coverage;
it is not an OOV or successful-prediction rate.

The reports include Hit@1/3/5, MRR@5, row coverage, and deterministic paired
percentile bootstrap confidence intervals. The resampling unit is the supplied
document/source group, not correlated token rows. The verified experiment used
1,000 replicates and seed 20261102. A result with fewer than 1,001 gold target
examples per language, missing rows, non-OK status, incomplete results or
language mismatch is not quality-eligible. Mechanical eligibility does not
remove the semantic character-tokenization caveat for Chinese or validate a
full input-method product.

## Reproduction

Run from the repository root; all outputs below remain in `build/<preset>/`.
No package installation is necessary. Python's standard library is sufficient.
Use `-B`/`PYTHONDONTWRITEBYTECODE=1` for tests; do not use `py_compile` to test the
no-bytecode policy because `py_compile` intentionally writes `.pyc` files.

```sh
PYTHONDONTWRITEBYTECODE=1 python -B -m unittest discover \
  -s tests/backend_training -p 'test_*.py' -v

PYTHONDONTWRITEBYTECODE=1 python -B tools/backend/train_ngram.py \
  --input build/backend-corpus-prepared/en.jsonl \
  --source-kind production-corpus --skip-invalid-records --min-ngram-count 2 \
  --output build/backend-training-budget/en/model.tsv \
  --report build/backend-training-budget/en/report.json \
  --evaluation-requests build/backend-training-budget/en/requests.tsv \
  --eval-cases-out build/backend-training-budget/en/gold.jsonl

PYTHONDONTWRITEBYTECODE=1 python -B tools/backend/evaluate_predictions.py \
  --cases build/backend-training-budget/en/gold.jsonl \
  --requests build/backend-training-budget/en/requests.tsv \
  --model build/backend-training-budget/en/model.tsv \
  --backend-executable build/backend-service/Release/epocanva_backend_cli.exe \
  --output build/backend-training-budget/en/evaluation.json \
  --bootstrap-replicates 1000
```

Repeat for `fr`, `ko` and `zh`; Chinese intentionally has no segmentation catalog.
For Japanese add
`--catalog build/backend-japanese-data/segmentation-catalog.jsonl` to training.
Use a fresh output directory for an independent repeat because the native CLI
refuses to overwrite a results file. The original full model experiment used
the same commands without `--min-ngram-count 2` under `build/backend-training/`.
The budget variants' train/dev/test, gold and request SHA-256 identities were
checked byte-for-byte equal to the originals before evaluation.

## Verified corpus experiment (2026-11-02)

The prepared corpus is a bounded attributed Tatoeba snapshot, not hand-authored
production samples. The acquisition/preparation manifest is
`build/backend-corpus-prepared/manifest.json`. All five original training and
native evaluation commands, and all five budget training and evaluation commands,
completed with observed exit status 0. Earlier fail-fast en/fr/ko attempts exited
1 because 6/4/1 whole records contained Unicode format scalars; the explicit
exclusion mode recorded those exclusions before successful reruns. Nothing in
this document treats those first attempts as passing.

The final independent test suite has **23 passing tests**. It checks fixed FNV
and split vectors, Unicode normalization, EOS counts, canonical TSV/hash ordering,
resource pruning, fixed min-count pruning, group leakage/dedup, structured
attribution, whole-record exclusion, known metric arithmetic and quality gates.
These are fixtures and are not production quality evidence. `pytest` was not
installed (`python -m pytest --version` exited 1); stdlib `unittest` was executed
instead. No dependency was installed.

Budget-model results (native K=5, all row coverage 1.0, all rows OK and complete):

| Language / tokenizer scope | Test target examples | Groups | Model Hit@1 | Hit@3 | Hit@5 | Model MRR@5 | Unigram MRR@5 | Paired delta MRR 95% CI |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| English words | 91,503 | 10,213 | 0.224233 | 0.347573 | 0.433297 | 0.297143 | 0.137984 | [0.156257, 0.162102] |
| French words | 84,884 | 9,436 | 0.199260 | 0.323665 | 0.380696 | 0.265249 | 0.126665 | [0.135659, 0.141318] |
| Korean spacing words | 5,958 | 910 | 0.180933 | 0.243202 | 0.270393 | 0.214225 | 0.167360 | [0.039896, 0.053833] |
| Chinese **partial character** baseline | 59,487 | 4,501 | 0.236270 | 0.379158 | 0.436818 | 0.310306 | 0.099964 | [0.205271, 0.214745] |
| Japanese approximate catalog longest match | 80,183 | 7,067 | 0.276580 | 0.416023 | 0.476610 | 0.351743 | 0.129252 | [0.219342, 0.225496] |

All runs use the same fixed split, including the size-driven second experiment;
this is transparent reuse, not a claim of a second untouched holdout. The
budget policy trades some quality for size. The full models' English/French/
Korean/Chinese MRR@5 were 0.301767/0.271130/0.230637/0.319728; keep the original
per-language JSON reports rather than replacing them with the budget results.

A compact artifact index is `build/backend-training-budget/summary.json`.
Per-language `report.json` contains source/split/tokenizer/OOV data;
`evaluation.json` contains exact hashes, native exit statuses, metric confidence
intervals and latency observations. The native executable used for the verified
budget experiment was SHA-256
`0acd0166aae03a9484af0157dedd2a7dcdc5b82b4214a919f3abb414e2721e8e`.

## Limitations and next gates

- This demonstrates corpus next-token quality, not every English capability and
  not cross-app, TSF or Android behavior. The separate native backend/API tests
  own those integration checks.
- Chinese needs legally sourced word segmentation before word-quality claims;
  Japanese needs a stronger segmentation model if longest-match quality is not
  adequate. Do not relabel character scores as word prediction quality.
- Corpus split family grouping does not prove arbitrary semantic/paraphrase
  disjointness. Attribution and redistribution compliance must accompany shipped
  models; hashes alone grant no rights.
- Runtime remains offline architecturally on an ordinary Windows TSF DLL: no
  network calls in our runtime/dependencies, not an OS sandbox guarantee. Android
  can enforce the distinct network restriction by omitting INTERNET permission.
- The model size gate is aggregate serialized TSV size, not process RSS or a
  complete package-size cap. Native memory/latency and full product admission
  remain separate measured gates.
