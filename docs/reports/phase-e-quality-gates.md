# Phase E quality gates — verification, fixes, and the E6 minimal baseline

**Date**: 2026-11-07
**Branch**: `feat/phase-e-quality-gates`
**Baseline HEAD (unchanged remote head of this round's start)**: `38b8693ef11d4f8192237c11a57c0c47e6695851`
**Base branch**: `feat/offline-multilingual-backend` at `ade5d4aabb5f686db89434eb481e07e0b86255a6` (verified ancestor)
**Status**: the pushed quality-gate commit is now verified for the first time and
its latent defects are fixed forward; the E6 minimal candidate-menu evaluation
baseline is implemented, unit-tested, and recorded against the real staged
resources. Commit-level verification covers every commit.

---

## 1. Sanitized environment summary

| Property | Value |
| --- | --- |
| OS | Windows 10.0.26200 x64 (workspace on a OneDrive-synced volume) |
| Toolchain | Visual Studio 2022 Professional 17.14, MSVC 19.44, generator Visual Studio 17 2022 x64 |
| CMake / CTest | 3.31.6-msvc6 (VS-bundled) |
| C++ standard | C++17, extensions OFF, `/permissive-`, `/utf-8`, `/W4` |
| Python | 3.14.3 (standard library only for all evaluator tooling) |
| Shell | Git Bash |
| GitHub CLI | **not installed** (`gh: command not found`) — see §9 |
| Working tree at round start | clean, on `feat/phase-e-quality-gates` == `origin/feat/phase-e-quality-gates` == `38b8693` |

## 2. Round narrative and what was verified

The previous session created this branch and pushed one commit, `38b8693`
("feat(ci): enable the hosted contract gate and add quality-gate tests"),
covering the Contract Gate workflow, the Tier 2 input-lock contract
(`tools/ci/inputs.lock.json` + `tools/ci/verify_inputs.ps1` + tests), the
deterministic learning-repository fault injection (`learning.repository.faults`),
and the E3 capability-matrix data with its validator test. This round:

1. Re-verified remote and base state (Task 0 commands in §4, all exit 0).
2. Ran the pushed commit's own quality gate locally — and found that the gate
   **did not run everything it claims** (§3.1).
3. Fixed the defects forward (no history rewrite; `38b8693` stays pushed).
4. Implemented the E6 minimal candidate-menu evaluation baseline
   (harness + evaluator + Tier A corpus + tests) and ran it against the real
   staged resources (§5, §6).
5. Re-ran the root and backend suites and the full Windows product suite (§7).

## 3. Defects found in the pushed `38b8693`, and their fixes

All four defects share one root cause: parts of the gate had never been
executed anywhere (unregistered CTests, a workflow step never run), so they
could not fail — which is exactly what §3 of `AGENTS.md` forbids shipping.

### 3.1 The contract gate silently ran 8 of its claimed test groups

The committed workflow regex
`^(locale_registry|candidate_contract|capability_matrix|ci_inputs_lock)\.`
claimed four groups, but:

- `tests/capability_matrix/` had **no CMakeLists.txt at all** — its validator
  had never been registered with any CTest;
- `tests/ci_inputs_lock/CMakeLists.txt` existed but was never included from the
  root `CMakeLists.txt`, and its test name (`ci_inputs.lock_contract`) did not
  match the workflow regex (`ci_inputs_lock\.`) anyway.

`ctest --no-tests=error` only fails when the total match count is zero, so the
gate stayed green with 8 of 10 tests. Fix: a new
`tests/capability_matrix/CMakeLists.txt` registering
`capability_matrix.contract`; the root project now adds both directories
(`ci_inputs_lock` under the same `WIN32` guard as the other PowerShell-using
suite); the test renamed to `ci_inputs_lock.contract`. Root suite grew 84 → 87
registered tests.

### 3.2 The capability-matrix data failed its own validator on first execution

With the validator finally running, `data/locale/capability-matrix.json` failed
validation: the binding-6 (zh-zhuyin, `unverified`) `host_acceptance` row
lacked the mandatory `zhuyin-unverified` note, and behind that, the validator's
uniqueness/coverage keys ignored the `language` field, so the legitimate
language-level rows for `es` and `de` (both `binding_id: null`) falsely
collided as duplicate `(None, engine)` pairs. Fixes: the row's note was
completed and the file re-serialized to canonical bytes (the canonical-bytes
test still passes); the validator now keys uniqueness and coverage on
`(language, binding_id, dimension)`. The negative mutation test still rejects a
duplicated row, and 20/20 matrix tests pass.

### 3.3 The workflow's manifest step referenced a nonexistent probe path

The workflow looked for
`build/vs2022-x64-debug/Debug/epocanva_locale_registry_tests.exe`; the VS
generator actually places it at
`build/vs2022-x64-debug/tests/locale_registry/Debug/epocanva_locale_registry_tests.exe`,
so the enabled workflow would have failed on its first real run. Fixed and
verified end-to-end locally: the probe writes the manifest (exit 0) and the
byte comparison against `data/locale/backend-bindings.json` is fresh (exit 0).

### 3.4 Contract-gate regex extension

The regex now also names `candidate_evaluation\.`, the E6 evaluator suite
registered in the same round (commit 2); at commit 1 the regex simply matches
nothing for that group and the gate remains valid.

## 4. Task 0 commands and observed results

```text
git rev-parse --is-inside-work-tree        -> true
git remote -v                              -> origin https://github.com/shijianus/EpoCanva.git
git branch --show-current                  -> feat/phase-e-quality-gates
git status --porcelain=v1 -uall            -> (empty at start)
git rev-parse HEAD                         -> 38b8693ef11d4f8192237c11a57c0c47e6695851
git fetch --prune origin                   -> (completed, no output)
git rev-parse origin/feat/offline-multilingual-backend
                                           -> ade5d4aabb5f686db89434eb481e07e0b86255a6
git merge-base --is-ancestor ade5d4a... origin/feat/offline-multilingual-backend
                                           -> ANCESTOR-OK (exit 0)
git log --oneline ade5d4a..origin/...      -> (empty: remote head == the pinned base)
```

The branch `feat/phase-e-quality-gates` already existed (created and pushed by
the previous session); this round continues it. No history rewrite, no force
push, no `reset --hard`, no `clean`.

## 5. E6 — the minimal candidate-menu evaluation baseline

Design source: `docs/testing/candidate-evaluation-baseline.md` (updated in this
round with an implementation-status header and the recorded deviations). The
slice consists of:

### 5.1 Tier A corpus (`data/eval/candidate-v1/`)

15 project-authored synthetic cases across en/zh/fr/ja/ko (plus one
`zhuyin` case reported separately as unverified). Every `expected` surface is
the target of a pinned assertion that already exists in
`tests/backend/profiles.cc` (the `kConversions` table, the `Bindings` admitted-id
assertions, `PinyinInitialisms`, and the `Edits` Unicode case) — nothing is
fitted from an unverified run, and the README documents the per-case derivation.
English abbreviation-expansion cases are deliberately excluded because their
expected leaders are model-computed at test time, not authored pins.
`manifest.json` pins each file's SHA-256, the case counts, and
`backend_manifest_sha256`; the unit tests make that pin a freshness gate.

### 5.2 Native harness (`tests/candidate_evaluation/candidate_eval_cli.cc`)

Built as `epocanva_candidate_eval_cli` in the `tools/backend` project (where
`epocanva_backend` exists). It reads the staged resource index
(`data/backend/resources.tsv`), verifies **every** pinned resource hash with the
product's own reviewed SHA-256 helper (`platforms/windows/product/product_io.h`,
BCrypt-backed), verifies the Rime DLL hash against the value from
`build/librime-english/engine-build-receipt.json` supplied on the command line,
copies the reviewed Chinese shared data into an isolated work tree (staged
inputs stay immutable), opens the real 13-profile backend **with no learning
root and `learning_enabled=0`**, and replays the corpus through the public v1 C
API only. Per case it emits one JSON object whose `repeats` array carries the
full ordered candidate menu of each in-process query; stderr stays text-free
(stage names and numeric ABI statuses only); candidate surfaces appear only in
the `--output` JSONL, which the evaluator keeps under `build/`.

