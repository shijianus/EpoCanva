# E6 candidate-menu evaluation baseline — design and minimal slice

**Status**: the minimal baseline slice is implemented and was run once against
the real staged resources (see `docs/reports/phase-e-quality-gates.md` for the
recorded numbers). Committed in this slice: the Tier A synthetic corpus
(`data/eval/candidate-v1/`, 15 cases whose expectations are pinned by existing
assertions in `tests/backend/profiles.cc`), the native harness
(`tests/candidate_evaluation/candidate_eval_cli.cc`), the evaluator
(`tools/backend/evaluate_candidates.py`) and its unit tests
(`tests/candidate_evaluation/test_evaluate_candidates.py`, registered as the
`candidate_evaluation.contract` CTest). NOT implemented: the Tier B external
corpus, the 1,001/500-case minimums, the warm-up query phase, and every part of
the §9 E4 entry gate beyond those inputs. E4 (cross-language fusion) and E5
(detection latching) must not start until the full gate exists and passes.

Deviations of the implemented slice from the design text below, recorded so
this document stays the single description of what exists:

- The harness emits one JSON object per case with a `repeats` array (one entry
  per in-process query) rather than one flat object per query; the run's
  `run_meta` line carries load time, peak working set and the repeat count.
  Determinism compares the full ordered `(kind, text_utf8, lexeme_ids, input
  begin/end)` list across repeats and across `--runs` fresh processes.
- The committed `manifest.json` cannot contain the commit under test (it would
  immediately be stale), so `repo_commit` is supplied to the evaluator per run
  (`--repo-commit`) and recorded in the report identity, not in the manifest.
  The manifest keeps the corpus digests, the case counts and the
  `backend_manifest_sha256` freshness pin, which the unit tests enforce.
- The harness accepts `--rime-dll`, `--rime-dll-sha256`, `--rime-shared` and
  `--work` because the staged backend resources carry no Rime runtime; zh
  replay requires the reviewed ranked shared data
  (`build/backend-chinese-ranked/shared`), which the harness copies into an
  isolated work tree so the staged inputs stay immutable.
- There is no 20-query warm-up phase; `latency_ms.warm` aggregates every query
  except the single cold first query of each language, and the report records
  `warmup_queries: 0` so the numbers are not mistaken for the designed
  post-warm-up regime.
- `wrong_language_rate` is structural for this backend: each case runs in its
  declared profile's session and the v1 ABI carries no per-candidate language,
  so the value is 0.0 by construction, not a measured claim.
