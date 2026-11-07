# Phase E evidence hardening — candidate evaluation, gate falsifiability, Tier 2 lock

**Date**: 2026-11-07
**Branch**: `feat/phase-e-quality-gates` (continues the previous round; no history
rewrite, all fixes are forward commits on top of `dafd05f18def82743ea1c7021be4809abfe28ae4`)
**Base branch**: `feat/offline-multilingual-backend` at `ade5d4aabb5f686db89434eb481e07e0b86255a6`
(`git merge-base --is-ancestor` exit 0, re-verified this round)
**Scope**: this round hardens the *evidence* — the evaluator, the CI gate and the
Tier 2 verifier — so every recorded claim can be re-derived and fails closed. It
is not E4/E5 work: E4 (cross-language fusion) and E5 (detection latching) remain
NOT_IMPLEMENTED, and the 15-case Tier A baseline is still not a quality
qualification.

## 1. Sanitized environment summary

| Property | Value |
| --- | --- |
| OS | Windows 10.0.26200 x64; workspace on a OneDrive-synced volume |
| Toolchain | Visual Studio 2022 Professional, MSVC 19.44, generator Visual Studio 17 2022 x64 |
| CMake / CTest | 3.31.6-msvc6 (VS-bundled), invoked by absolute path (not on PATH in Git Bash) |
| C++ standard | C++17, extensions OFF, `/permissive-` |
| Python | 3.14.3 (stdlib only for evaluator/CI tooling) |
| PowerShell | Windows PowerShell 5.1 available; pwsh 7 **not installed locally** |
| GitHub CLI | not installed (`gh: command not found`); hosted evidence read through the public Actions API |
| Working tree at round start | clean; `HEAD == origin/feat/phase-e-quality-gates == dafd05f`, ahead/behind 0/0 |

## 2. Audit facts established before any change

- `git fetch --all --prune` exit 0; local/remote no divergence; the parent
  branch tip `ade5d4a` is an ancestor of HEAD.
- Hosted runs read from `api.github.com/repos/shijianus/EpoCanva/actions/runs`:
  #1 `42325315561` head `38b8693` failure; #2 `42334561912` head `a9f2432`
  failure; #3 `42336295759` head **`022ff38f3626f8b724389dadcef142ed61529cf4`**
  success; #4 `42336892770` head `dafd05f` success. This confirmed the
  previous report's §7.3 was wrong (it named `2145cf0` as the head of run
  #3; that commit is only an ancestor) — corrected in that report by addendum.
- The previous round's E6 report (`build/candidate-eval/report.json`, kept
  unchanged) recorded `identity.repo_commit = 38b8693…` — a caller-supplied
  value written in unverified, while the tree under measurement was at
  `12c7647` with an untracked report file. Confirmed from the report bytes;
  corrected by addendum and superseded by this round's run (§6).

## 3. Defects found and fixed (one commit each)

### 3.1 Evaluator identity was caller-supplied — `3facd83`

`--repo-commit` was copied into the report identity verbatim. Now the
evaluator executes `git rev-parse --verify HEAD` itself, requires the root to
be exactly a git work-tree top level, and records `repo_commit`, `source_ref`
(empty when detached — never fabricated), `source_tree_clean`
(`git status --porcelain=v1 -uall`), the declared `--mode`, the evaluator
schema version (`epocanva-candidate-eval-v2`) and the quality policy version
(`epocanva-candidate-quality-policy-v1`), plus corpus, manifest, bindings,
resource-index, harness, per-language and per-profile model SHA-256s.
`--expected-repo-commit` is an assertion that fails in both modes on
mismatch; qualification mode additionally requires the clean tree. Same-
language staged profiles must share one model: disagreement nulls the
per-language hash with a note and the per-profile map stays authoritative —
the first profile's hash is never silently adopted.

### 3.2 The quality gate could claim anything — `868174b`

`quality_gate` was a single bool fed only by case minimums. It is now a
policy-versioned predicate set (13 mandatory predicates: minimum samples,
exact process/repeat protocol, all expected cases executed, no extra cases,
validation failures empty, cross-process and in-process determinism, empty
menu rate, raw contract, quality metrics with grouped CIs, latency/memory
recorded, privacy/ABI status, wrong-language observability) with statuses
`failed` / `not_evaluable` / `passed`; `met` is true exactly when `passed`.
Baseline mode writes reports for unmet quality with exit 0; structural,
identity, schema and whole-run integrity failures exit 1 in both modes;
qualification mode exits nonzero whenever the gate is not passed.
`wrong_language_rate` is `null` with an explicit `not_observable` status —
the v1 candidate ABI carries no per-candidate language attribution, so 0.0
and Unicode script heuristics are both forbidden. Consequence, recorded
honestly: under the current ABI, policy v1 can never reach `passed`
(`wrong_language_observable` is not evaluable), so qualification mode
cannot exit 0 until the ABI exposes candidate language — an E4-scope fact.