Three harness defects were found and fixed during bring-up, each recorded here
because they are exactly the kind of bug a green-silent gate hides: a
`stream.eof()` check after `istreambuf_iterator` reading (the iterator never
sets `eofbit`, so every read "failed" at the wrong stage); relative resource
paths (the backend's `LocalResource` pins absolute drive paths only, so all
path arguments are now resolved to absolute); and the corpus parser rejecting
the documented corpus schema (the harness now skips fields it does not use and
leaves strict validation to the evaluator).

### 5.3 Evaluator (`tools/backend/evaluate_candidates.py`)

Standard-library only, deterministic output (`sort_keys`, `ensure_ascii=False`,
two-space indent, trailing LF). It validates the corpus against every rule of
the design document (ids, languages, schemes, binding ownership via
`data/locale/backend-bindings.json`, UTF-8 and control-scalar checks, the
4,096-byte input bound, splits, groups, licences), validates the manifest,
invokes the harness as a fresh subprocess per language and per run (hashing the
executable before and after), computes the design metrics over the real menus
plus the `suggestion_*` variants, grouped bootstrap CIs, latency statistics,
and refuses to write a report outside `build/` or when any input is invalid
(exit 1) with bad arguments (exit 2). Failures carry case ids and reason
codes, never surfaces.

