# Phase E follow-up — E1.1, quality fixes, E2 slice, and E3–E8 gates

**Date**: 2026-11-06 (§1–§12); §13 stabilization addendum dated 2026-11-07
**Branch**: `feat/offline-multilingual-backend`
**Baseline HEAD (audited, unchanged remote head)**: `0798bcb3fda8aade3ccb56758d187d90a306c0eb`
**Previous audit HEAD (historical report)**: `0ec6072a952c2e0027f61e6d63d0c2c1c2317d9b`
**Working tree at report time**: modified/untracked, not committed (that brief
forbade commit/push; nothing was committed or pushed at that time). The §13
addendum records the later round that grouped these files into commits A–E and
verified each commit in detached worktrees.
**Status**: E1.1 and the TSF timing fix are implemented and tested; the backend
persistence failure did **not** reproduce and is recorded as BLOCKED, not fixed;
E2 has an internal contract and an English-first vertical slice with the public
C ABI unchanged; E3–E8 are designs and gates, not implementations. Commit-level
and clean-worktree verification are recorded in §13.

---

## 1. Remote and workspace state (Task 0)

Commands and observed results:

```text
git rev-parse --is-inside-work-tree        -> true
git remote -v                              -> origin https://github.com/shijianus/EpoCanva.git
git branch --show-current                  -> feat/offline-multilingual-backend
git status --porcelain=v1                  -> (empty at start)
git rev-parse HEAD                         -> 0798bcb3fda8aade3ccb56758d187d90a306c0eb
git rev-parse origin/feat/offline-multilingual-backend
                                           -> 0798bcb3fda8aade3ccb56758d187d90a306c0eb
git log --oneline 0798bcb..origin/...      -> (empty)
git diff --stat 0798bcb..origin/...        -> (empty)
git merge-base --is-ancestor 0798bcb origin/...
                                           -> exit 0 (0798bcb IS an ancestor)
```

- Local HEAD == remote HEAD == `0798bcb`; there are no later remote commits.
- The working tree was clean before any change. No user work was overwritten,
  reset, stashed or cleaned.
- No destructive git operation was run. No commit/tag/push/PR was created.

---

## 2. Actual difference from `0798bcb`

There were **no source differences** between the audited baseline and the
remote. All changes below are this session's uncommitted work:

**Single manifest (E1.1)**
- `engine/locale/backend_abi_values.h` (new): the only numeric copy of the
  backend ABI constants inside the locale layer.
- `engine/locale/backend_manifest.{h,cc}` (new): validation, flattened rows and
  deterministic JSON export from `BuiltinProfiles()`.
- `engine/locale/locale_profile.{h,cc}`: constants moved to
  `backend_abi_values.h`; registration now rejects a binding for an absent
  scheme, a binding naming a scheme the profile does not declare, a zero id and
  a duplicate id across the registry.
- `engine/locale/CMakeLists.txt`: new sources.
- `data/locale/backend-bindings.json` (new): generated export, committed as build
  input. It is machine-generated, never hand-edited.
- `tests/locale_registry/main.cc`, `CMakeLists.txt`, `assert_manifest.cmake`:
  full-field 13-row oracle, ABI equality, negative mutations, deterministic
  export and committed-manifest freshness gate.
- `tools/backend/stage_resources.py`: `profile_rows()` no longer contains the
  13-row table; it reads the exported manifest and resolves each symbolic ABI
  name against `epocanva_backend.h`.
- `tests/backend_resource_staging/test_stage_resources.py`: manifest-only source,
  drift/duplicate/absent/unexpected-field/wrong-digest negatives.

**Backend persistence diagnosis (privacy-safe, text-free)**
- `engine/data_store/windows_learning_repository.{h,cc}`: every failing
  filesystem step now records `IoStage` + Win32 code in a thread-local
  `IoFailure` (`LastIoFailure()`); stages cover open/reopen, read, write, flush,
  replace, delete, lock, security, read-only and exception. No path, text or ID
  byte is stored or printed.
- `tests/learning_repository/main.cc`: stage assertions for success/no-diagnostic,
  read-only, replace-blocked (no-delete handle → sharing/lock violation),
  lock contention, weakened DACL, and diagnostic clearing on retry.
- `tests/backend/profiles.cc`: text-free `DIAG ack`/`DIAG learning-load`/`DIAG
  reopen` diagnostics only. No assertion was removed, relaxed or made
  unconditional.

**TSF timing**
- `tests/windows/tsf_product/product_sink_tests.cc`: one readiness deadline
  shared by the worker and the main event wait; bounded idle/release/unload
  budgets; diagnostics printed after `join()` only.

**E2 candidate contract**
- `engine/candidates/{CMakeLists.txt,candidate_contract.h,candidate_contract.cc}`
  (new): internal contract, stable ID, normalization, validation.
- `engine/backend_session/session.h`: `flow::Candidate` carries the contract.
- `engine/backend/backend_api.cc`: fills and validates the contract before
  publication; language/scheme helpers.
- `tests/candidate_contract/{CMakeLists.txt,main.cc}` (new): 3 CTests.
- `docs/architecture/candidate-contract.md` (new): identity rules and the
  proposed additive C ABI extension (design only).

**Documentation**
- `docs/adr/0005-multilingual-language-profiles.md`
- `docs/product/requirements.md`
- `docs/testing/backend-resource-staging.md`
- `docs/ci/windows-ci.md` (new): minimal Windows CI design (design only, no
  workflow file).
- `docs/testing/clean-worktree-verification.md` (new): post-commit verification
  plan (plan only).
- `docs/testing/candidate-evaluation-baseline.md` (new): E6 measurement design
  (design only, no evaluator implemented).
- this report.

### 2.1 Complete file list

Modified (21):
`CMakeLists.txt`,
`docs/adr/0005-multilingual-language-profiles.md`,
`docs/product/requirements.md`,
`docs/reports/phase-e-audit-implementation-verification.md`,
`docs/testing/backend-resource-staging.md`,
`engine/backend/CMakeLists.txt`,
`engine/backend/backend_api.cc`,
`engine/backend_session/CMakeLists.txt`,
`engine/backend_session/session.h`,
`engine/data_store/windows_learning_repository.cc`,
`engine/data_store/windows_learning_repository.h`,
`engine/locale/CMakeLists.txt`,
`engine/locale/locale_profile.cc`,
`engine/locale/locale_profile.h`,
`tests/backend/profiles.cc`,
`tests/backend_resource_staging/test_stage_resources.py`,
`tests/learning_repository/main.cc`,
`tests/locale_registry/CMakeLists.txt`,
`tests/locale_registry/main.cc`,
`tests/windows/tsf_product/product_sink_tests.cc`,
`tools/backend/stage_resources.py`.

Added (15):
`data/locale/backend-bindings.json`,
`docs/architecture/candidate-contract.md`,
`docs/ci/windows-ci.md`,
`docs/reports/phase-e-followup-implementation.md`,
`docs/testing/candidate-evaluation-baseline.md`,
`docs/testing/clean-worktree-verification.md`,
`engine/candidates/CMakeLists.txt`,
`engine/candidates/candidate_contract.cc`,
`engine/candidates/candidate_contract.h`,
`engine/locale/backend_abi_values.h`,
`engine/locale/backend_manifest.cc`,
`engine/locale/backend_manifest.h`,
`tests/candidate_contract/CMakeLists.txt`,
`tests/candidate_contract/main.cc`,
`tests/locale_registry/assert_manifest.cmake`.

No file under `third_party/` was touched. No file was deleted. No build
artefact, absolute path, machine-specific path, user text or candidate surface
appears in any changed file (`git diff --check` clean; grep for machine paths
empty; new diagnostics print status names, aggregate counts, stages and Win32
codes only).

