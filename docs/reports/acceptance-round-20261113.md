# Internal acceptance round — 2026-11-13

**Status of this file:** implementation and pre-acceptance record. It is committed
before the final clean build so that the final evidence can bind one immutable
commit. The authoritative final result for that commit is written to
`build/windows-product/ACCEPTANCE-REPORT-<commit>.md` by the acceptance run; this
file does not claim a final PASS.

## 1. Starting point

- Branch: `feat/internal-acceptance-round`
- Starting commit: `8e7b8b4382159aa370b72ac3c70d7b9652fc7ded`
- Prior machine result: 15 PASS, 12 BLOCKED, 1 NOT_REQUIRED; nine technical
  blockers were `build-tests`, `candidate-quality`, `installer-artifact`,
  `installer-toolchain`, `installer-lifecycle`, `real-host`,
  `english-capabilities`, `offline-privacy`, `language-acceptance`.
- The worktree was clean and matched `origin/feat/internal-acceptance-round`.

## 2. What this round adds

### 2.1 build-tests — real link coverage without elevation

`tests/learning_repository/main.cc` no longer returns the CTest skip code when
`SeCreateSymbolicLinkPrivilege` is absent (Win32 1314). It still tries a real
directory/file symlink first. On an ordinary token it runs the equivalent
unprivileged fixtures instead:

- a directory junction for the name-surrogate reparse-point rejection
  (`IO_REPARSE_TAG_MOUNT_POINT` follows the same `CheckObject` branch);
- a hardlink to an external file for the `nNumberOfLinks == 1` leaf policy and
  the "external bytes unchanged" assertion.

The case log prints which fixture was used. The CTest `SKIP_RETURN_CODE` was
removed. This is not a claim that a real symlink ran on a machine that forbids
one; it is a real, unprivileged execution of the same security properties.

### 2.2 100,000 edits × 5 fixed seeds

`tests/reference_model/main.cc` is the aggregate reference-model protocol that
earlier phase reports listed as NOT_RUN. Per seed it performs exactly 100,000
edits across three real product layers:

- Korean `TwoSetComposer` against an independently written standard Hangul
  automaton (text, active span, caret, step count, rejection, commit, undo);
- Japanese `RomajiComposer` against the documented pure-function contract:
  exact predecessor snapshots on backspace, periodic full-history replay into a
  fresh composer, commit/reset, mode boundaries and rejected bytes;
- TSF `EditPlan` against an independent status/field model (context identity,
  negative and non-collapsed selections, malformed UTF-8, U+FFFC, token range,
  monotonic sequence).

The fixed seeds are `0x5eed000000000001` … `0x5eed000000000005`; a failure prints
the seed and operation index and the RNG stream can be replayed with
`--seed 0x...`. Every 10,000 operations the harness samples working set and
handle count and fails on gross growth. `--force-fail` is the negative control.

### 2.3 English capabilities

`tests/english_core/language_support_tests.cc` now exposes
`--capability <frequency|context-learning|morphology|spelling|segmentation|abbreviation>`,
each running only that capability's assertions. Six CTest cases are registered.
`tools/product/run_english_acceptance.ps1` executes those six cases plus a real
`product_sink_tests --mode multi` cross-session case (two independent TSF
sessions in one process) and writes hash-bound JUnit and release evidence. The
cross-session case is not a real Notepad/Chrome/Word claim; that remains the
separate `real-host` gate.

### 2.4 Offline privacy

`product_sink_tests --mode privacy` types a fresh canary through an ordinary
document and a password-scoped document on one running engine lifetime, then
reads every isolated user-data file and asserts the canary is absent and no
learning store file exists. `tools/product/run_offline_privacy.ps1` additionally:

- runs `dumpbin /imports` on `EpoCanva.exe`, `epocanva_tsf.dll` and `rime.dll`
  and rejects `ws2_32`, `wsock32`, `wininet`, `winhttp`, `urlmon`, `libcurl`,
  `mswsock` and `dnsapi`;