### 5.4 The recorded baseline (real run, this tree)

Command: §7 row 14. Corpus: 15 cases, 2 fresh harness runs per language,
2 in-process repeats per case. Results:

| Language (verified schemes) | Cases | top1 | top3 | MRR | sug-top1 | sug-MRR | raw first | deterministic | cold ms | warm p95 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| en | 2 | 0.0 | 0.0 | 0.142 | 0.0 | 0.171 | 1.0 | 1.0 | 3.76 | 8.21 |
| zh (pinyin) | 3 | 0.0 | 0.0 | 0.178 | 0.0 | 0.217 | 1.0 | 1.0 | 0.17 | 4.22 |
| fr | 3 | 0.333 | 1.0 | 0.667 | 1.0 | 1.0 | 1.0 | 1.0 | 112.59 | 111.94 |
| ja | 4 | 0.0 | 1.0 | 0.5 | 1.0 | 1.0 | 1.0 | 1.0 | 4.30 | 6.99 |
| ko | 2 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | **0.5** | 1.0 | 0.31 | 0.35 |

- Failures: none. Determinism: 1.0 for every case across all repeats and both
  fresh processes (full ordered-list equality).
- The raw-first contract holds at ordinal 0 for en/zh/fr/ja. **Finding**: for
  ko two-set the kind-0 row carries the composed Hangul rather than the typed
  romanization keys (`dkssud` → raw row `안녕`), so `raw_present_rate`/`raw_first_rate`
  are 0.5 for ko — a real per-language property now recorded, not a defect.
- English baseline: `hel` → raw `hel` first, then `help` (rank 1), `hello`
  (rank 11); morphology rows (`help's`, kind 3) interleave above the lexicon
  completions. `wor` → `work` (1), `world` (4). These numbers are the recorded
  pre-calibration baseline; the E6 ranking work will be judged against them.
- Resource load 2.8–4.7 s per language process; peak working set ≈ 428 MB
  (432 MB for zh, which additionally loads Rime).
- `quality_gate.met == false`, `claim: baseline-only`: 15 cases is far below
  the E4-entry minimums (1,001/500). The gate is explicitly withheld.

## 6. Design choices and rejected alternatives

- **Harness in `tools/backend` tree, tests in root**: the harness needs
  `epocanva_backend`, which only the backend project builds; the Python
  evaluator tests need neither a native build nor Tier 2 inputs, so they are
  registered from the same directory in the root suite and in the CI gate.
  Rejected: a separate top-level project (duplicates the toolchain plumbing the
  backend project already owns).
- **Expectations pinned by existing test assertions**: keeps the corpus an
  independent oracle. Rejected: harvesting expected values from observed
  harness output (circular; would make the baseline unfalsifiable).
- **Validator key `(language, binding_id, dimension)`**: language-level rows
  for several engine-less languages are legitimate; keying only on
  `(binding_id, dimension)` made the committed es/de rows unrepresentable.
- **`suggestion_*` variant metrics added rather than redefining `top1`**: the
  design's `top1` is kept verbatim (full menu including the raw row), and the
  variant records suggestion ordering without silently changing metric
  semantics mid-phase. The design document records both.
