# Phase E non-determinism round — cold-start budget and replace retry

**Date**: 2026-11-12
**Branch**: `feat/internal-acceptance-round`
**Baseline HEAD**: `a070f9224b3f841819385143c0cb4bf1c41e206a`
**Parent branch**: `feat/phase-e-multilingual-fusion` (same commit)
**Status**: two measured non-deterministic failures were reproduced and localized;
both fixes below are in the working tree of this round. The product suite rerun and
the release-gate run for the exact final candidate are separate artifacts; this
report is not their substitute.

---

## 1. Scope and workspace at the start of the round

```text
git rev-parse --show-toplevel            -> <repo>
git branch --show-current                -> feat/internal-acceptance-round
git rev-parse HEAD                       -> a070f9224b3f841819385143c0cb4bf1c41e206a
git status --short                       -> three modified files (learning repository)
git diff --stat                          -> 55 insertions(+), 7 deletions(-)
git submodule status --recursive         -> no output (no initialized submodules at the root)
```

The three modified files were the previous, unfinished work of this branch and were
read and adopted rather than reverted:

- `engine/data_store/windows_learning_repository.cc`
- `engine/data_store/windows_learning_repository.h`
- `tests/learning_repository/main.cc`

This round adds `tests/windows/tsf_product/product_sink_tests.cc` (diagnostics and one
budget constant). No user-authored change was discarded, stashed or rewritten.

The environment is the one recorded in `docs/reports/environment.local.md`: Windows
11 10.0.26200 x64, VS 2022 Professional 17.14, MSVC 19.44, CMake 3.31.6-msvc6,
C++17 with extensions off, workspace on a OneDrive-synced volume. That last fact is
part of both root causes below.

---

## 2. Failure 1 — `product.integration.sink_settings_timing`

### 2.1 First observation

A verified product build (`tools/product/build_product.ps1`) ran 236 registered tests
and stopped with one failure:

```text
215/236 Test #215: product.integration.sink_settings_timing ...***Failed  96.65 sec
  FAIL timing: the first session is ready
FAILED: 1 check(s)
exit code mismatch, got 14 expected 0
```

The remaining 235 tests passed; `sink_raw` in the same run took 126.89 s, against
34.21 s in the earlier full run. The process was a separate, normally exited CTest
case; nothing was rerun or filtered at that point.

### 2.2 Reproduction and the blocked field

The diagnostics added by this round print the product's own readiness counters on a
timeout. Running the complete `product.integration.sink_*` group reproduced the
failure on the next attempt:

```text
[readiness-timeout] elapsed_ms=90000 have_counters=1 last_probe_eaten=0
[...] engine=1 estate=2 eatt=0 [...] bound=0 [...] perm=2 [...] queue=0/0 [...] phase=0
FAIL timing: the first session is ready
```

`engine=1` and `estate=2` (`EngineBootstrapState::kReady`) prove the engine host had
started. `bound=0` and `perm=2` (`PermissionState::kUnknown`) show the coordinator had
not yet received a key that could bind the context, because the engine became ready
only at the end of the 90 s window. There was no denial, no attach failure
(`eatt=0`), no queue backlog and no eaten key.

The user-data tree confirms the same ordering: `settings.ini` was written at
16:07:38 UTC and the deployment's `deployed-state` at 16:09:14 UTC, 96 s later.

### 2.3 What the cold start actually costs

The timing case deliberately starts from a removed user root, so its first activation
compiles the multilingual deployment (`epocanva_backend_terra.table.bin`, pinyin and
zhuyin prisms). Measurements on the same build and machine:

| Case | Observed |
| --- | --- |
| Warm second session inside the running engine | 62–79 ms |
| Warm restart after the deployment exists | ~1 s |
| Cold deployment, fresh root, quiet volume (three runs) | 9–10 s |
| Cold deployment during the saturated sink suite | 96 s |

The 96 s case is not a different code path: every phase completed, the deployment was
valid, and the restarted engine served input with zero suppressed text. It is an
external storage stall on the OneDrive-synced volume, the same class of provider
interference measured for the learning store in §3. The test's 90 s readiness budget
was below the measured worst-case cold start.

### 2.4 Fix

`tests/windows/tsf_product/product_sink_tests.cc`:

- `kColdStartReadinessBudgetMs = 180000` is used **only** for the first session of
  `RunSettingsTimingCase`; the warm-session budget stays at 90 s. A start that never
  completes still fails, and the case still asserts the deployment marker, the
  compiled schema, the settings digest and the restarted engine's document.
- `ReadyToType` now prints the counters that define readiness when it times out
  (`elapsed_ms`, `have_counters`, `last_probe_eaten`, then the usual text-free
  counters), so the next occurrence names the blocking field instead of leaving only
  "not ready".

This is a registered test budget, not a silent pass: the value and its measurements
are written in the source comment and here, and the check remains falsifiable.

### 2.5 Verification

```text
ctest --test-dir build/windows-product -C Release -R '^product\.integration\.sink_'
  -> exit 0; 21/21 passed (sink_raw 127.39 s, settings_timing 8.70 s)
ctest --preset vs2022-x64-debug --output-on-failure --no-tests=error
  -> exit 0; 94/94 passed, 1 explicit skip (learning.repository.symlinks)
```

---

## 3. Failure 2 — learning-store publication on a synced volume

### 3.1 Observation

`learning.repository.*` had produced rare, non-reproducible `kIoError` failures on
this workspace. The failure kept no stage or Win32 code, so it could not be
diagnosed from the log. The previous unfinished change adds that diagnostic and a
bounded retry; this round measured both.

Measured on 2026-11-11 in this repository, with the store on the OneDrive volume:

- `SetFileInformationByHandle(..., FileRenameInfoEx)` with
  `FILE_RENAME_FLAG_REPLACE_IF_EXISTS` returned `ERROR_ACCESS_DENIED` on roughly
  1 publication in 300 otherwise identical transactions;
- the same sequence on a non-synced local volume recorded 0 failures in 3,000;
- in every observed failure the durable store was intact, the staging name was gone,
  and an immediate retry of the identical transaction succeeded.

### 3.2 Fix

`engine/data_store/windows_learning_repository.cc` retries the final atomic replace up
to four times, 2 ms apart, and only for `ERROR_ACCESS_DENIED`. Every attempt re-checks
the permission gate immediately before the rename. A permanent denial (read-only
target, no-delete sharing handle, reparse point) still fails after the budget with the
same `IoStage::kReplace` and Win32 code. `ERROR_SHARING_VIOLATION` and every other
error fail on the first attempt, so a busy lock is not converted into a success.

`tests/learning_repository/main.cc` now prints the stage and Win32 code when an
expected status is `kIoError`, so a future failure is actionable without logging any
user data.

### 3.3 Verification

```text
epocanva_learning_repository_tests.exe persistence <temp>   x30 -> 0 failures
ctest learning.repository.{persistence,privacy,revocation,errors,corruption,
  feedback,management,acl,paths,concurrency,faults,force-failure,invalid-mode}
  -> exit 0; all passed (faults covers permanent delete/open denials and kBusy)
```

---

## 4. Remaining skips and unexecuted verification

These are recorded here because they are **not** passes and they are not repaired by
the two fixes above:

| Item | State | Reason |
| --- | --- | --- |
| `learning.repository.symlinks` | SKIPPED (77) | `CreateSymbolicLinkW` returns Win32 1314; no elevation or Developer Mode was requested or attempted |
| `windows.tsf_context_observer.real_focus` | SKIPPED (77) | The controlled thread's `ITfThreadMgr::SetFocus` changes `GetFocus` but TSF does not deliver `OnSetFocus` in this environment; the fallback revalidation path is asserted |
| 100,000-operation aggregate edit protocol with five seeds | NOT_RUN | no aggregate reference-model harness exists in the current tree; the per-operation classes are covered by the sink cases, but that is not the protocol |
| Installed-host and installer lifecycle | BLOCKED | no installer builder on PATH and no authorization for registration/UAC/system changes |

The release gate refuses both skips by design (`build-tests` requires zero skipped
tests); that is a real blocker for aggregate acceptance and is not waived here.

---

## 5. Files changed in this round

- `engine/data_store/windows_learning_repository.cc` — bounded replace retry.
- `engine/data_store/windows_learning_repository.h` — contract comment.
- `tests/learning_repository/main.cc` — stage/Win32 diagnostics on failure.
- `tests/windows/tsf_product/product_sink_tests.cc` — cold-start budget, readiness
  timeout diagnostics.
- `docs/reports/phase-e-nondeterminism-20261112.md` — this report.