### 2.2 Baseline, uncommitted work and planned commit groups

Three distinct layers must not be conflated:

1. **Baseline SHA `0798bcb3fda8aade3ccb56758d187d90a306c0eb`** — audited,
   equals remote HEAD. Contains none of this work.
2. **Current uncommitted work** — the 36 files in §2.1, present only in the
   working tree at report time.
3. **Planned commit groups**, now **committed and verified** (§13): the
   grouping below was authored from source review; after the commits existed,
   each commit was re-verified in a detached worktree built from that commit
   alone, and D was verified with the full three-suite clean-worktree run.

   ```text
   SOURCE_VERIFIED: each group is independently organizable from the source
     and its dependency review — no group references a file that only a later
     group would add.
   COMMIT_LEVEL_INTEGRATION: verified after the corresponding commit SHA
     existed (detached-worktree configure/build/test per commit, and the
     final clean-worktree full run on D) — see §13.2 and §13.3.
   ```

   File counts: **A=15, B=1, C=4, D=11, E=5; total 36 = 21 modified + 15 added.**

| Group | Message | Files | Group-level verification plan |
| --- | --- | --- | --- |
| A | `feat(locale): generate backend resource manifest from locale profiles` | `engine/locale/backend_abi_values.h`*, `engine/locale/backend_manifest.{h,cc}`*, `engine/locale/locale_profile.{h,cc}`, `engine/locale/CMakeLists.txt`, `data/locale/backend-bindings.json`*, `tests/locale_registry/{main.cc,CMakeLists.txt}`, `tests/locale_registry/assert_manifest.cmake`*, `tools/backend/stage_resources.py`, `tests/backend_resource_staging/test_stage_resources.py`, `docs/testing/backend-resource-staging.md`, `docs/adr/0005-multilingual-language-profiles.md`, `docs/product/requirements.md` | `locale_registry.*` (5), Python staging (40), real staging hash |
| B | `test(tsf): share a bounded engine readiness deadline` | `tests/windows/tsf_product/product_sink_tests.cc` | `sink_settings_timing` ×10, `sink_lease_lock` ×5, product suite |
| C | `feat(learning): add privacy-safe persistence failure diagnostics` | `engine/data_store/windows_learning_repository.{h,cc}`, `tests/learning_repository/main.cc`, `tests/backend/profiles.cc` | `learning.repository.*` (13), backend profiles suite |
| D | `feat(candidates): add validated internal candidate contract` | `engine/candidates/{CMakeLists.txt,candidate_contract.h,candidate_contract.cc}`*, `engine/backend_session/{CMakeLists.txt,session.h}`, `engine/backend/{CMakeLists.txt,backend_api.cc}`, `CMakeLists.txt` (root), `tests/candidate_contract/{CMakeLists.txt,main.cc}`*, `docs/architecture/candidate-contract.md`* | `candidate_contract.*` (3), backend suite 58/58 |
| E | `docs(phase-e): record foundation verification and remaining gates` | `docs/reports/phase-e-followup-implementation.md`*, `docs/reports/phase-e-audit-implementation-verification.md`, `docs/ci/windows-ci.md`*, `docs/testing/clean-worktree-verification.md`*, `docs/testing/candidate-evaluation-baseline.md`* | documentation only |

\* new file. No single file carries changes for two groups, so no `git add -p`
hunk splitting is required; the grouping is whole-file.

Dependency notes (verified against the diffs):
- A is self-contained: `backend_manifest.cc` reads `locale_profile.h`; the
  staging tool reads the committed JSON, so Python tests pass without a native
  build.
- C is self-contained: `IoStage`/`IoFailure` live in the repository header its
  tests include.
- D is self-contained: `backend_session` only *declares* a
  `candidates::Record` member through the header include path; it needs no
  `epocanva_candidates` symbol. `engine/backend/CMakeLists.txt` and the root
  `CMakeLists.txt` add the target behind `if(NOT TARGET epocanva_candidates)`
  guards, so A+D or D alone both configure.
- A, B, C, D have no interdependency; E references all four. Commit order:
  A → B → C → D → E.

Scope notes required by the commit titles:
- B changes test code only; the title is `test(tsf)`, not a product-runtime fix.
- C changes product source (it adds diagnostics, it does not fix any
  persistence failure), so the title is `feat(learning)`, not `test(learning)`.

---

## 3. Completion status

Labels used in this report:

- **SOURCE_VERIFIED** — asserted against the actual source/header on disk with a
  file:line citation; not a runtime claim.
- **UNIT_TESTED** — covered by a falsifiable unit/negative test that ran green.
- **INTEGRATION_TESTED** — exercised end-to-end through a real interface
  (CTest suite, real resources, or the controlled TSF host).
- **HOST_VERIFIED** — verified in a real registered application host. Reserved;
  **nothing in this report is HOST_VERIFIED** (the TSF work used the controlled
  test host, not a registered real host).
- **FAILED** — executed and did not pass.
- **BLOCKED** — could not be established; no fix claimed.
- **NOT_IMPLEMENTED** — design may exist, code does not.

| Item | Status | Evidence |
| --- | --- | --- |
| Task 0 remote check | SOURCE_VERIFIED | §1 |
| Root baseline before changes | INTEGRATION_TESTED | 78 tests, 77 pass, 0 fail, 1 skip, exit 0 |
| Backend baseline before changes | INTEGRATION_TESTED | 58 tests, 58 pass, 0 fail, exit 0 |
| Product baseline before changes | INTEGRATION_TESTED | 235 tests, 233 pass, 0 fail, 2 skip, exit 0 |
| E1.1 single manifest | UNIT_TESTED + INTEGRATION_TESTED + COMMIT_LEVEL_INTEGRATION | §4, §7, §13.2 (A), §13.3 |
| Manifest deterministic/negative gates | UNIT_TESTED | 5 locale CTests + Python negatives |
| Staging still works on real resources | INTEGRATION_TESTED | 1/1 real staging; `resources.tsv` hash unchanged |
| `backend.profiles.real_acceptance` failure | **BLOCKED — not reproduced** | §6 |
| TSF `sink_settings_timing` | INTEGRATION_TESTED (controlled TSF host) + COMMIT_LEVEL_INTEGRATION | 20/20 runs, §5; §13.2 (B) |
| TSF `sink_lease_lock` (same worker pattern) | INTEGRATION_TESTED | 5/5 before + 5/5 after initial-budget change; one 60 s initial-readiness failure documented in §5 |
| E2 internal candidate contract | UNIT_TESTED + INTEGRATION_TESTED + COMMIT_LEVEL_INTEGRATION | 3 CTests; backend 58/58; §13.2–13.3 |
| E2 public C ABI extension | NOT_IMPLEMENTED (design in doc) | `docs/architecture/candidate-contract.md` §3 |
| E3 real candidate sources | INTEGRATION_TESTED for existing 5 languages | existing backend suites; no fake arrays |
| E4 cross-language fusion | NOT_IMPLEMENTED | §8 |
| E5 persistent detection latch | NOT_IMPLEMENTED | §8; ranker is per-query |
| E6 offline evaluation extension | NOT_IMPLEMENTED — design recorded | `docs/testing/candidate-evaluation-baseline.md`; §8 |
| E7 mixed-language regression | NOT_IMPLEMENTED | §8 |
| E8 improved non-neural next-word | NOT_IMPLEMENTED (n-gram baseline exists) | §8 |
| Io-stage persistence diagnostics | UNIT_TESTED + INTEGRATION_TESTED + COMMIT_LEVEL_INTEGRATION | `learning.repository.errors` stage assertions; §6.6; §13.2 (C) |
| Repository io-error root cause | **BLOCKED — not reproduced, not diagnosed** | §6, §6.6 |
| CI | NOT_IMPLEMENTED (no `.github/workflows`); design recorded | `docs/ci/windows-ci.md`; §9 |
| Clean-worktree re-verification | INTEGRATION_TESTED | §13.3 (D_SHA full serial run) |
| Android | NOT_IMPLEMENTED | unchanged |