- Measurement discovered the raw-first contract: every COMPLETION menu pins the
  raw candidate at ordinal 0 (zh/ja explicitly; en/fr/ko via the lexical
  service's kOriginal row). `top1`/`top3`/`mrr` are kept exactly as designed
  over the full menu (their denominator includes the raw row), and the
  evaluator additionally records `suggestion_top1`/`suggestion_top3`/
  `suggestion_mrr` over the menu with raw-kind rows excluded, which track
  suggestion ordering directly. For ko two-set, the "raw" row carries the
  composed Hangul rather than the typed romanization keys, so `raw_present_rate`
  is a real per-language property, not a constant.
- The evaluator refuses a report path inside the repository that is not under
  `build/`; the runs directory (which carries candidate surfaces) lives beside
  the report.

## 1. Purpose and non-goals

E6 measures the candidate menu the product actually publishes, per language and
scheme, so a later fusion or latching change can be compared against a recorded
baseline instead of an opinion.

Non-goals:

- no neural model, no model download, no inference requirement;
- no real user input, telemetry, clipboard, crash report or host log;
- no product learning-write change;
- no new public C ABI and no persisted-format change;
- no E4/E5 implementation. This document only defines the measurement.

## 2. Corpus format (JSONL)

One strict UTF-8 JSON object per line, LF endings, no BOM, no blank lines
between records. Comments are not allowed. Each record is:

```json
{
  "id": "en-prefix-0001",
  "language": "en",
  "scheme": "latin",
  "profile_id": 1,
  "raw_input": "goo",
  "preedit": "goo",
  "context": [],
  "expected": ["good", "goodness"],
  "expected_languages": ["en"],
  "group_id": "synthetic-en-prefix-family-01",
  "split": "test",
  "source": "epocanva-synthetic-v1",
  "license": "AGPL-3.0-only",
  "attribution": {"author": "EpoCanva", "license": "AGPL-3.0-only"},
  "tags": ["completion", "raw-retention"]
}
```

Required fields:

| Field | Type | Rule |
| --- | --- | --- |
| `id` | string | unique across all files; ASCII `[A-Za-z0-9._-]+` |
| `language` | string | exactly `en`, `zh`, `fr`, `ja` or `ko` |
| `scheme` | string | `latin`, `pinyin`, `zhuyin`, `romaji`, `kana`, `two_set` or `katakana` |
| `profile_id` | integer | the binding id from `data/locale/backend-bindings.json`; the case must be admissible for that binding |
| `raw_input` | string | the exact raw bytes to set; nonempty, valid UTF-8, no control scalar |
| `context` | array of strings | 0..2 prior tokens, synthetic/licensed; empty for most cases |
| `expected` | array of strings | 1..8 acceptable committed surfaces; order is not a preference |
| `expected_languages` | array of strings | languages that may legally appear; usually one |
| `group_id` | string | stable source/document family for clustered bootstrap |
| `split` | string | must be `test`; train/dev records are rejected by the evaluator |
| `source` | string | corpus identity, not a URL |
| `license` | string | SPDX identifier or an explicit `NOASSERTION` with a note |
| `attribution` | object | optional author/source/license fields, preserved |
| `tags` | array | optional; `raw-retention`, `mixed`, `abbreviation`, ... |

Optional `preedit` defaults to `raw_input`. A case with `scheme: "zhuyin"` is
accepted but reported in a separate **unverified** section and is excluded from
the E4 quality gate; the locale registry still resolves zhuyin as
`kSchemeNotVerified`.

The evaluator rejects:

- duplicate `id`, unknown language/scheme, a `profile_id` that does not own the
  requested scheme, empty `raw_input` or `expected`;
- control characters, invalid UTF-8, a non-test split, or a record larger than
  the backend's 4,096 UTF-8 byte input bound;
- an `expected_languages` entry outside the five-language set, or an empty
  `expected` list. No Unicode script table is claimed; language membership is
  checked by identity, not by guessing a script from the surface text.

## 3. Provenance and licensing

Two corpus tiers, never mixed in one gate:

1. **Tier A — committed synthetic corpus.** Project-authored, deterministic
   templates and hand-written cases; licence `AGPL-3.0-only`; committed under
   `data/eval/candidate-v1/cases/`. This tier proves determinism, latency,
   raw-retention plumbing and evaluator correctness. It is not a quality claim
   about natural language.
2. **Tier B — licensed external corpus.** Public text with a reviewed licence
   (for example Tatoeba-derived material already used by the prediction
   baselines). The corpus is **not** committed when the licence does not permit
   redistribution; it lives under `build/eval-inputs/<corpus-id>/` and is
   identified by SHA-256. Results are marked `external-corpus` and cannot be a
   release gate until the licence review is recorded.

No tier may contain real user input, a real composition buffer, a keystroke log,
a clipboard capture, a focused-field name or an application identifier. A
reviewer must be able to reproduce every case from the recorded source and
licence.

## 4. Metrics

All metrics are computed per language/scheme and overall. Samples are cases, not
candidates, unless stated.

| Metric | Definition | Unit |
| --- | --- | --- |
| `top1` | fraction of cases whose rank-1 surface is in `expected` | ratio |
| `top3` | fraction whose first three surfaces contain an expected surface | ratio |
| `mrr` | mean of `1 / rank` of the first expected surface; 0 when absent | ratio |
| `raw_present_rate` | fraction of cases whose menu contains a candidate whose text equals `raw_input` exactly | ratio |
| `raw_first_rate` | fraction where that exact raw candidate is at ordinal 0 | ratio |
| `empty_menu_rate` | fraction of valid nonempty inputs that produce zero candidates | ratio |
| `wrong_language_rate` | fraction of returned candidates whose contract language is not in `expected_languages` | ratio |
| `deterministic_cases` | fraction where two in-process runs and one fresh-process run produce identical ordered `(stable_id, text, source, commit)` lists | ratio |
| `latency_cold_ms` | first query after resource load, fresh process | ms |
| `latency_warm_ms` | P50/P95/P99 over all queries after 20 warm-up queries | ms |
| `peak_working_set_bytes` | `PeakWorkingSetSize` for a fresh-process language run | bytes |
| `resource_load_ms` | open backend + load model/conversion/auxiliary, no query | ms |

Notes:

- **Raw retention** is reported for completion queries. Languages whose backend
  does not publish a separate raw candidate (English/French/Korean lexical
  menus) can still score `raw_present_rate` when the exact input appears as a
  lexical candidate; `raw_first_rate` is reported only where a raw candidate
  exists. A zero denominator is reported as `null`, never as 1.0 or 0.0.
- **Wrong language** is identity-based: it uses the candidate contract's
  `language_tag`, not a Unicode script guess. For current single-language
  sessions the expected value is 0; for E4 mixed cases the case declares
  `expected_languages`.
- **Determinism** compares the full ordered list, not just the first item. A
  tie-order change is a failure even when Top-1 is unchanged.
- **Cold/warm** uses `QueryPerformanceCounter` (monotonic) and excludes file
  I/O from the timed region except for the explicit resource-load measurement.
- **Peak memory** is process-level and includes the harness; the report also
  records the baseline working set before opening resources, so the delta is
  visible. No claim is made about a single allocation.
- **Resource loading cost** is measured in a fresh process with a `--load-only`
  mode; it must not be inferred from the first query.

Confidence intervals: 95% bootstrap over `group_id` (10,000 replicates, fixed
seed), matching the existing prediction evaluator's grouping discipline. A
per-case metric without a group is rejected.

## 5. File layout

```text
data/eval/candidate-v1/
  README.md                       provenance, licence, how to reproduce
  manifest.json                   corpus + environment pins (below)
  cases/en.jsonl
  cases/zh.jsonl
  cases/fr.jsonl
  cases/ja.jsonl
  cases/ko.jsonl
tools/backend/evaluate_candidates.py
tests/candidate_evaluation/
  CMakeLists.txt                  CTest wiring (gate + negative)
  candidate_eval_cli.cc           native menu harness (NOT_IMPLEMENTED)
  test_evaluate_candidates.py     metric/validation unit tests
docs/testing/candidate-evaluation-baseline.md
build/<preset>/candidate-eval/<run-id>/
  results.jsonl                   native per-case output
  report.json                     computed report
  environment.json                sanitized toolchain summary
```

`manifest.json` records, and the report repeats:

```json
{
  "format": "epocanva-candidate-eval-manifest-v1",
  "corpus_id": "candidate-v1",
  "corpus_version": 1,
  "license": "AGPL-3.0-only",
  "repo_commit": "<40-hex commit SHA under test>",
  "backend_manifest_sha256": "<64-hex SHA-256 of data/locale/backend-bindings.json>",
  "resource_manifest_sha256": "<64-hex SHA-256 of the staged resources.tsv>",
  "model_sha256": {"en": "<64-hex>", "zh": "...", "fr": "...", "ja": "...", "ko": "..."},
  "corpus_sha256": {"en.jsonl": "<64-hex>", "zh.jsonl": "..."},
  "case_count": {"en": 0, "zh": 0, "fr": 0, "ja": 0, "ko": 0},
  "harness_version": 1
}
```

No wall-clock timestamp is written into the manifest; `repo_commit` and the
hashes are the identity. The report may record the run's UTC start time for
human context, but it is not part of the comparison key.

## 6. Native harness contract

The native side is intentionally small and text-free in its logging:

```text
candidate_eval_cli --resources <staged-product-root> --cases <jsonl>
                   --language <en|zh|fr|ja|ko> --profile-id <id>
                   --output <results.jsonl> --load-only
```

For each case it:

1. opens a backend session with the staged resources and the case's binding;
2. calls `SetInput(raw_input, preedit)`;
3. queries `EPOCANVA_BACKEND_COMPLETION` with a bounded candidate count;
4. emits one JSON object: `case_id`, `status`, `candidates` (rank, surface,
   lexeme_ids, source, `input_begin/end_utf8_byte`), and `latency_ns`;
5. never logs raw input or candidate text to stderr; the results file is the
   only place surfaces appear, and it stays under `build/`.

`--load-only` opens the session, loads resources, reports wall time and peak
working set, then exits without reading cases.

The harness is not implemented in this round. The command is the target
interface, not a claim that it runs.

## 7. Evaluator and report

`tools/backend/evaluate_candidates.py` is standard-library-only and performs:

1. manifest and corpus validation (UTF-8, fields, duplicates, split, bounds);
2. one native run per language, twice in-process and once in a fresh process
   for the determinism check (or one run with `--repeat 2` plus a `--fresh`
   run; the exact flag is fixed with the harness);
3. metric computation, grouped bootstrap CIs and per-language aggregation;
4. a deterministic JSON report (`ensure_ascii=False`, `sort_keys=True`,
   two-space indent, trailing LF);
5. refusal to write a report when any input is invalid; exit 1 on validation or
   metric failure, 2 on bad arguments.

Report shape:

```json
{
  "format": "epocanva-candidate-eval-v1",
  "corpus": {"id": "candidate-v1", "version": 1, "license": "AGPL-3.0-only"},
  "identity": {
    "repo_commit": "...",
    "backend_manifest_sha256": "...",
    "resource_manifest_sha256": "...",
    "model_sha256": {"en": "..."}
  },
  "per_language": {
    "en": {
      "cases": 1001,
      "top1": 0.0,
      "top3": 0.0,
      "mrr": 0.0,
      "top1_ci95": [0.0, 0.0],
      "raw_present_rate": 0.0,
      "raw_first_rate": 0.0,
      "empty_menu_rate": 0.0,
      "wrong_language_rate": 0.0,
      "deterministic_cases": 1.0,
      "latency_ms": {"cold": 0.0, "warm": {"p50": 0.0, "p95": 0.0, "p99": 0.0}},
      "peak_working_set_bytes": 0,
      "resource_load_ms": 0.0
    }
  },
  "unverified": {"zhuyin": {"cases": 0, "reason": "scheme-unverified"}},
  "failures": [{"case_id": "en-prefix-0001", "reason": "empty-menu"}]
}
```

Failure entries carry case IDs and reasons, not raw input or candidate text.
The results JSONL is the only file with surfaces and is never uploaded as a CI
artifact.

## 8. Test commands (target)

```sh
cmake --build build/backend-service --config Release --target epocanva_candidate_eval_cli

python -B tools/backend/evaluate_candidates.py \
  --cases data/eval/candidate-v1/cases \
  --manifest data/eval/candidate-v1/manifest.json \
  --resources build/product-resources/product \
  --harness build/backend-service/Release/epocanva_candidate_eval_cli.exe \
  --output build/candidate-eval/report.json \
  --repeat 2 --fresh-run \
  --min-cases-en 1001 --min-cases-zh 1001 --min-cases-fr 1001 \
  --min-cases-ja 1001 --min-cases-ko 500

python -B -m unittest tests.candidate_evaluation.test_evaluate_candidates
```

The Python unit tests must include a negative case for every validator rule and
a pinned metric vector, so a broken metric cannot pass silently.

## 9. Quality gate to enter E4

E4 may start only when **all** of these are recorded in a report:

1. **Corpus**: Tier A green for all five languages; Tier B with at least 1,001
   cases for en/zh/fr/ja and 500 for ko, licence reviewed and SHA-256 recorded.
2. **Correctness**: `deterministic_cases == 1.0`; `wrong_language_rate == 0.0`
   for single-language sessions; `empty_menu_rate == 0.0` for valid nonempty
   inputs; `raw_present_rate == 1.0` where the backend publishes a raw
   candidate, and `raw_first_rate == 1.0` where ordinal-0 raw is the contract.
3. **Quality baseline**: Top-1, Top-3, MRR and their 95% CIs are recorded for
   every language. A later E4 change must not lower any language's Top-1 or MRR
   beyond the CI-overlap tolerance (the report records the exact tolerance; no
   uncalibrated "improvement" is accepted without it).
4. **Latency**: cold and warm P50/P95/P99 are recorded per language. E4 must
   keep warm P95 within +20% and +5 ms of the baseline, whichever is larger,
   unless a written product decision accepts the regression.
5. **Memory**: peak working set and resource-load time are recorded. E4 must not
   add a second model load or exceed +20% / +32 MiB peak working set, whichever
   is larger, without a written decision.
6. **Privacy**: the report and results contain no real user input; corpus
   provenance and licences are recorded; no network I/O occurred.
7. **No ABI/schema change**: the public C ABI and learning schema are unchanged;
   E4 identity/dedup uses the internal contract only.

Until this report exists, E4 and E5 remain NOT_IMPLEMENTED. A green unit test or
a synthetic 100-case smoke run is not this gate.
