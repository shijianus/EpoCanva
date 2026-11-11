# Phase E4/E5 — multilingual candidate normalization, dedup, fusion and detection latching

**Date**: 2026-11-11
**Branch**: `feat/phase-e-multilingual-fusion` (from `feat/phase-e-gate-closure` at
`0cb00d8`; the previous round's uncommitted evidence work was first carried
forward as commit `446f9c0` so this branch starts from the recorded gate state)
**Scope**: the fusion layer (E4) and the detection latch wiring (E5). The gate
closure prerequisite is the recorded evidence state described in
`docs/reports/phase-e-evidence-hardening.md` — this round does not re-interpret
or bypass any quality-gate predicate.

## 1. Sanitized environment summary

| Property | Value |
| --- | --- |
| OS | Windows 10.0.26200 x64; workspace on a OneDrive-synced volume |
| Toolchain | Visual Studio 2022 Professional, MSVC 19.44, generator Visual Studio 17 2022 x64 |
| CMake / CTest | 3.31.6-msvc6 (VS-bundled), invoked by absolute path |
| C++ standard | C++17, extensions OFF, `/permissive-` |
| Python | 3.14.3 (stdlib only for evaluator/CI tooling) |
| Working tree at round start | gate-closure branch with one uncommitted evidence round (backed up to `../epocanva-gate-closure-uncommitted.patch`, then committed as `446f9c0`) |
| Runtime network / telemetry / product learning writes | none added; product `ProductLearningWritesPermitted()` remains false |

## 2. What was implemented

### 2.1 The fusion layer (E4) — `engine/fusion/`

Two new files, pure logic, no platform header, no I/O, no network:

- **`fusion_policy.h/.cc`** — the single compiled source of fusion ranking
  parameters (`FusionPolicy` version 1, `dedup_version` 1). Per-source
  admission state is explicit: in policy v1 **every non-raw source is
  `kRestrictedFallback` and `rank_only`**. Its fusion points derive from the
  producer insertion ordinal inside the fallback band `[0, 100]`, so the
  backend's own producer order is the prior and **no uncalibrated raw score is
  ever added to another source's scale**. Raw is a hard tier, not a score.
  Each row carries an `evidence` string; flipping a source to calibrated
  requires a new recorded evaluation and a version bump of the mirror JSON.