---

## 4. E1.1 — one authored catalogue, one deterministic export

### 4.1 Design chosen

`BuiltinProfiles()` in `engine/locale/locale_profile.cc` remains the **only**
authored source for profiles, schemes and backend bindings.
`BackendManifestRows()` flattens it and `BackendManifestJson()` serializes it.
The export is committed as `data/locale/backend-bindings.json` so the Python
staging tool needs no native build, and `tests/locale_registry.manifest`
regenerates it from the compiled C++ on every test run and byte-compares it.

C++ and Python now consume one definition:

```text
engine/locale/locale_profile.cc  (authored)
        │
        ├── BackendManifestRows()/Json()  ──> data/locale/backend-bindings.json
        │                                              │
        │                                              └── tools/backend/stage_resources.py
        │
        └── epocanva_locale (runtime)
```

Rejected alternatives:
- **Hand-maintained JSON as the top source, generating C++**: needs a JSON parser
  or a configure-time generator in the root build, and moves profile identity out
  of the compiled product; larger blast radius for no E1.1 gain.
- **Keep both tables and add a comparison test**: would still be two authored
  definitions; the brief explicitly forbids a third table and asks for one source.
- **Python invoking a C++ tool at staging time**: `stage_resources.py` is
  standard-library-only and explicitly rejects subprocesses; a committed export
  plus a freshness gate preserves that property.

### 4.2 Stable IDs and resource format

- The 13 numeric IDs 1–13 are unchanged and pinned by an exact-row oracle.
- `resources.tsv` keeps its 14-column format, order and byte content. A real
  staging run produced **2983 bytes**, SHA-256
  `f6acd2eaf5c0a660404944d5ddcaaad1477f369035247b057e24c2fb543e9ffd`, exactly the
  value recorded in `docs/testing/backend-resource-staging.md`. The only new
  inventory input is `data/locale/backend-bindings.json` (145 inputs total).
- Profile 6 (`zh-zhuyin`) is exported as `"scheme_implementation":
  "unverified"` and the locale registry still returns `kSchemeNotVerified` for
  `zh-Hans-CN/zhuyin`. A binding or staged resource never promotes it to
  `kImplemented`.
- A serviceable resource is not evidence: `kImplemented` still requires a
  relative local `evidence_ref`; staging state is not consulted.

### 4.3 ABI drift control

The locale layer cannot include `epocanva_backend.h` (dependency direction), so
the numeric values live once in `engine/locale/backend_abi_values.h`.
`tests/locale_registry` includes both that header and the public ABI header and
compares every copied constant, every manifest enum name/value pair, and the
exact 13-row field set. Changing either side fails the test.

### 4.4 Manifest acceptance evidence

- 13 rows, full-field equality (id, owning profile, language tag, four enum
  names and values, variant, scheme, implementation state).
- IDs unique, nonzero, exact set `1..13`.
- Every scheme belongs to its owning profile; a binding for an absent scheme is
  rejected at registration and by the manifest validator.
- Same language with different keyboard layouts (1/11/12/13) and different
  output/input modes (4/7/8/9, 5/10) stays distinct; a duplicate configuration
  tuple is rejected.
- Negative mutations that must fail: empty rows, zero id, duplicate id, unknown
  profile, scheme not in profile, binding for `double_pinyin` (absent), enum
  name/value drift, missing enum name, language-tag drift, duplicate
  config tuple, relabelling zhuyin as implemented, unknown output mode.
- Determinism: two exports are byte-identical; the CTest re-exports, compares to
  the committed file, and separately proves a one-byte mutation would be
  detected (`locale_registry.manifest_stale_can_fail`). A separate one-off run
  supplied a copy with `language_value` changed from 1 to 2 as the "committed"
  file and the gate exited 1 with the stale-manifest message.
- Missing binding: the exact 13-row oracle plus the committed-file comparison
  fail if a row disappears; the Python manifest test also asserts the exact id
  set.
- Wrong resource hash: staging tests assert a missing resource and a malformed
  digest are rejected; provenance tests already cover an independently pinned
  digest mismatch.

---

## 5. TSF `sink_settings_timing` — root cause and fix

### 5.1 Root cause

The test had two independent timeouts for one readiness phase:
- the main thread waited `25000 ms` on `ready_event`;
- the worker's `ReadyToType(..., 60000)` could legitimately run longer after a
  slow activation/deployment, and the event is signalled only after that call.

Under 15–25 s OneDrive/Rime deployment, the main wait expired while the worker was
still inside its own budget. The previous audit correctly identified this
mismatch; the fix is not a single constant bump.

### 5.2 Fix

`tests/windows/tsf_product/product_sink_tests.cc` now computes one deadline
before starting the worker and passes it to `HoldSessionWorker`, which gives
`ReadyToType` only the remaining budget. The main wait is
`deadline + 10 s signalling margin`, so it cannot expire while the worker is
still inside the same bounded phase. Constants:
`kEngineReadinessBudgetMs = 90000`, `kReadinessSignalMarginMs = 10000`,
`kEngineIdleBudgetMs = 30000`, `kSessionReleaseBudgetMs = 30000`,
`kModuleReleaseBudgetMs = 30000`.

Exception paths:
- the release event is always set before `join()`;
- worker-owned counters/error are read only after `join()` (no data race);
- on failure the test prints elapsed ms, whether the event was signalled, the
  text-free seam counters and the worker's error;
- idle polling and module unload have their own bounded budgets.

The same `HoldSessionWorker` pattern in `sink_lease_lock` was fixed identically.

A later standalone `sink_lease_lock` run exposed one remaining unequal bound: the
case's **first** activation readiness still used `ReadyToType(..., 60000)` and
timed out once after 62.7 s under load (it passed 3/3 immediately afterwards).
The initial readiness checks in both `sink_settings_timing` and
`sink_lease_lock` now use `kEngineReadinessBudgetMs` (90 s), the same budget as
the worker/deadline path. This is an equalised bound, not a replacement for the
shared-deadline fix.

### 5.3 Verification

| Run | Result | Wall time |
| --- | --- | --- |
| `sink_settings_timing` first 10× (after fix, before E2) | 10/10 exit 0 | 9.59–12.12 s |
| `sink_settings_timing` second 10× (after E2 rebuild) | 10/10 exit 0 | 8.91–10.12 s |
| `sink_lease_lock` 5× (before initial-budget change) | 5/5 exit 0 | 6.14–6.67 s |
| `sink_lease_lock` after final rebuild, single run | 1 fail at first activation readiness (60 s) | 62.71 s |
| `sink_lease_lock` immediately re-run 3× | 3/3 exit 0 | — |
| `sink_lease_lock` 5× after initial-budget change | 5/5 exit 0 | — |
| `sink_settings_timing` 5× after initial-budget change | 5/5 exit 0 | — |
| Full Windows Product suite (before initial-budget change) | 235 total, 233 pass, 0 fail, 2 skip; exit 0; 828.75 s | timing 9.70 s, lease 6.27 s |
| Full Windows Product suite (final) | 235 total, 233 pass, 0 fail, 2 skip; exit 0; 857.87 s | timing 9.42 s, lease 6.52 s |

This is a controlled TSF host (`ITfKeyEventSink` called directly, no system TSF
registration). It is **not** `HOST_VERIFIED` in a real registered application;
that gate remains open.

---

## 6. `backend.profiles.real_acceptance` — what was actually established

### 6.1 Result: not reproduced