- launches the privacy run and polls `Get-NetTCPConnection` and
  `Get-NetUDPEndpoint` for the process and its descendants;
- searches the process output and the isolated user tree for the canary,
  without ever writing the canary to the evidence log;
- writes four JUnit cases with the exact gate names and binds them to the
  current package/build identity.

A missing monitoring cmdlet or dumpbin fails the case; it is never skipped.

### 2.5 Installer artifact and toolchain

`tools/installer/build_helper.ps1` builds the project-owned TSF profile helper.
`tools/installer/build_msi.ps1` generates a WiX v4-schema source from the
verified package manifest and builds a per-user x64 MSI with WiX 6.0.2:

- 66 runtime payload files under `%LOCALAPPDATA%\EpoCanva`;
- HKCU COM registration (`InprocServer32` and both TSF implemented categories);
- the profile helper runs `register` / `unregister` as a deferred custom action;
- a numeric `ProductVersion` derived from `product/VERSION`;
- administrative extraction (`msiexec /a`) verifies every payload hash and the
  helper before the artifact is recorded.

`tools/installer/record_installer.ps1` writes the top-level `installer`
reference into release evidence. `check_release.ps1` now discovers a pinned
builder under `build/_local/wix-tool` or `build/tools/wix` in addition to PATH;
the artifact gate still requires a real installer file and the lifecycle gate
still requires real install/upgrade/repair/uninstall/rollback/cleanup.

## 3. What was actually executed before this commit

| Check | Result |
| --- | --- |
| Root Debug configure + build | exit 0 |
| Root CTest (94 cases, old tree) | 93 passed, 1 skipped (`learning.repository.symlinks`) |
| Root CTest with the link fix + new tests | one timing failure under concurrent product-build I/O (`english.core.spelling_cost_trial_on_pilot_lexicon`); standalone rerun passed |
| Product Release build/tests at the prior HEAD | 238 registered; 2 sink readiness timeouts under concurrent I/O (`sink_settings_timing`, `sink_suggestion_resync`); both standalone reruns passed (8.93 s and 6.43 s) |
| `windows.tsf_context_observer.real_focus` | passed in the visible-window harness (0.87 s) |
| Reference-model stress | 5 seeds × 100,000 ops = 500,000 ops, 111,482–111,764 checks/seed, PASS; handles 61→61, peak working set ≈5.7 MB, ≈1.1 s/seed |
| Reference-model negative control | exit 1 as required |
| `learning.repository.symlinks` with the unprivileged fixtures | PASS; log names the junction/hardlink substitution |
| `product.integration.sink_privacy` | PASS (5.43 s warm), canary absent, 11 user-data files scanned |
| WiX 6.0.2 helper + MSI build | helper exit 0; MSI 25,116,672 bytes; administrative extraction verified 68 files (66 payload + helper + manifest) |

These observations were made before the final clean rebuild. They are recorded
here as development evidence, not as the final gate result.

## 4. Still blocked at this commit

- `candidate-quality`: the official corpus is still 15 Tier A cases, the Tier B
  lock is `not_approved`, and `absolute_quality.decision` is
  `pending_product_owner`. No threshold or corpus approval was invented.
- `real-host`, `installer-lifecycle`, and the `installed-host` quarter of
  `language-acceptance`: this machine is a normal OneDrive-synced desktop; no
  authorization was given to register a TSF profile or install the MSI here.
  The test producers and installer are prepared but were not run against real
  Notepad/Chrome/Word.
- 30-minute real-host soak: same authorization reason.
- GitHub Actions provenance: no push or workflow dispatch was authorized.
- `language-acceptance`: core/engine/TSF evidence can be produced, but the gate
  requires all 20 cases including installed-host; that aggregate remains
  blocked until an authorized installed run exists.

Nothing in this file converts a BLOCKED item to PASS.