- **`fusion_merger.h/.cc`** — deterministic cross-source merge:
  - **fail closed**: a candidate whose contract is missing a required field
    (stable_id, language_tag, scheme_id) or fails `candidates::Validate`
    aborts the whole merge (`kMissingContract` / `kInvalidContract`), and the
    backend refuses the query — no language is ever guessed from script;
  - **raw hard protection**: raw-tier rows always precede suggestions,
    independent of any score;
  - **exact-expansion protection**: a merged row is exact if any of its
    provenance rows was exact, and exact rows precede non-exact rows within
    the suggestion band (defense in depth on top of the backend's own sort);
  - **most-reliable-source survivor**: among duplicates the survivor is the
    more reliable source (lower Source value), then higher backend score, then
    lexeme-ID order, then lower producer ordinal;
  - **provenance auditability**: every merged row lists each folded row's
    stable_id, source, producer ordinal, fusion points and exact flag,
    survivor first — deterministic and repeatable;
  - **dedup key scope** (`dedup-v1`): language + scheme + tier class + text
    key. Language and scheme are part of the identity, so the same surface in
    two languages never collides. The text key folds ASCII A-Z only for
    word/expansion/morphology surfaces in en/fr/ko — exactly the fold the
    candidate contract already makes. **No NFKC, no full Unicode casefold, no
    NFC/NFD equivalence** (the contract tests pin the same honesty);
  - **producer-order perturbation safety**: suggestion ranks derive from
    collection-time producer ordinals, never from vector position, and the
    final order tie-breaks on producer ordinal then stable_id, so delivering
    the same collection in a different transport order yields a byte-identical
    menu (tested).

### 2.2 Machine-readable mirror — `data/eval/candidate-v1/fusion-policy.json`

The one JSON that mirrors the compiled policy: format
`epocanva-candidate-fusion-policy-v1`, every source's calibration, the dedup
rule (folded sources, folded languages, `nfkc: false`,
`unicode_casefold: false`), the score scale bands, the calibration basis
(what evidence justifies v1's rank-only choice) and the fail-closed actions.
`tests/fusion` policy case pins the mirror against the compiled values.

### 2.3 Backend wiring — `engine/backend/backend_api.cc`

After the existing per-candidate contract build, the query now runs
`fusion::Merge` over the collected records (parallel vectors of contracts,
producer ordinals and exact flags), then publishes. A merge failure returns
`EPOCANVA_BACKEND_UNAVAILABLE` rather than publishing a guessed row. Rows
folded by dedup are removed with each survivor carrying its provenance[0]
identity; the published `flow::Candidate::exact_expansion` flag is the merged
one.

### 2.4 Detection latching (E5) — per-session ranker

Fixes the recorded P2.2 defect ("Detection latching in AdaptiveRanker never
exercised in current backend"): `ApplyPersonalRanking` used to construct a
fresh `rk::AdaptiveRanker` per query, so the latch was always empty. The
backend `Session` now owns one `detection` ranker for its lifetime:

- **observation source**: the composition routing itself. A ja keystroke
  observes "ja", ko observes "ko", zh (Rime-piped) observes "zh"; Latin
  sessions observe nothing. No script heuristic on candidate text, nothing
  persisted, nothing reported anywhere (§2 privacy rules hold).
- **default threshold preserved**: 3 consecutive identical observations
  (`kDefaultDetectionConfirmations`), unchanged — no product-owner decision
  to change it exists.
- **explicit selection wins**: the user's language is `request.user_language`
  in every Rank call; the latch only admits the detected language as an
  additional candidate source and the language term keeps the explicit
  language ahead at near-equal scores (tested).
- **reset**: `epocanva_backend_cancel` clears the latch together with the rest
  of the input state, so a stale streak cannot leak across a profile or
  session boundary (tested).
- **one ranker per session, no second model load**: the ranker is a pure
  in-memory object; no resource loads were added anywhere in this round.

### 2.5 Harness link repair — `tests/candidate_evaluation/CMakeLists.txt`

The evaluation static library's dependency closure (now including
`epocanva_fusion`) is linked explicitly into `epocanva_candidate_eval_cli`;
MSBuild does not propagate a static library's project references into the
consumer's link. The list mirrors `engine/backend/CMakeLists.txt` and is
marked as needing to stay in step. The French dictionary link is
`$<TARGET_NAME_IF_EXISTS:...>` because the backend links it only under the
French option.

### 2.6 Contract-gate inventory — `tools/ci/contract-tests.json`

The five `fusion.*` tests carry the `contract-gate` label; the inventory now
expects 17 tests and `contract_inventory.py` verifies an exact match.

## 3. Key design choices and rejected alternatives

| Decision | Rejected alternative | Why |
| --- | --- | --- |
| All non-raw sources rank-only fallback in policy v1 | Fit linear/offset per source now | No recorded corpus justifies a cross-source scale; the merge probe (recorded evaluation, §4) reproduces every baseline winner with rank-only points, and any fitted weight without that evidence would violate §3/§6 honesty. Calibration is a version-bump away. |
| Dedup key includes language + scheme + tier | Dedup by text bytes alone | The candidate contract explicitly scopes identity by language and scheme; folding fr "paris" into en "paris" would erase a distinct admitted identity. |
| ASCII A-Z fold only for word-class sources in en/fr/ko | NFKC or full casefold | Contract §2.3 forbids adopting NFKC/casefold silently; the contract tests already pin NFC≠NFD. |
| Merger input = already-validated contract records | Merger re-derives language from script | Fail-closed §一.2: missing fields refuse the query. |
| Latch observation = composition routing | Observe from candidate-script heuristics | A script guess on candidates is exactly what §一.2 forbids; routing is the pipeline's own language signal and it is local-only. |
| Session-owned ranker, reset on cancel | Reset on every query | Resetting per query would reproduce the P2.2 behavior (latch can never trigger). Profile/generation boundaries are enforced by the session's existing Check() plus the cancel-path reset. |

## 4. Verification — exact commands and exit codes

All commands were run and their exit codes observed; nothing below is inferred.

### 4.1 Fusion module and tests (root tree, Debug)

| Command | Exit | Result |
| --- | --- | --- |
| `cmake --preset vs2022-x64-debug` | 0 | configure with the new `engine/fusion` and `tests/fusion` |
| `cmake --build build/vs2022-x64-debug --config Debug --target epocanva_fusion epocanva_fusion_tests -- -m` | 0 | module + tests build clean (/W4, /permissive-) |
| `epocanva_fusion_tests.exe --policy` | 0 | `RESULT PASS 18 checks` |
| `epocanva_fusion_tests.exe --dedup` | 0 | `RESULT PASS 7 checks` |
| `epocanva_fusion_tests.exe --merge` | 0 | `RESULT PASS 42 checks` |
| `epocanva_fusion_tests.exe --mixed` | 0 | `RESULT PASS 50 checks` |
| `epocanva_fusion_tests.exe --forcefail` | 1 | negative control: the harness reports red |

During development three real defects were caught by these tests and fixed
(they can fail): the first merger version folded same-surface rows across
languages (violating the contract's language-scoped identity), derived
suggestion rank from vector position (perturbation-visible), and lacked the
exact-expansion ordering rule. The perturbation test initially encoded the
wrong expectation (shuffling ordinals is a different collection, not a
perturbation) — corrected to shuffle vectors while keeping row-ordinal pairs.

### 4.2 Detection latch tests

| Command | Exit | Result |
| --- | --- | --- |
| `cmake --build build/vs2022-x64-debug --config Debug --target epocanva_adaptive_ranking_tests -- -m` | 0 | |
| `epocanva_adaptive_ranking_tests.exe --detection_latch` | 0 | `RESULT PASS 23 checks` — threshold, low confidence, manual override, boundary reset, plus the same-surface ranker rule |
| `epocanva_adaptive_ranking_tests.exe --mixed_language` / `--ordering` / `--bounds` | 0/0/0 | neighbours unaffected |

`adaptive_ranking.detection_latch` is registered as its own CTest entry.

### 4.3 Backend suites (backend-service tree, Release, fusion wired in)

| Command | Exit | Result |
| --- | --- | --- |
| `cmake -S tools/backend -B build/backend-service` | 0 | reconfigure |
| `cmake --build build/backend-service --config Release --target epocanva_backend epocanva_backend_cli epocanva_backend_contract_tests epocanva_backend_context_regressions epocanva_backend_learning_integration epocanva_backend_session_tests epocanva_candidate_eval_cli epocanva_backend_profiles epocanva_backend_real_languages -- -m` | 0 | all build with the fusion pass inside `epocanva_backend` |
| `epocanva_backend_contract_tests.exe <fixtures>/model.tsv <fixtures>/model.sha256` | 0 | `PASS backend contracts` |
| `epocanva_backend_session_tests.exe protocol|context|privacy|validation|catalogs|reset|failure|wrong_thread` | 0 ×8 | all session modes pass |
| `epocanva_backend_context_regressions.exe --unit <abs-build-path>` | 0 | `checks=890 failures=0` (the scratch path must be absolute-inside-build; the earlier relative-path failure was a test-argument error, not a product failure) |
| `epocanva_backend_learning_integration.exe <model> <sha> <scratch>` | 0 | `RESULT PASS` (learning persistence ranking unaffected) |
| `epocanva_backend_real_languages.exe <repo>` | 0 | `PASS actual five-language headless API` (peak working set 761,540,608 bytes recorded by the test itself) |
| `epocanva_backend_profiles.exe <repo> <scratch> 0e202bb8…` | 0 | **12/12 profile acceptance cases, 26,052 checks, 19 verified assets** — the real Rime Chinese engine, Japanese decoder, Korean two-set, French Hunspell and English paths all pass with the fusion layer in the publish path |

### 4.4 E6 evaluator re-run (single-language quality vs the recorded baseline)

```
python -B tools/backend/stage_resources.py --product-root build/fusion-staging/product --oov error   (exit 0; 13 profiles, 145 provenance inputs)
python -B tools/backend/evaluate_candidates.py --mode baseline \
  --cases data/eval/candidate-v1/cases --manifest data/eval/candidate-v1/manifest.json \
  --resources build/fusion-staging/product \
  --harness build/backend-service/candidate-evaluation-tests/Release/epocanva_candidate_eval_cli.exe \
  --output build/candidate-eval-fusion/report.json \
  --rime-dll build/librime-english/rime/install/lib/rime.dll \
  --rime-dll-sha256 0e202bb871bd7d7b272feb461ef7db0f501187a51ca4f70dc8b8ce8967eecd2f \
  --rime-shared build/backend-chinese-ranked/shared                                     (exit 0)
```

Per-language comparison, recorded baseline (`build/candidate-eval/report.json`,
evidence-hardening round) vs this fusion run (`build/candidate-eval-fusion/`):

| Language | Top-1 | MRR | Sugg-Top-1 | raw_present | raw_first | empty_menu | wrong_language | warm P95 ms (base→fusion) | peak WS bytes (base→fusion) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| en | 0.0 → 0.0 | 0.1417 → 0.1417 | 0.0 → 0.0 | 1.0 → 1.0 | 1.0 → 1.0 | 0.0 → 0.0 | 0.0 → 0.0 | 8.21 → 6.74 (−18%) | 427,913,216 → 427,999,232 (+86,016) |
| zh | 0.0 → 0.0 | 0.1778 → 0.1778 | 0.0 → 0.0 | 1.0 → 1.0 | 1.0 → 1.0 | 0.0 → 0.0 | 0.0 → 0.0 | 4.22 → 4.06 (−4%) | 431,521,792 → 431,607,808 (+86,016) |
| fr | 0.333 → 0.333 | 0.6667 → 0.6667 | 1.0 → 1.0 | 1.0 → 1.0 | 1.0 → 1.0 | 0.0 → 0.0 | 0.0 → 0.0 | 111.94 → 117.55 (+5.0%) | 427,876,352 → 427,913,216 (+36,864) |
| ja | 0.0 → 0.0 | 0.5 → 0.5 | 1.0 → 1.0 | 1.0 → 1.0 | 1.0 → 1.0 | 0.0 → 0.0 | 0.0 → 0.0 | 6.99 → 6.63 (−5%) | 427,933,696 → 428,068,864 (+135,168) |
| ko | 1.0 → 1.0 | 1.0 → 1.0 | 1.0 → 1.0 | 0.5 → 0.5 | 0.5 → 0.5 | 0.0 → 0.0 | 0.0 → 0.0 | 0.35 → 0.31 (−10%) | 427,913,216 → 427,909,120 (−4,096) |

Against the task's quality requirements:

- raw_present_rate=1.0, raw_first_rate=1.0, empty_menu_rate=0.0,
  wrong_language_rate=0.0 — **unchanged and within target for en/zh/fr/ja**
  (ko's 0.5 raw_present is the recorded pre-existing two-set property: the
  "raw" row carries the composed Hangul, not the typed keys);
- **no Top-1/MRR regression in any language** — every value is identical to
  the baseline (the rank-only policy keeps single-language menus in producer
  order, which is the baseline order);
- warm P95 increment worst case fr +5.61 ms = +5.0% relative — inside
  max(20%, 5 ms) (the fr absolute number includes the upstream Hunspell wall
  clock budget, see §6);
- peak working set increment worst case ja +135,168 bytes ≈ 0.13 MiB — far
  inside max(20%, 32 MiB);
- **no second model load**: resource load times moved base 2.84–4.72 s →
  2.84–4.39 s (no new loads; the fusion layer loads nothing).

### 4.5 Root full build + ctest + contract gate

| Command | Exit | Result |
| --- | --- | --- |
| `cmake --build build/vs2022-x64-debug --config Debug -- -m` | 0 | whole root tree |
| `ctest --test-dir build/vs2022-x64-debug -C Debug --no-tests=error --output-on-failure` | 8 | **92/94 passed**, 1 skip (`learning.repository.symlinks`, symlink privilege — as recorded before); 2 failed: `english.core.spelling_cost_trial_on_pilot_lexicon` and `learning.repository.errors` |
| re-run `epocanva_learning_repository_tests.exe errors` ×2 | 0, 0 | **passes in isolation** — the ctest failure is an environmental/file-state interaction (OneDrive volume), not a product regression; same test passed inside the same ctest run in previous rounds |
| re-run `epocanva_english_core_tests.exe --pilot-bench <pilot.tsv>` | 0 | **passes in isolation** (worst-p95 23,365 µs vs the 25,000 µs budget); the ctest-run failure measured 27,905 µs — a load-sensitive timing miss of a performance budget, recorded honestly here as such |
| `ctest -L contract-gate --no-tests=error --output-on-failure` | 0 | **17/17 passed** including `fusion.policy`, `fusion.dedup`, `fusion.merge`, `fusion.mixed`, `fusion.can_fail`, `adaptive_ranking.detection_latch` |
| `python -B tools/ci/contract_inventory.py --ctest <ctest> --build-dir build/vs2022-x64-debug --config Debug --expected tools/ci/contract-tests.json` | 0 | `exact match, 17 tests` |

## 5. Files added / modified

Added:
- `engine/fusion/CMakeLists.txt`
- `engine/fusion/fusion_policy.h` / `fusion_policy.cc`
- `engine/fusion/fusion_merger.h` / `fusion_merger.cc`
- `tests/fusion/CMakeLists.txt` / `tests/fusion/main.cc`
- `data/eval/candidate-v1/fusion-policy.json`
- `docs/reports/phase-e-multilingual-fusion.md` (this report)

Modified:
- `engine/backend/backend_api.cc` — fusion pass in the publish path; session-owned detection ranker; observation at composition; latch reset on cancel
- `engine/backend/CMakeLists.txt` — link `epocanva_fusion`
- `CMakeLists.txt` — add `engine/fusion` and `tests/fusion` subdirectories; contract-gate aggregate depends on `epocanva_fusion_tests`
- `tests/adaptive_ranking/main.cc` — new `DetectionLatchTests` (threshold, low confidence, manual override, boundary reset, same-surface rule)
- `tests/adaptive_ranking/CMakeLists.txt` — `detection_latch` CTest entry
- `tests/candidate_evaluation/CMakeLists.txt` — explicit dependency-closure link for the eval CLI
- `tools/ci/contract-tests.json` — inventory extended with the five `fusion.*` tests (exact match verified)

## 6. Unfinished items, risks, and the fr flake

- **fr `fr-prefix-francais-0001` nondeterminism — root cause identified,
  fix out of scope.** The evaluator re-run reproduced the flake first recorded
  in the evidence-hardening report (§6/§8 there). This round diagnosed it to a
  verified source: Hunspell's `suggestmgr.cxx` checks a **wall-clock budget**
  (`TIMELIMIT_SUGGESTION_MS = 100 ms`, `atypes.hxx:108`; checks at
  `suggestmgr.cxx:252–314` in the pinned upstream
  `build/backend-french-dependency/hunspell-7d725f…`) inside
  `native->suggest()`. The fr completion path p95 sits at ~112–118 ms, so the
  suggestion set can differ between identical queries depending on machine
  load — the spelling/expansion rows fluctuate exactly as recorded. Upstream
  is byte-clean under §7, so this cannot be patched in place; the honest
  options (a patched build with the budget disabled for our fixed inputs, or
  a determinism-ordered post-filter) are E6 work and are listed in
  OPEN_QUESTIONS. Deterministic_case_rate for fr is therefore **not 1.0 on
  this machine** — same as the recorded baseline; no regression, no silence.
- The two ctest failures (`pilot-bench` timing, `learning.repository.errors`
  file state) both pass in isolation on the same tree; neither touches the
  fusion or latch code. They are recorded, not explained away.
- E6 entry-gate minimums (1,001/500 cases), Tier B corpus and the calibrated
  (non-rank-only) cross-source scale remain open, as the quality-gate report
  already states. Policy v1 deliberately does not claim "calibrated".
- Spanish and German remain NOT_IMPLEMENTED engines — nothing in this round
  registers or fabricates them.
- The task brief's mixed-language families are covered at the fusion layer
  (12 families, `fusion.mixed`) and the profile-acceptance suite exercises
  the real five-language engines end to end; a dedicated mixed-language
  external corpus remains E7/E6 work.

## 7. Git

Local commits only; **no push, no tag, no PR, no history rewrite** (§9 and the
task authorization).

1. `446f9c0` feat(eval): carry forward the gate-closure evidence work
   (the previous round's uncommitted tree, protected before any fusion change)
2. (this round) feat(fusion): add the versioned fusion policy and deterministic merger
3. (this round) feat(backend): run the fusion merge in the candidate publish path and own the detection latch per session
4. (this round) test(fusion): fusion/detection/mixed-language suites and the contract inventory update
5. (this round) docs(phase-e): record the E4/E5 fusion round

(Exact SHAs are printed by `git log --oneline` at read time; the final list is
verified in the report commit itself.)