| Attempt | Result |
| --- | --- |
| Full backend CTest before changes | 58/58 pass, exit 0 |
| Profiles binary, 6 serial runs | 6/6 pass (`checks` 26036–26132) |
| Profiles binary, 32 runs (4 rounds × 8 concurrent) | 32/32 pass |
| Full backend CTest after E2 contract | 58/58 pass, exit 0 |
| `learning.repository.persistence`, alone 20× | 20/20 pass |
| `learning.repository.persistence`, 40 runs (5 × 8 concurrent) | 40/40 pass |
| `epocanva_learning_repository_tests.persistence` in a non-OneDrive `%TEMP%` root, 5× | 5/5 pass |
| Final root CTest run concurrently with the backend suite | 83 total, 81 pass, **1 fail** (`learning.repository.persistence`, `v1 migration got io-error`), 1 skip, exit 8 |
| Root CTest re-run alone after that failure | 83 total, 82 pass, 0 fail, 1 skip, exit 0 |

The historical assertion names (`second-confirmed-id-learns`,
`reopen-retains-exact-confirmed-id-counts`) never failed in these runs.

### 6.2 Evidence about the historical failure

- The preserved profile-test failure logs are under
  `build/audit-pinyin-profiles*.log` (from the separate
  `build/ime-audit-integration` tree). No preserved log contains the exact
  `second-confirmed-id-learns` assertion named by the Phase E audit.
- `audit-pinyin-profiles3.log` is dated **2026-11-05 16:36**, before commit
  `0ec6072` (2026-11-06 17:21), which rewrote 416 lines of
  `windows_learning_repository.cc` and 660 lines of `learning_store.cc` for
  schema v3.
- `build/audit-pinyin-profiles3.log` fails at
  `candidate-after-context-import-learns-own-id` (third ack), a different line
  from the Phase E report's second ack. This is consistent with a transient
  write failure rather than a deterministic test bug.
- Commit `0ec6072` itself records “backend 58/58” in its message, which
  contradicts the Phase E audit's 57/58. The audit may have run a stale
  `ime-audit-integration` binary rather than the rebuilt `backend-service` tree.

**Conclusion**: the failure is not present at the audited HEAD; the available
logs predate the v3 repository rewrite. It is recorded as
**BLOCKED — not reproduced**, not as fixed. No assertion was deleted, skipped or
relaxed.

### 6.3 Permanent diagnostics added

`tests/backend/profiles.cc` now prints text-free diagnostics on failure:
- `DIAG ack step=... consumed=... context_advanced=... persistence=<code>(<name>)`
- `DIAG learning-load profile=... status=... revision=... words=... pairs=...`
- `DIAG reopen revision=... words=... pairs=... good_count=... morning_count=...`

No candidate text, preedit, committed text, raw keystroke or field identity is
printed. If the failure recurs, the status code identifies the failing layer.

### 6.4 Remaining hypotheses (not accepted as root cause)

1. Transient sharing/lock conflict on the atomic `.dat`/`.v2` replacement under
   OneDrive/indexer/AV interference. `ERROR_SHARING_VIOLATION` and
   `ERROR_LOCK_VIOLATION` map to `kBusy`; other filesystem failures map to
   `kIoError`. A single `learning.repository.persistence` failure during the
   concurrent final root run reported exactly `io-error` on the v1 migration
   path (the `.v2` backup publication followed by the `.dat` replacement).
   It did not recur in 20 sequential or 40 concurrent isolated reruns, so the
   exact Win32 error is still unknown. `Acknowledge` consumes the commit before
   persistence and has no retry path. A retry would be safe only for `kBusy`
   (each `Record` reloads and re-applies one selection), but adding it without
   a reproduced status would be a speculative behaviour change. This is the
   strongest available evidence that intermittent external I/O interference is
   real in this workspace, and it is exactly why the new `DIAG ack` status is
   text-free but specific.
2. A pre-v3 defect removed by the v3 rewrite. The dated logs support this, but
   there is no failing run at current HEAD to prove which change removed it.
3. A stale binary/test invocation in the previous audit (also supported by the
   commit message).

The learning write test path remains explicitly test-side: the product keeps
`ProductLearningWritesPermitted() == false`, and no product learning write was
enabled.

### 6.5 Source-level state propagation (SOURCE_VERIFIED)

`Session::Acknowledge` (`engine/backend_session/session.cc:197-269`):
1. validates token/copied/applied and re-reads permission;
2. builds the selection (context target, generation, previous ID, lexeme IDs,
   learning key, bounded normalized identity);
3. **consumes the commit and advances the snapshot/context before persistence**
   (`:237-245`), so a transport retry cannot double-count;
4. calls the repository persist callback (`:254-257`), then, only on `kOk`,
   records into the in-memory `LearningStore` (`:266`).

`WindowsLearningRepository::Record`
(`engine/data_store/windows_learning_repository.cc:790-825`) is a complete
transaction: create a store, open the namespace lock, load the file, apply the
one selection, serialize, and atomically publish. A failure before `Save`
leaves the previous file intact. `LearningStore::Record`
(`engine/data_store/learning_store.cc:497-506`) can only return denied, stale,
invalid lexeme/format, revision exhaustion or size-limit; the selection built by
`Acknowledge` and the preceding permission checks make those unreachable for
the test inputs.

Consequences:
- `persistence_status != kOk` means the repository transaction failed
  (most plausibly `kBusy`/`kIoError` under external file contention), not the
  in-memory store;
- because the commit is already consumed, there is no retry inside
  `Acknowledge`; a caller must re-select and re-commit;
- if a future change made the in-memory `store->Record` fail after a successful
  disk write, `learned` would be false while disk advanced. That divergence is
  not reachable today but is a reason to keep the two steps in this order and
  covered by the repository tests.

### 6.6 Stabilization round — io-error risk analysis (no fix claimed)

The one observed `learning.repository.persistence` `io-error` (root CTest
concurrent with the backend suite) is still **BLOCKED, root cause unknown**.
This round audited the failure surface against the sources; nothing below is a
fix claim.

Fact-checked isolation properties (SOURCE_VERIFIED against the test sources):

1. **Shared temp directories**: no. Root `learning.repository.*` cases run
   under `<root-build>/tests/learning_repository/run/`
   (`tests/learning_repository/CMakeLists.txt`, `file(MAKE_DIRECTORY .../run)`);
   the backend suite's `IsolatedRoot`
   (`tests/backend/profiles.cc:129-141`) lives under its own build tree and
   requires the run root inside `<repo>/build/`. Leaf directories carry
   `PID` + `GetTickCount64()` (+ ordinal), created with an exclusive
   `create_directory` race check. Two suites cannot collide on one leaf path.
2. **Fixed filenames**: per-namespace catalog stems (`.dat`, `.tmp`, `.v2`,
   `.v2.tmp`, `.lock`) are fixed within one root, but each transaction holds
   the `.lock` (`LockFileEx`, fail-immediately → `kBusy`) and stale `.tmp`
   files are removed by handle-based cleanup. A killed process leaves no
   in-flight name that survives the next successful transaction.
3. **Cross-suite rename/cleanup collision**: ruled out — the suites share no
   leaf path. The remaining shared resource is the OneDrive-synced repository
   scope itself (`build/` lives under it): OneDrive/indexer/AV can hold handles
   on any file asynchronously. `ERROR_SHARING_VIOLATION`/`ERROR_LOCK_VIOLATION`
   from such interference map to `kBusy` (`FileError`), so a *sharing* conflict
   alone cannot produce the observed `io-error`. A non-sharing filter error
   (cloud-filter, access-denied variant, unexpected network error) would map to
   `kIoError` — plausible, unproven.