### 3.3 Artifacts and run results were unvalidated — `dde477e`

- Containment: all evaluation artifacts (report, runs, rime worktree, temp
  file) must resolve strictly inside the canonical `<repo>/build/`; `..`
  components, absolute escapes, source-tree paths, outside-repo paths and
  junction/symlink escapes are rejected. The committed test asserting that
  outside-repo temp dirs are acceptable now asserts failure.
- Run results: exactly one `run_meta` matching language/repeat/case_count,
  results file name, the v1 candidate kind enum (0–6), contiguous unique
  ranks, boolean flags, nonnegative latency and byte offsets, uint64 lexeme
  ids, UTF-8 surfaces, and against the corpus: exactly the expected case set
  (no missing, no extra), matching profile ids, input byte ranges inside the
  raw UTF-8 input. The harness must exit 0 with **empty stderr** — verified
  by a real subprocess fixture.
- `\uXXXX` escapes are now rejected lexically on the raw JSON bytes before
  decoding (the old decoded-substring check both missed real escapes and
  false-positived on escaped backslashes); the genuine `"\u0068"`-bytes
  corpus line is the integration negative and the `\\u0068` literal
  backslash the positive control. The committed corpus contains zero
  backslashes (byte-counted).

### 3.4 The CI gate could not fail — `917a045`

- `git diff --check` examined the clean checkout's working tree (always
  empty). Replaced by `tools/ci/check_whitespace.py`: `pull_request` ranges
  start at merge-base(base, head), `push` uses `before..after`, branch
  creation falls back to the default-branch merge-base, and any
  underivable range fails closed. Fixture-repo tests pin committed trailing
  whitespace (fails), clean commits (pass), base-branch whitespace (not
  re-reported), the merge-base range, branch-creation fallback, and the
  fail-closed paths.
- The contract regex proved a pattern, not a set. Every contract test now
  carries the `contract-gate` label; `tools/ci/contract_inventory.py`
  compares the real `ctest --show-only=json-v1` inventory against the
  version-controlled `tools/ci/contract-tests.json` (12 names) exactly —
  deleted, duplicated or unregistered tests fail before anything runs — and
  the suite then executes by label. The workflow builds the aggregate
  target `epocanva_contract_gate_tests` instead of a hand-maintained list.
- Actions hardened: checkout v7.0.1 (`3d3c42e5aac5ba805825da76410c181273ba90b1`),
  setup-python v7.1.0 (`6d4fd22b20465ccc0f7354476588e8df7c9ca0e0`),
  upload-artifact v7.0.2 (`cf430e030ddbb5b0abf93d22962f4752f3646cd9`), all
  pinned by full commit SHA (peeled SHAs from `git ls-remote`); `persist-
  credentials: false`; `fetch-depth: 0` for the comparison objects; Python
  pinned to 3.14; toolchain-version log step; the official gate on
  `windows-2022` (the verified image family) with a `windows-latest`
  continue-on-error canary.

### 3.5 The Tier 2 verifier trusted the lock and the checkout — `6a5572f`

`verify_inputs.ps1` now enforces the lock schema itself (format/owner/
purpose, entry kinds, unique repo-relative paths without traversal, declared
allowed canonical roots, case-sensitive digest/commit formats, positive
integer sizes — exit 2) and, per entry: file entries resolve their final
physical path via `GetFinalPathNameByHandle`, so a symlink/junction that
relocates the pinned bytes fails (`REPARSE_ESCAPE`) while OneDrive cloud
placeholders — reparse tags that never relocate a path — still verify; this
matters because the naive "reject any reparse point" rule rejected the real
22-entry tree on this OneDrive-synced workspace and was corrected to reject
escapes, not placeholders. Commit entries additionally require
`git status --porcelain=v1 -uall` empty and recursive submodule status clean:
moved (`+`) or conflicting (`U`) submodules fail; an uninitialized (`-`)
submodule has no modifiable content and cannot be initialized without
network I/O the contract forbids, so absence is accepted (the real
`third_party/librime` checkout has four uninitialized nested deps; its
porcelain is empty and it now verifies).

## 4. Test evidence (this machine, this round)