- **Fix-forward commits**: `38b8693` is already pushed; rewriting it is
  forbidden. Each fix is a new reviewed commit.

## 7. Commands, exit codes and test counts

| # | Command | Exit | Result |
| ---: | --- | ---: | --- |
| 1 | Task 0 git sequence (§4) | 0 | base verified, tree clean |
| 2 | `git diff --check` (after all edits) | 0 | no whitespace errors |
| 3 | `cmake --preset vs2022-x64-debug` | 0 | root configure with new test dirs |
| 4 | `cmake --build build/vs2022-x64-debug --config Debug --parallel 8` | 0 | root build |
| 5 | `ctest --test-dir build/vs2022-x64-debug -C Debug --no-tests=error --output-on-failure` | 0 | **87 total, 87 passed, 0 failed, 1 skip** (`learning.repository.symlinks`, symlink privilege) |
| 6 | `cmake -S tools/backend -B build/backend-service -G "Visual Studio 17 2022" -A x64 -DEPOCANVA_BACKEND_WITH_FRENCH_DICTIONARY=ON` | 0 | backend configure incl. harness target |
| 7 | `cmake --build build/backend-service --config Release --parallel 8` | 0 | backend build |
| 8 | `ctest --test-dir build/backend-service -C Release --no-tests=error --output-on-failure` | 0 | **59 total, 59 passed, 0 failed** (incl. `backend.profiles.real_acceptance`, `backend.headless.real_five_language_api`, new `candidate_evaluation.contract`) |
| 9 | Python suites: `backend_resource_staging` 40 OK (2 opt-in skips); `ci_inputs_lock` 18 OK; `capability_matrix` 20 OK; `candidate_evaluation` 29 OK | 0 | all green |
| 10 | `epocanva_locale_registry_tests --manifest build/ci/manifest/backend-bindings.json` then `git diff --no-index` | 0, 0 | manifest export + byte-fresh gate |
| 11 | `python -B tools/backend/stage_resources.py --product-root build/qg-e6-staging/product --oov error` | 0 | 13 profiles, 145 inputs |
| 12 | Product configure/build/ctest with `EPOCANVA_BUILD_PRODUCT=ON`, staged resources, French ON | 0, 0, 0 | §7.1: 236 total, 234 passed, 0 failed, 2 skips, 945.19 s |
| 13 | `python -B tools/backend/evaluate_candidates.py --cases data/eval/candidate-v1/cases --manifest data/eval/candidate-v1/manifest.json --resources build/qg-e6-staging/product --harness build/backend-service/candidate-evaluation-tests/Release/epocanva_candidate_eval_cli.exe --output build/candidate-eval/report.json --repo-commit 38b8693ef11d4f8192237c11a57c0c47e6695851 --rime-dll build/librime-english/rime/install/lib/rime.dll --rime-dll-sha256 0e202bb871bd7d7b272feb461ef7db0f501187a51ca4f70dc8b8ce8967eecd2f --rime-shared build/backend-chinese-ranked/shared` | 0 | report written; `quality claim withheld: below E4 gate minimums`; failures `[]` |

The Rime DLL SHA-256 was cross-checked against
`build/librime-english/engine-build-receipt.json` (`0e202bb8…eecd2f`) before the
run. The evaluator's report records the corpus, bindings and harness hashes;
the runs directory (`build/candidate-eval/runs/`) is the only artifact carrying
candidate surfaces and is never committed.

### 7.1 Windows product suite (row 12)

| Step | Exit | Result |
| --- | ---: | --- |
| Configure `platforms/windows/engine_bridge` (`EPOCANVA_BUILD_PRODUCT=ON`, staged resources from row 11, French ON, both Rime DLLs) | 0 | Rime pin + DLL receipt verified by configure |
| Build Release `--parallel 8` | 0 | full product build |
| `ctest --test-dir build/windows-product -C Release --no-tests=error --output-on-failure` | 0 | **236 total, 234 passed, 0 failed, 2 skips**, 945.19 s |

The two skips are the declared environmental ones
(`windows.tsf_context_observer.real_focus`, `learning.repository.symlinks`).
The 236th test versus the follow-up report's 235 is
`learning.repository.faults` — added by the already-pushed `38b8693` into
`tests/learning_repository`, which the product tree includes; this round is the
first time it executed in the product tree (passed, 0.54 s).