4. **Unique names**: yes — PID + tick (+ ordinal), as above.
5. **Per-step error codes**: now yes. Every failing Win32 step records
   `(IoStage, win32_error)` (`FailLast`/`Fail`); exceptions record `kException`;
   gate/security rejections record `kSecurity` (no Win32 code — they are policy
   results, not filesystem errors). At the time of the observed failure these
   diagnostics did not exist, which is exactly why the failure is unattributed.
6. **Stage distinguishability**: yes — open (and reopen), read, write, flush,
   replace, delete, lock, security, read-only, exception are separate stages
   (`IoStageName`), asserted per scenario in `learning.repository.errors`.

Minimal fault-injection coverage (existing + designed; **design only** — the
italic rows are not yet implemented):

| Fault | Mechanism | Status |
| --- | --- | --- |
| sharing violation at replace | real second handle without `FILE_SHARE_DELETE` on `.dat`; asserts `kBusy`, stage `replace`, Win32 code preserved, old bytes intact, success after close | **INTEGRATION_TESTED** (`learning.repository.errors`) |
| read-only target | `FILE_ATTRIBUTE_READONLY`; asserts stage `read-only` | **INTEGRATION_TESTED** |
| lock contention | real `LockFileEx` holder; asserts stage `lock`, bounded wait | **INTEGRATION_TESTED** |
| weakened DACL | real ACL edit; asserts stage `security` | **INTEGRATION_TESTED** |
| lock contention on load | same lock path; asserts stage `lock` on `LoadSnapshot` | **INTEGRATION_TESTED** |
| success clears diagnostic | after `kOk`, `LastIoFailure().stage == kNone` | **INTEGRATION_TESTED** |
| *temp-open failure* | pre-create a foreign `.tmp`/`.v2.tmp` → `CREATE_NEW` fails `ERROR_FILE_EXISTS` → `kIoError`, stage `open`, `.dat` unchanged | designed, not implemented |
| *open sharing violation* | exclusive external handle on `.dat` → `kBusy`, stage `open` | designed, not implemented |
| *v1 corruption* | `broken\n` → `kInvalidFormat` + quarantine | **INTEGRATION_TESTED** (corruption case) |
| *v3 catalog mismatch* | valid v3 bytes with a foreign `catalog-sha256` → `kInvalidFormat` + quarantine | designed, not implemented |
| *disk full / flush failure* | not deterministically injectable on NTFS without admin/volume tools; `ERROR_DISK_FULL` maps to `kIoError` stage `write`/`flush` by code review | designed as code-review-only, declared limitation |
| *kill mid-transaction* | child process killed after the start event while `.tmp` exists; parent expects lock acquirable, old bytes or valid v3, no orphan `.tmp` after next success | designed, not implemented |
| *no double-count* | after a replace-stage failure, a retried `Record` must land exactly one revision (selection is absolute, and `Acknowledge` consumes the commit before persistence) | designed, not implemented |

No generic retry is added. A retry is defensible only for `kBusy` and only
outside the repository; without a reproduced error there is no evidence for
one. The next occurrence of a persistence `io-error` must be reported with
`LastIoFailure()` (`IoStageName` + Win32 code) — the diagnostics now make that
possible.

---

## 7. Commands, exit codes and test counts

All commands were run from the repository root. CMake/CTest are the VS2022
bundled binaries (`Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin`).

| # | Command | Exit | Result |
| ---: | --- | ---: | --- |
| 1 | `cmake --preset vs2022-x64-debug` | 0 | configured |
| 2 | `cmake --build build/vs2022-x64-debug --config Debug --parallel 8` | 0 | built |
| 3 | `ctest --test-dir build/vs2022-x64-debug -C Debug --no-tests=error --output-on-failure` (baseline) | 0 | 78 total, 77 pass, 0 fail, 1 skip |
| 4 | `cmake --build build/backend-service --config Release --parallel 8` | 0 | built |
| 5 | `ctest --test-dir build/backend-service -C Release --no-tests=error --output-on-failure` (baseline) | 0 | 58 total, 58 pass, 0 fail |
| 6 | `cmake --build build/windows-product --config Release --parallel 8` | 0 | built |
| 7 | `ctest --test-dir build/windows-product -C Release --no-tests=error --output-on-failure` (baseline) | 0 | 235 total, 233 pass, 0 fail, 2 skip |
| 8 | `ctest ... -R "^locale_registry\."` | 0 | 5/5 pass |
| 9 | `ctest ... -R "^candidate_contract\."` | 0 | 3/3 pass |
| 10a | root CTest final run concurrently with backend CTest | 8 | 83 total, 81 pass, 1 fail (`learning.repository.persistence`, io-error), 1 skip |
| 10b | root CTest re-run alone | 0 | 83 total, 82 pass, 0 fail, 1 skip, 33.35 s |
| 11 | `ctest --test-dir build/backend-service -C Release ...` (final) | 0 | 58 total, 58 pass, 0 fail |
| 12 | `ctest --test-dir build/windows-product -C Release ...` (final) | 0 | 235 total, 233 pass, 0 fail, 2 skip; 857.87 s |
| 13 | `python -m unittest tests.backend_resource_staging.test_stage_resources` | 0 | 40 tests, 38 pass, 2 opt-in skips |
| 14 | `EPOCANVA_STAGING_REAL_TESTS=1 python -m unittest ...RealResourceTests` | 0 | 1/1 pass, 49.57 s |
| 15 | `python -B tools/backend/stage_resources.py --product-root build/phase-e-fu-staging/product --oov error` | 0 | 13 profiles, 145 inputs; `resources.tsv` 2983 bytes `f6acd2…e9ffd` |
| 16 | `epocanva_backend_profiles <repo> <run-root> <rime-sha>` × 6 serial | 0 | 6/6 pass |
| 17 | same binary, 4 × 8 concurrent | 0 | 32/32 pass |
| 18 | `ctest ... -R "^product\.integration\.sink_settings_timing$"` × 10 | 0 | 10/10 |
| 19 | same × 10 after E2 rebuild | 0 | 10/10 |
| 20a | `ctest ... -R "^product\.integration\.sink_lease_lock$"` × 5 | 0 | 5/5 (before initial-budget change) |
| 20b | same, single run after final rebuild | 8 | 1 fail: first activation readiness timed out at 60 s |
| 20c | same, re-run immediately 3× | 0 | 3/3 |
| 20d | same, 5× after initial-budget change | 0 | 5/5 |
| 20e | `sink_settings_timing` 5× after initial-budget change | 0 | 5/5 |
| 21 | manifest gate with a one-value mutation supplied as the committed file | 1 | rejected as stale (expected failure) |
| 22 | `git diff --check` | 0 | no whitespace errors |

Skipped tests are explicit and environmental:
`learning.repository.symlinks` (symlink privilege) and
`windows.tsf_context_observer.real_focus` (environment-limited). They are not
counted as passes.

### 7.1 Stabilization-round re-verification (fresh, this working tree)

Run after the diff audit, on the same uncommitted tree:

| # | Command | Exit | Result |
| ---: | --- | ---: | --- |
| 23 | `git diff --check` | 0 | no whitespace errors |
| 24 | `locale_registry.*` + `candidate_contract.*` (8 tests, Debug) | 0 | 8/8 pass, incl. `locale_registry.manifest` and `manifest_stale_can_fail` |
| 25 | `python -B -m unittest tests.backend_resource_staging.test_stage_resources` | 0 | 40 tests, OK, 2 opt-in skips |
| 26 | `python -B tools/backend/stage_resources.py --product-root build/phase-e-stabilize-staging/product --oov error` | 0 | 13 profiles, 145 inputs |
| 27 | SHA-256 of the fresh `resources.tsv` | — | 2,983 bytes, `f6acd2eaf5c0a660404944d5ddcaaad1477f369035247b057e24c2fb543e9ffd` — byte-identical to the recorded baseline |
| 28 | grep for machine-specific paths across all changed/new files | 1 (no match) | clean |