| Suite | Command | Result |
| --- | --- | --- |
| ci_inputs_lock | `python -B -m unittest discover -s tests/ci_inputs_lock -p "test_*.py"` | **30 OK** (was 18; +12 hardening tests) |
| candidate_evaluation | `python -B -m unittest discover -s tests/candidate_evaluation -p "test_*.py"` | **68 OK** (was 29; +39 identity/gate/schema/containment tests) |
| capability_matrix | same form | **20 OK** |
| backend_resource_staging | same form | **40 OK, 2 declared skips** (real-resource tests are opt-in by env var) |
| ci_gate (new) | `python -B -m unittest discover -s tests/ci_gate -p "test_*.py"` | **19 OK** |
| root build | `cmake --preset vs2022-x64-debug` then `cmake --build build/vs2022-x64-debug --config Debug --parallel 8` | exit 0 |
| root ctest | `ctest --test-dir build/vs2022-x64-debug -C Debug --no-tests=error --output-on-failure` | exit 0 — **88 total, 88 passed, 0 failed, 1 skip** (`learning.repository.symlinks`, symlink privilege) |
| contract inventory | `python -B tools/ci/contract_inventory.py --ctest <ctest> --build-dir build/vs2022-x64-debug --config Debug --expected tools/ci/contract-tests.json` | exit 0 — exact match, 12 tests |
| contract suite by label | `ctest --test-dir build/vs2022-x64-debug -C Debug -L contract-gate --no-tests=error` | exit 0 — **12/12 passed** |
| whitespace gate (real range) | `python -B tools/ci/check_whitespace.py --event push --before-sha dafd05f… --after-sha <HEAD> --baseline-ref origin/main` | exit 0 — clean |
| manifest freshness | locale probe `--manifest build/ci/manifest/backend-bindings.json` + `git diff --no-index --exit-code` | exit 0, 0 — byte-identical |
| Tier 2 verifier (real tree) | `powershell -NoProfile -ExecutionPolicy Bypass -File tools/ci/verify_inputs.ps1` | exit 0 — **22 of 22 entries verified** (21 file bytes + byte-clean librime checkout) |

The backend (tools/backend) and windows-product suites were **not** re-run in
this round: no backend or product source changed (the harness binary rebuilt
byte-identically in function — same source; its SHA-256
`eeb6a445a86f91d649c9bf25c4958346ffc75c3fd55102a74001982296e6f6e6` is
recorded in the new baseline report). Their last full evidence remains the
previous round's §7 rows 8 and 12.

## 5. Commits (fix-forward, no history rewrite)

1. `3facd83` fix(eval): bind reports to the real clean source tree
2. `868174b` fix(eval): make candidate evaluation fail closed
3. `dde477e` fix(eval): enforce result schema and build-only artifacts
4. `917a045` fix(ci): make contract inventory and diff checks falsifiable
5. `6a5572f` fix(ci): harden Tier 2 input verification
6. (docs) docs(phase-e): reconcile evidence hardening and hosted evidence

Each code commit ran its suite before committing; §4 lists the combined
final state.

## 6. The new E6 baseline (run 2, real clean HEAD)

Command (exit 0), from a clean tree at `6a5572fc11d93f296bcbcd14096336492b933d5b`:

```text
python -B tools/backend/stage_resources.py --product-root build/qg-e6-staging-r2/product --oov error
python -B tools/backend/evaluate_candidates.py \
  --cases data/eval/candidate-v1/cases \
  --manifest data/eval/candidate-v1/manifest.json \
  --resources build/qg-e6-staging-r2/product \
  --harness build/backend-service/candidate-evaluation-tests/Release/epocanva_candidate_eval_cli.exe \
  --output build/candidate-eval-r2/report.json \
  --mode baseline --expected-repo-commit 6a5572fc11d93f296bcbcd14096336492b933d5b \
  --rime-dll build/librime-english/rime/install/lib/rime.dll \
  --rime-dll-sha256 0e202bb871bd7d7b272feb461ef7db0f501187a51ca4f70dc8b8ce8967eecd2f \
  --rime-shared build/backend-chinese-ranked/shared
```

Identity (all evaluator-derived): `repo_commit 6a5572f…`,
`source_ref feat/phase-e-quality-gates`, `source_tree_clean true`, `mode
baseline`; `backend_manifest_sha256 0b638426074914fec728319a949f4fb3b9c8aec2b449ec95a99fb276411809ce`,
`manifest_sha256 8c58ee2a8c991b3628d476011991e7980da0f86966cac8df3bb6c7b9d797329c`,
`resource_manifest_sha256 f6acd2eaf5c0a660404944d5ddcaaad1477f369035247b057e24c2fb543e9ffd`,
`harness_sha256 eeb6a445a86f91d649c9bf25c4958346ffc75c3fd55102a74001982296e6f6e6`;
per-language models unchanged from the committed pins (en `51717a82…`, zh
`cce13364…`, fr `e5d6550d…`, ja `b40382f5…`, ko `4bb6010c…`).