### 7.2 Commit-level verification (detached worktrees)

- `a37849c` (commit 1): fresh worktree at the commit; root configure exit 0;
  contract-target build exit 0; five-group contract regex ctest exit 0 —
  **10/10 passed** (locale 5, candidate 3, `capability_matrix.contract`,
  `ci_inputs_lock.contract`; the `candidate_evaluation` regex group matches
  nothing yet). `git status` clean after the run. Worktree removed.
- `12c7647` (commit 2): same procedure — **11/11 passed** (adds
  `candidate_evaluation.contract`); worktree clean after the run; removed.
- The three-suite totals in §7 rows 5/8/7.1 ran on the working tree that is
  byte-identical to commit 2 (`git status` shows only the untracked report at
  that point), so those runs are the full-suite evidence for commit 2's tree.
- Commit 3 is documentation only (this file).

## 8. Files added or modified this round

**Commit 1 — `fix(ci): make the contract gate run what it claims`**
- `tests/capability_matrix/CMakeLists.txt` (new) — registers `capability_matrix.contract`
- `tests/ci_inputs_lock/CMakeLists.txt` — rename to `ci_inputs_lock.contract`, registration note
- `CMakeLists.txt` — root adds `tests/capability_matrix` and (WIN32) `tests/ci_inputs_lock`
- `data/locale/capability-matrix.json` — binding-6 note completed; canonical bytes preserved
- `tests/capability_matrix/test_capability_matrix.py` — uniqueness/coverage key gains `language`
- `.github/workflows/windows-ci.yml` — real probe path; regex names `candidate_evaluation`

**Commit 2 — `feat(eval): add the E6 minimal candidate-menu evaluation baseline`**
- `data/eval/candidate-v1/README.md`, `manifest.json`, `cases/{en,zh,fr,ja,ko}.jsonl` (new, 7 files)
- `tools/backend/evaluate_candidates.py` (new)
- `tools/backend/CMakeLists.txt` — adds the harness subdirectory
- `CMakeLists.txt` — root adds `tests/candidate_evaluation`
- `tests/candidate_evaluation/CMakeLists.txt` (new), `candidate_eval_cli.cc` (new), `test_evaluate_candidates.py` (new)
- `docs/testing/candidate-evaluation-baseline.md` — implementation status, deviations, raw-first finding

**Commit 3 — `docs(phase-e): record quality-gate verification and the E6 baseline`**
- `docs/reports/phase-e-quality-gates.md` (this report)

No file under `third_party/` was touched. No public C ABI change, no learning
schema change, no product-runtime behaviour change. All new generated files are
deterministic and contain no timestamps, absolute paths or machine names.

## 9. Unfinished items, risks, blockers

- **GitHub CLI is not installed** on this machine (`gh: command not found`), so
  the Draft PR cannot be created from here and remote CI runs cannot be
  observed via CLI. The push itself will trigger `windows-ci` on GitHub
  (`push: branches: ["feat/**"]`); the workflow is now verified locally step by
  step, but its first hosted run must still be watched by someone with `gh` or
  browser access. This is the round's only blocker for goal 9.
- The E6 slice is a **baseline**, not the E4 entry gate: Tier B corpus,
  1,001/500-case minimums, the warm-up latency regime and §9.3–9.7 of the
  design document remain open, and `quality_gate.met` is false on purpose.
- English ranking quality is weak in the recorded baseline (`hello` at rank 11
  behind morphology rows); that is now measurable and must not be "improved"
  without the E6 comparison discipline.
- The historical `learning.repository.persistence` io-error and the
  `real_acceptance` failure remain BLOCKED-not-reproduced as recorded in
  `phase-e-followup-implementation.md` §6; nothing in this round changed that
  evidence (backend suite green, 59/59).
- Product persistent learning stays closed (`ProductLearningWritesPermitted()`
  untouched); the E6 harness never enables learning and provides no learning
  root. `zhuyin` remains `unverified` and is reported separately by the
  evaluator. No network I/O, telemetry, model download or committed-text
  logging was added anywhere in this round.
- Nothing in this round is HOST_VERIFIED: the harness and all suites run
  through controlled test hosts and the public/test ABIs, not a registered
  real-host TSF session.