The real staging TSV was regenerated from the actual resources in this run, not
cited from the earlier session's log: the manifest refactor provably did not
change the product payload. `backend.profiles`, root suite, and product suite
were **not** re-run in this stabilization round; the §7 numbers above remain
that round's evidence, and the clean-worktree plan is the required
serial re-run after commits exist.

The stale build artifacts under `build/` are not test evidence; only the logs
listed above were produced by this session.

---

## 8. E3–E8: evidence-based order and gates

### E3 — real language candidate sources (existing baseline)

| Language | Real source in code | Evidence | Status |
| --- | --- | --- | --- |
| English | `Model::Load` production corpus + `LexicalService` + case/morphology/spelling/segmentation + abbreviation decoder | `backend.profiles`, `backend.headless.*`, candidate contract | INTEGRATION_TESTED |
| Chinese | Rime pinyin schema + Terra dictionary + `PinyinInitialism` | `backend.profiles` pinyin/initialism cases, `backend.headless.chinese_real_engine` | INTEGRATION_TESTED |
| French | Production model + `LexicalService` + Hunspell `.aff/.dic` | `french.dictionary.*`, `backend.profiles` | INTEGRATION_TESTED |
| Japanese | Production model + romaji composer + Mozc `lexicon.bin` decoder | `backend.profiles` romaji/kana cases | INTEGRATION_TESTED |
| Korean | Production model + two-set composer; **no standalone Korean dictionary** | `backend.profiles` two-set cases | INTEGRATION_TESTED (bounded) |
| Spanish/German | No profile, engine or resource | locale resolves `kNoProfile` | NOT_IMPLEMENTED |

E3 is not "done" in the product sense: French is only built when
`EPOCANVA_BACKEND_WITH_FRENCH_DICTIONARY=ON`; Korean has no dictionary source;
all five languages still lack real-host acceptance.

### E4 — cross-language fusion (design)

Dependency: E2 stable ID + E3 sources + E6 calibration.

Design constraints:
- one session collects candidates from multiple producers against the same raw
  input; each candidate already carries language/scheme/stable ID;
- one raw/original candidate is injected first; it is never displaced and never
  rewritten;
- deduplicate by `stable_id` (language+scheme scoped), keeping one best source
  and recording which source won;
- never add uncalibrated raw scores across sources; use only calibrated weights
  or language-priority ordering when scores are incomparable;
- deterministic tie-break: language priority, source ordinal, stable ID bytes;
- every language still produces its own script; English behaviour is not
  downgraded;
- no automatic commit and no document mutation.

Acceptance: controlled en+zh, en+fr, en+ja, en+ko, en+es cases; one raw candidate
first; one occurrence per stable ID; identical results on repeated queries; a
missing source falls back without fabricating a candidate. Spanish stays
`NOT_IMPLEMENTED` unless a real engine/resource is added.

### E5 — explicit priority and detection latching (design)

Evidence: `AdaptiveRanker` has `latched_`, `effective_detected_language()` and
`ResetDetection()`, but `ApplyPersonalRanking()` constructs a local
`rk::AdaptiveRanker ranker;` inside a per-query helper
(`engine/backend/backend_api.cc:439`), so the latch is always empty.

Design: one ranker per session, owner-thread-only, stored in the session object;
reset on session open, context-target change, generation change, permission
revocation and deactivate. Detection is advisory: explicit user language always
wins; only N consecutive consistent observations may change the effective
detected language. Ranking order must use `locale::LanguagePriority()`; no second
hard-coded language order. Stale generation or a different target must not leak
prior detection state.

Acceptance: single/alternating observations do not change order; N consistent
observations do; explicit language wins near-equal scores; repeated queries are
deterministic; target/generation switch resets the latch.

### E6 — offline quality evaluation (existing baseline + gaps)

Existing: `tools/backend/evaluate_predictions.py` reports Hit@1/3/5, MRR@5,
coverage, bootstrap confidence intervals, latency mean/p50/p95 (and rejects
inconsistent inputs). `docs/testing/backend-prediction-quality.md` records
per-language baselines and the unigram comparison.

Gaps for E6:
- candidate stability across repeated queries is not part of the report;
- fallback correctness (raw first, missing model) is not a metric;
- memory and resource-load cost are not measured;
- the evaluator is not wired into the default test gate.

Design: add a deterministic replay harness that runs each request twice and
compares full ordered candidate IDs/text; record peak working set and model-load
time; add fallback cases; keep the existing corpus cases (no user-collected
data). Do not claim calibration before these numbers exist. The current ranking
weights (`personal=0.25`, `negative=0.25`, source/language `0`) remain explicitly
uncalibrated.

### E7 — mixed-language regression (design)

Depends on E4/E5. Cases: en+zh (pinyin/initialism + English prefix), en+fr
(accents/case/forms), en+es (mark missing engine honestly), en+ja
(romaji/kana/kanji), en+ko (two-set/native Hangul), zh+en+controlled
terminology. Each case needs a real source integration test, a repeated-query
determinism assertion and a raw-candidate-first assertion. No fixed candidate
arrays and no fabricated language support.

### E8 — non-neural next-word prediction (design)

Baseline exists: `Model::Predict` with n-gram/Jelinek-Mercer scoring and
`NEXT_WORD` filtering of terminal punctuation; `evaluate_predictions.py` already
measures next-word Hit@1/3/5 and MRR. Improvements to evaluate before adopting:
context/token features, bounded personal ranking for NEXT_WORD, and latency.
Acceptance: Top-1/Top-3 and p50 compared against the recorded unigram/model
baseline, a fallback test with no model/context, candidates never auto-commit,
and no neural model downloaded or required. Uncalibrated changes are not wired
into the product.

---

## 9. CI (evaluation and minimal design)

There is no `.github/workflows` directory, so the target branch has no continuous
integration. A minimal Windows pipeline would separate:
1. **unit/manifest**: VS2022 x64, CMake bundled, Python 3, root configure/build,
   `locale_registry.*`, `candidate_contract.*`, manifest freshness;
2. **backend**: configure `tools/backend` with `EPOCANVA_BACKEND_WITH_FRENCH_DICTIONARY=ON`,
   build Release, full backend CTest;
3. **resource integration**: cached pinned inputs, `stage_resources.py --check-only`
   plus the opt-in real staging test; fail on hash mismatch;
4. **product**: build `platforms/windows/engine_bridge`, run product tests that do
   not require system TSF registration; keep real-host/registration as a separate
   isolated acceptance job;
5. **negative/privacy**: keep the existing `*_can_fail` CTests and the Python
   staging negative suite.

No job may convert `Skipped` or an unexecuted step into a pass. Third-party
dependencies must remain pinned; the pipeline must not download models at runtime.

---

## 10. Performance and privacy regression

- E1.1 has no runtime path change: the manifest is generated in tests and read by
  the Python build tool; the product still uses compiled C++ data.
- E2 adds per-candidate normalization/validation before publication. It is
  O(candidates × text bytes), bounded by 64 candidates and 4096 UTF-8 bytes per
  surface; it performs no I/O and no allocation proportional to a corpus.
  No microbenchmark was run, so no latency claim is made. The end-to-end timing
  case stayed in its previous 9–12 s band, which is a coarse regression signal,
  not a performance measurement.
- No network I/O, telemetry, crash reporting or model download was added.
- The contract keeps candidate text in memory only; persistence still stores
  admitted lexeme IDs/counts, not display text.
- New diagnostics in `tests/backend/profiles.cc` print status names and aggregate
  counts only; no candidate/preedit/committed text, keystrokes or field identity.