Numbers (2 fresh processes per language, 2 in-process repeats per case):
en top1 0.0 / MRR 0.142; zh 0.0 / 0.178; fr 0.333 / 0.667; ja 0.0 / 0.5;
ko 1.0 / 1.0; raw_first 1.0 for en/zh/fr/ja and 0.5 for ko (the known
two-set composed-Hangul property). Resource load 2.8–4.7 s; peak working
set ≈ 408–412 MB; zhuyin case reported separately as unverified.

**quality_gate.status = failed** (`claim: gate-failed`), unmet reasons:
- `minimum_samples`: en 2/1001, zh 3/1001, fr 3/1001, ja 4/1001, ko 2/500 —
  the Tier A corpus is a baseline, far below the E4 entry minimums;
- `validation_failures_empty`: 1 case-level failure (below);
- `cross_process_determinism` / `in_process_repeat_determinism`: fr 0.667;
- `raw_contract`: ko raw_present/raw_first 0.5.
`not_evaluated_predicates`: `wrong_language_observable` (ABI limitation).
Exit code 0 in baseline mode — recorded evidence, not a qualification.

**New finding, exactly what the hardened gate exists to catch**: case
`fr-prefix-francais-0001` is **not deterministic**. In the second fresh
process, the two in-process repeats diverge — ordinal 2–3 alternate between
kind-2 SPELLING rows (`francisa`, `franchis`) and kind-4 EXPANSION rows
(`fais rien aux normes caisse`, …). The first process was internally
consistent. The previous round's evaluator compared only cross-process
sequences and would not have caught a within-process divergence; this run is
the first recorded observation of the fr spelling/expansion ordering
fluctuating between identical queries in one session. This is a real
backend property to be diagnosed in the E6 ranking work — not silenced here.

Artifacts: `build/candidate-eval-r2/report.json` (metrics, no surfaces) and
`build/candidate-eval-r2/runs/*.jsonl` (the only files with candidate
surfaces; never committed, never uploaded).

## 7. Hosted contract gate

Read back from the public Actions pages after the non-forced push
(`dafd05f..3722744`, exit 0); no values were predicted in advance. The
hosted run is the **contract gate only** — it is not a hosted full
backend/product validation: those require the self-hosted runner with the
hash-verified Tier 2 input cache (docs/ci/windows-ci.md §2) and are not
enabled.

| Field | Value |
| --- | --- |
| run id | `42346805346` (windows-ci #5) |
| event | push (`dafd05f..3722744` to `feat/phase-e-quality-gates`) |
| head_sha | `37227442a5e3fd8f38d64712cbedb5b2820f538a` |
| workflow path | `.github/workflows/windows-ci.yml` |
| conclusion | **success** (1 m 13 s; jobs `contract-gate` ≈65 s and `contract-gate-canary` ≈70 s both success) |
| attempt | 1 |
| timestamps | created/observed 2026-11-07 (UTC); per-step timestamps require sign-in |

The green gate now proves more than the previous round's: the whitespace
gate ran over the real committed event range, the exact inventory
(12 contract tests) was verified against the committed set before `ctest -L
contract-gate` executed, the aggregate target built both native probes, and
the canary exercised the same steps under `windows-latest` — which also
exercised the new gate tooling under pwsh 7 (the verifier subprocess itself
runs under Windows PowerShell 5.1 on the runner, as designed).

## 8. Unfinished items, risks

- **`wrong_language_observable` is not evaluable at the v1 ABI** — policy v1
  therefore cannot pass; qualification mode always exits nonzero until E4
  provides per-candidate language attribution. Recorded; not worked around.
- **fr nondeterminism** (§6) is unexplained. It must be diagnosed before any
  E4 comparison discipline starts; it also means `deterministic_cases == 1.0`
  (design §9.2) currently fails for fr on real runs.
- The E6 slice remains a baseline: Tier B corpus, 1,001/500 minimums, the
  warm-up latency regime and §9.3–9.7 of the design document are open.
- pwsh 7 is not installed locally; the verifier's pwsh compatibility rests on
  5.1-verified syntax plus the hosted run (no pwsh claim is made from this
  machine alone).
- Nothing in this round is HOST_VERIFIED: no registered real-host TSF
  session ran; product persistent learning stays closed; no network I/O,
  telemetry, model download or committed-text logging was added.