- Product persistent learning remains closed (`ProductLearningWritesPermitted()`
  unchanged); the backend test path is explicitly not a product write path.
- The manifest contains locale metadata only; no user data.

---

## 11. Risks, rollback and entry condition

**Residual risks**
1. `backend.profiles.real_acceptance` is not reproduced but also not explained by
   a failing current build. If it recurs, the new diagnostics identify the
   persistence status; do not open product learning writes to "fix" it.
2. E2 is internal only. Any product/ABI consumer expecting the new fields must
   wait for the additive versioned API in
   `docs/architecture/candidate-contract.md`.
3. English-only ASCII case folding and no NFC/NFD equivalence are documented
   limitations; a future Unicode-normalization change must version the ID
   namespace and provide migration. The contract's `language_tag` is only the
   primary subtag today; binding a second Chinese script/region engine requires
   the full canonical profile tag in the ID namespace first.
4. Cross-language fusion, persistent detection, calibration and mixed regression
   are not implemented. Claiming them now would be false.
5. Product TSF real-host acceptance and Android remain open.

**Rollback**
- E1.1: restore the previous `stage_resources.py` table and delete
  `data/locale/backend-bindings.json`/`backend_manifest.*`; this restores the
  duplicate-source state and should not be used while the manifest tests exist.
- E2: remove the `contract` field/population and the `engine/candidates` target;
  the public ABI and learning schema do not change, so no data migration is
  involved.
- TSF: revert the deadline/constant changes in `product_sink_tests.cc`; no
  product binary behaviour changed.
- No persisted format changed in this session.

**Entry condition for the next implementation phase**: E1.1 and E2 internal
tests green, TSF timing stable across repeated runs, backend suite green, and an
explicit decision on the residual backend persistence risk. E4/E5 may start only
after E6 records a stable baseline; E3 completion still requires real resources
for Korean/French-in-default-build and no fabricated language support.

---

## 12. Conclusion

E1.1 is a real single-source change: the C++ registry is authored once, the
committed JSON is generated and byte-gated, and the Python staging tool no longer
holds a second 13-row catalogue. The byte-identical `resources.tsv` hash is the
strongest evidence that the refactor did not change the product payload.

The TSF timing failure had a structural cause and is fixed with a shared deadline
and bounded cleanup, not a constant bump; 20/20 repeated runs pass.

The backend persistence failure did **not** reproduce at the audited HEAD; the
preserved failures predate the schema-v3 repository rewrite. It is recorded as
BLOCKED with permanent text-free diagnostics rather than claimed as fixed. A
separate repository-level `learning.repository.persistence` failure appeared
once under concurrent full-suite I/O and then passed 20 sequential and 40
concurrent reruns; §6 records it as evidence of intermittent external I/O
interference, not as a diagnosed product defect.

E2 establishes stable candidate identity, source/match/commit semantics and a
learnable flag for all candidates the backend publishes, with the public v1 ABI
and schema-v3 learning keys untouched. E4–E8 remain designs with explicit gates.

---

## 13. Foundation stabilization round — commits, per-commit and clean-worktree verification (2026-11-07)

This section records the round that grouped the §2.1 files into commits and
verified them in detached worktrees. It changes no §1–§12 claim.

### 13.1 Commits (actual)

Baseline SHA: `0798bcb3fda8aade3ccb56758d187d90a306c0eb` (also the remote head
before this round; re-checked with `git fetch --prune` immediately before the
documentation commit — no third-party commits appeared).

| Group | Commit | Title | Files |
| --- | --- | --- | --- |
| A | `33a697fccded7d652dbdff6767853967e99c5a70` | `feat(locale): generate backend resource manifest from locale profiles` | 15 |
| B | `61c403630cca9e7b322e5e592699c0c7444223ff` | `test(tsf): share a bounded engine readiness deadline` | 1 |
| C | `27b23efcc4dffff538e479c3e38212eb606173b6` | `feat(learning): add privacy-safe persistence failure diagnostics` | 4 |
| D | `cb546ee2122f8929546fe87dffb7a5917fefa64d` | `feat(candidates): add validated internal candidate contract` | 11 |
| F | `f8ecb6940bd5dfac4595226a3f9aa900d3f00d3f` | `fix(product): pin package payload hashes to the canonical LF checkout bytes` | 1 |
| E | this documentation commit | `docs(phase-e): record foundation verification and remaining gates` | 5 |

A+B+C+D = 15+1+4+11 = 31 of the 36 files from §2.1 (21 modified + 15 added);
E carries the remaining 5. F is a one-file product-policy fix **discovered by
this round's clean-worktree verification** (§13.4); it is not part of the
original 36-file set and is committed separately so the five group commits stay
exactly as planned. `git diff --cached --name-status` was checked before every
commit; each staged set contained exactly its group's files; `git diff --check`
clean at every step.

```text
Baseline SHA: 0798bcb3fda8aade3ccb56758d187d90a306c0eb
Manifest commit: 33a697fccded7d652dbdff6767853967e99c5a70
TSF test timing commit: 61c403630cca9e7b322e5e592699c0c7444223ff
Persistence diagnostics commit: 27b23efcc4dffff538e479c3e38212eb606173b6
Candidate contract commit: cb546ee2122f8929546fe87dffb7a5917fefa64d
Payload-policy fix commit (found by this round): f8ecb6940bd5dfac4595226a3f9aa900d3f00d3f
Tested code SHA: f8ecb6940bd5dfac4595226a3f9aa900d3f00d3f (see §13.4 for why
  the tested tree is exactly this commit)
Documentation commit: intentionally recorded in Git history / PR, not
  self-referenced
```

Grouping status:

```text
SOURCE_VERIFIED: each group is independently organizable from the source
  and its dependency review — no group references a file that only a later
  group would add.
COMMIT_LEVEL_INTEGRATION: verified after the corresponding commit SHA
  existed (detached-worktree configure/build/test per commit, §13.2) plus
  the final clean-worktree full serial run (§13.3).
```

### 13.2 Per-commit verification (detached worktrees, local NTFS volume outside the synced folder)

Each worktree was `git worktree add --detach` at the named commit, with the
pinned external inputs copied in; the build/test gates themselves verify the
inputs (rime DLL SHA-256 against `engine-build-receipt.json`, librime checkout
pin `33e781440250125871856cdc5b42ddc6a5fcd3cd4`, staging provenance anchors,
package payload pins). Worktrees were removed after their round.

- **A_SHA**: root preset configure + `epocanva_locale_registry_tests` build
  exit 0; `locale_registry.*` 5/5 (includes `locale_registry.manifest`
  determinism + committed-file comparison and `manifest_stale_can_fail`); the
  worktree stayed clean after the manifest test. Python staging unit tests:
  40 tests OK (2 opt-in skips). Real staging: 13 profiles, 145 inputs;
  `resources.tsv` 2,983 bytes, SHA-256
  `f6acd2eaf5c0a660404944d5ddcaaad1477f369035247b057e24c2fb543e9ffd` —
  byte-identical to the recorded baseline; post-staging `git status` empty.
- **B_SHA**: product configure verified the Rime source pin and the DLL
  SHA-256 (`0e202bb871bd7d7b272feb461ef7db0f501187a51ca4f70dc8b8ce8967eecd2f`)
  against the build receipt; build exit 0.
  `product.integration.sink_settings_timing` ×10: 10/10 exit 0.
  `product.integration.sink_lease_lock` ×5: 5/5 exit 0. The full product
  suite was deliberately left to the final clean-worktree run (§13.3).
- **C_SHA**: root configure + `epocanva_learning_repository_tests` exit 0;
  `learning.repository.*` 13 registered, 12 pass, 1 skip
  (`learning.repository.symlinks`, symlink privilege), exit 0 — the
  `IoStage`/`LastIoFailure` stage assertions and negative controls pass.
  Backend configure/build exit 0 (the first configure failed until the
  pinned `third_party/librime` headers were provisioned — an input
  provisioning step, recorded here for reproducibility);
  `backend.profiles.*` 3/3 including `backend.profiles.real_acceptance` and
  the `real_can_fail` falsifiability control. No persistence `io-error`
  occurred; the historical failure remains **BLOCKED — NOT REPRODUCED,
  ROOT_CAUSE_UNKNOWN**.
- **D_SHA**: verified by the full clean-worktree run below rather than a
  separate partial round.

### 13.3 Clean-worktree full verification (tested tree = F content)

A fresh detached worktree at D_SHA with new build directories; the pinned
inputs (models, schemas, Hunspell, Mozc lexicon, ESDB checkout, rime DLLs and
receipts, `third_party/librime` checkout) were copied from the original
workspace tree — no separate immutable input cache exists yet, which is
exactly the dependency `docs/ci/windows-ci.md` designs away. The three full
suites ran **serially, in this order**, each as one complete CTest invocation:

1. **Root (Debug)**: configure exit 0, build exit 0 (~47 s), ctest exit 0 —
   **81 registered, 81 executed, 100% passed, 0 failed, 1 skip**
   (`learning.repository.symlinks`; symlink privilege). Manifest freshness
   (`locale_registry.manifest`, `manifest_stale_can_fail`) passed inside this
   run. The clean worktree registers **81, not 83**:
   `english.core.language_support_pilot_budget` and
   `english.core.spelling_cost_trial_on_pilot_lexicon` register only when the
   generated P2.4 pilot word list exists under `build/english-pilot/` — a
   documented registration rule (README §快速开始 1), i.e. an absent external
   generated input, not a silently skipped registered test.
2. **Backend (Release)**: configure/build exit 0 (~113 s). First full ctest:
   57/58 — `backend.headless.real_five_language_api` failed with
   `backend_open_status=-102` (`EPOCANVA_BACKEND_INVALID_RESOURCE`) because
   its default model directory `build/backend-training/` had not been
   provisioned into the worktree (provisioning omission, not a code failure;
   `budget_five_language_api`, which reads the provisioned
   `build/backend-training-budget/`, passed in the same run). After
   provisioning that pinned input, the **complete suite** re-ran end to end:
   **58 total, 58 pass, 0 fail, exit 0, 60.9 s**.
3. **Staging + product**: Python staging unit tests 40 OK (2 opt-in skips);
   `RealResourceTests` 1/1 (35.2 s); real staging 13 profiles / 145 inputs,
   `resources.tsv` 2,983 bytes, SHA-256
   `f6acd2eaf5c0a660404944d5ddcaaad1477f369035247b057e24c2fb543e9ffd`
   (byte-identical to the recorded baseline). Product configure (Rime pin +
   DLL receipt verified) and build exit 0 (~220 s).
   Product ctest — see §13.4 for the two defect-finding runs and the fix;
   **final result: 233 registered, 233 executed, 100% passed, 0 failed,
   2 skips** (`windows.tsf_context_observer.real_focus`,
   `learning.repository.symlinks`), exit 0, ~806 s. The clean worktree
   registers 233, not 235, for the same two pilot-lexicon registration rules
   as the root suite.

Post-run: `git diff --check` clean; the only working-tree difference versus
commit F was the F fix itself (`git diff f8ecb69 --stat` empty), so the tested
tree is exactly the `f8ecb69…` commit tree. All 15 key new files verified with
`git ls-files --error-unmatch` (11 code files tracked by A/D; the 4
documentation files are tracked by this commit E).

### 13.4 Defect found by the clean-worktree run, and its fix (commit F)

Runs 1 and 2 of the product suite failed `product.policy.release_gate_normal`
and `product.unit.settings_manifest_paths` (233 executed: 231 pass / 2 fail /
2 skip, then 232 pass / 1 fail / 2 skip after input provisioning). Root cause,
verified against sources and bytes:

- `data/manifests/product-package-files.inc` pinned
  `data/product/default.yaml` and `data/product/epocanva_english.schema.yaml`
  at **675 / 2071 bytes — the maintainer checkout's stale CRLF working-tree
  bytes**. The stored blobs are already LF, and `.gitattributes` mandates
  `eol=lf` for `*.yaml`, so every compliant fresh checkout (and any CI) writes
  **659 / 2013 bytes** and the gate rejects the payload with "required payload
  identity is missing or changed".
- Not a transient error: reproduced identically across three runs once the
  files were present and stable, and the maintainer tree passed only because
  its working copy still held the stale CRLF bytes (content-identical to the
  index under the clean filter, which is why `git status` looked clean).
- Fix (commit F, one file): move the two pinned digests/byte counts to the
  canonical LF bytes
  (`7a75610aa595dd6dcc75177ee4352d38f139a4ed20e2980888619c87f31aaeec` / 659 and
  `657e4933ad3dee9ce26ff4fcaccf77475c8c1c33b9de72e354f8558d6b754372` / 2013)
  and renormalize the maintainer working copies to those bytes (no blob
  changes; `git diff` empty before and after). No assertion was removed or
  relaxed, no timeout was raised, no test was skipped.
- After the fix: the three policy tests pass in the maintainer tree, and the
  **full product suite re-ran from scratch in the clean worktree: 233/233,
  0 failed, 2 skips, exit 0** (§13.3 step 3).

This is a pre-existing release-gate tooling defect, not a regression of A–D;
the round records it here rather than claiming the suites were green for the
wrong reason.

### 13.5 Transient-failure record

No `learning.repository.persistence` `io-error` occurred in any
commit-level or clean-worktree run of this round (the §6 observations stand:
one occurrence under concurrent full-suite I/O in the earlier round, never
reproduced; the IoStage/Win32 diagnostics now exist but have not yet captured
a recurrence). The only clean-worktree failures were the two provisioning
omissions and the pre-existing payload-pin defect above; each is recorded
with its root cause, and the re-runs were complete suites, not single-test
retries.

### 13.6 Stabilization-round status deltas

| Item | Before this round | After this round | Evidence |
| --- | --- | --- | --- |
| Commit grouping A–D | planned (SOURCE_VERIFIED) | COMMIT_LEVEL_INTEGRATION | §13.1–13.2 |
| E1.1 manifest | UNIT_TESTED + INTEGRATION_TESTED | + COMMIT_LEVEL_INTEGRATION | §13.2 (A), §13.3 |
| TSF timing | INTEGRATION_TESTED (controlled host) | + COMMIT_LEVEL_INTEGRATION | §13.2 (B) |
| Io-stage diagnostics | UNIT_TESTED + INTEGRATION_TESTED | + COMMIT_LEVEL_INTEGRATION | §13.2 (C) |
| Candidate contract | UNIT_TESTED + INTEGRATION_TESTED | + COMMIT_LEVEL_INTEGRATION | §13.2, §13.3 |
| Clean-worktree re-verification | NOT_EXECUTED (plan only) | INTEGRATION_TESTED (executed; plan doc updated) | §13.3; `docs/testing/clean-worktree-verification.md` |
| Product payload pins | stale CRLF bytes | canonical LF bytes (commit F) | §13.4 |
| CI | NOT_IMPLEMENTED (design only, no workflow file) | unchanged | §9 |
| Anything HOST_VERIFIED | none | still none | — |

Unchanged constraints: product persistent learning remains closed
(`ProductLearningWritesPermitted()` untouched); the public C ABI is unchanged;
`zhuyin` remains `unverified`; Candidate ABI v2 remains NOT_IMPLEMENTED;
E4/E5 not started; E6 remains a design; Android remains NOT_IMPLEMENTED; no
network I/O, telemetry or model download was added.
