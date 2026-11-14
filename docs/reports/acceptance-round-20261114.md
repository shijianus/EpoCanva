# Internal acceptance hardening — 2026-11-14

**Status: BLOCKED_NOT_COMPLETE.** This is a development/audit record, not a final
acceptance certificate. Final commands and candidate identity are written below
`build/acceptance-20261114/`; a historical PASS is not promoted to this candidate.

## 1. Environment and baseline

- Windows x64, Git Bash with Windows PowerShell 5.1. The checkout is in a
  OneDrive-synchronized directory, not an authorized isolated acceptance desktop.
- Starting branch: `feat/internal-acceptance-round`.
- Starting HEAD: `0b449da52dcb80660928ee10b45ec1df570c7810`; initially clean.
- `git fetch origin` exited 0. Remote acceptance branch:
  `8e7b8b4382159aa370b72ac3c70d7b9652fc7ded`; local was three commits ahead,
  zero behind. Remote main: `5c0f11b1fa94c965e1a00f920272860c2d20ff73`.
- Public GitHub REST query observed run `43223329762`, successful `windows-ci`
  for `8e7b8b4`, created `2026-11-12T13:01:34Z`. It is not this HEAD's CI.
  The preceding run `43220855206` for `39176fc` failed.
- `command -v gh` / `gh auth status`: `gh` unavailable (shell command-not-found).
  Read-only public GitHub API queries used `curl` instead. No push or workflow
  dispatch occurred as part of the initial inspection.
- CMake and CTest are not on the ordinary shell PATH, but existing VS2022 tools
  were found and executed: `3.31.6-msvc6`. Existing local WiX reports
  `6.0.2+b3f3403`. No system package installation, elevation, registry change,
  input-method activation or installer lifecycle run was authorized or performed.
- Existing `product-build-receipt.json` was `in_progress`; existing readiness
  still named `39176fc`. Neither is valid evidence for the new candidate.

## 2. Issue register

| ID | Module / priority | Root cause and reproduction | Action / files | Verification and status |
| --- | --- | --- | --- | --- |
| IA14-01 | Candidate producer / P1 | `-Manifest` feeds the evaluator while identity hashes the default manifest; recorded command omits input overrides. | Normalize and consistently bind actual input; preserve reproducible command. `tools/product/run_candidate_quality.ps1` and dedicated regression. | Local repair and regression; formal qualification remains dependent on approved corpus/thresholds. |
| IA14-02 | Stress runner / P1 | Report-open failure returns silently; requested rather than executed operations can appear in a failure report. | Fail nonzero on report I/O errors and report actual execution. `tests/reference_model`. | Positive 100000×5, negative control, report failure and count regressions. |
| IA14-03 | Release gate / P1 | Full editor stress, installed-host soak and complete acceptance-CI provenance were not mandatory gates. | Add fail-closed report/JUnit contracts and negative regressions. `tools/product/check_release.ps1`, `platforms/windows/product/tests/release_gate_tests.ps1`. | Missing/shortened/stale/idle-only/incomplete evidence must remain BLOCKED. |
| IA14-04 | English acceptance / P1 | Two controlled same-process sessions could satisfy the cross-app-labelled case without installed Notepad/Chrome/Word. | Require real-host with explicit English cross-app case before English aggregate PASS. Same gate and docs. | Unit evidence cannot independently establish installed-host acceptance. |
| IA14-05 | Installer lifecycle / P1 | Existing required list omitted repeat install, multiple users, registration and user-data-policy cases. | Extend mandatory case set, preserve old requirements. Same gate and docs. | Missing cases block; no system lifecycle run on this desktop. |
| IA14-06 | Hosted CI / P1 | Contract-only successful CI does not build/test the complete root suite. | Add safe hosted full-root layer, strict JUnit inventory and execution metadata; no privileged runner exposed to PRs. `.github/workflows/windows-ci.yml`, `docs/ci/windows-ci.md`. | Local root execution and workflow checks are not a remote CI run. |
| IA14-07 | Installer toolchain / P1 | Builder accepts arbitrary WiX 6.x despite claiming a pinned 6.0.2 toolchain. | Exact toolchain verification; installer-specific regression. `tools/installer`. | Record actual executable/package provenance; do not invent supply-chain approval. |
| IA14-08 | Candidate docs / P3 | Stale warm-up and removed CLI options conflict with current evaluator. | Update `docs/testing/candidate-evaluation-baseline.md` from source. | Documentation does not constitute qualification execution. |
| IA14-09 | Test development / P1 | First new soak parser accepted 1799 reported seconds within a 1-second timestamp tolerance. | Add an explicit reported-duration minimum of 1800. | Initial regression exit 14, 1/148 assertions failed; retained in `release-gate-tests.log`; second full parser run exit 0, 148 assertions. |
| IA14-10 | Privacy producer / P0-P1 | Canary scan skips files larger than 16 MiB and silently ignores read/enumeration errors; raw child output is appended before canary redaction; network query errors can appear as empty observations. | Stream all bytes, fail closed on incomplete scans/monitoring and redact before log persistence. `tools/product/run_offline_privacy.ps1` plus dedicated behavioral regression. | Large/cross-boundary canary, missing/inaccessible tree, redaction and observation-error tests; this does not replace authorized installed-host privacy acceptance. |

Limited source review of sensitive-field denial, worker learning gates, focus
loss and sink shutdown did not establish a P0/P1 runtime defect. This is not a
complete memory/concurrency/ABI audit and no speculative runtime rewrite was
made. Persistence writes must remain disabled until the field-permission
lifetime is genuinely accepted.

## 3. Dependency order and design choices

1. Preserve the clean starting state and inspect actual remote/CI identity.
2. Confirm runnable local toolchain without changing system configuration.
3. Repair evidence truthfulness before creating new build receipts.
4. Run targeted positive/negative regression; retain original failures.
5. Freeze a candidate, then rebuild and execute the complete root and product
   suites serially; regenerate identity-bound package/source/evidence outputs.
6. Only an authorized isolated desktop can run lifecycle/installed-host/soak.
7. Only attributable product/data owners can approve Tier B and thresholds.
8. Complete acceptance CI requires approved infrastructure and actual execution;
   a hosted contract/root success is insufficient.

Rejected alternatives: removing skips without equivalent exercised security
properties; labelling composer replay a full independent editor model; counting
sleep as soak; manufacturing approved corpus/threshold records; treating MSI
extraction as installation; reporting an old successful CI run as current.

## 4. Actual preliminary executions

The commands below ran before the final candidate was frozen. They are
**development observations**, not final acceptance evidence.

| Command | Actual exit / observation |
| --- | --- |
| `git rev-parse --show-toplevel`; `git status --porcelain=v2 --branch`; `git remote -v`; `git log -10`; `git branch -avv`; `git ls-remote --heads origin` | Completed; repository and remote identities inspected; initially clean, three local commits ahead. |
| `git fetch origin` | 0; no merge or source checkout change. |
| `curl --fail --silent --show-error -H 'Accept: application/vnd.github+json' 'https://api.github.com/repos/shijianus/EpoCanva/actions/runs?per_page=5'` | Public API response parsed successfully; most recent run above. |
| Existing VS CMake `--version`; CTest `--version`; local `wix.exe --version` | All 0; versions above. |
| `cmake --preset vs2022-x64-debug` | 0; log `root-configure.log`. |
| `cmake --build --preset vs2022-x64-debug` | 0; log `root-build.log`. |
| `ctest --test-dir build/vs2022-x64-debug -C Debug --output-on-failure --no-tests=error --output-junit ../acceptance-20261114/root-ctest.xml` | 0; 102 testcases, 0 failures/errors/skips, 152.43 seconds; executed before subsequent edits. |
| `powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File platforms/windows/product/tests/release_gate_tests.ps1 -RunRoot build/acceptance-gate-20261114` | 14; first new parser regression exposed IA14-09; not hidden by retry. |
| `powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File platforms/windows/product/tests/release_gate_tests.ps1 -RunRoot build/acceptance-gate-r2-20261114` | 0; 148 assertions, including positive/negative stress, soak and CI-provenance parser cases. |
| `powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File platforms/windows/product/tests/privacy_producer_tests.ps1 -RunRoot build/acceptance-20261114` | 0; 9 synthetic scanner/redaction/observation assertions. |
| Same privacy producer with `-ForceFailure` | 14 as required; negative control did fail. |
| `python -B -m unittest discover -s tests/ci_gate -p test_verify_root_junit.py -v` | 0; exact-root-JUnit parser positive/negative regression passed. |
| `powershell.exe ... evidence_input_tests.ps1 -RunRoot build/acceptance-20261114` | First run 1 due to test-fixture AST assumption; corrected run `r2` exit 0; custom manifest and exact WiX version cases passed. |
| `powershell ... tools/product/check_release.ps1 -BuildDir build/windows-product -StageRoot build/windows-product/stage/EpoCanva-0.2.6-w3b-20261114 -EvidencePath build/windows-product/release-evidence.json -AcceptanceProfile internal-unsigned` | Read-only gate exit `1`; `acceptance_ready=false`, `technical_ready=false`. It blocked build-tests (receipt skipped=1), dirty source, stale package/evidence identities, missing current evidence, corresponding source, candidate quality, installer, host, language, stress, soak and CI provenance. |

All listed build logs are under `build/acceptance-20261114/`. Exact later
commands, versions, candidate hashes and exits belong to the generated run
record there. Build artifacts, user fixtures and canary text are not committed.

## 5. Remaining mandatory external blockers

- **Candidate qualification:** actual JSONL counts are en=2, zh=4, fr=3, ja=4,
  ko=2 (15 total), versus required 1001/1001/1001/1001/500. Tier B lock is
  `not_approved`; absolute quality is `pending_product_owner`. No approval or
  successful qualification is inferred. The unverified zhuyin fixture is not
  automatically a valid qualification sample.
- **Authorized installed Windows environment:** no authorization to install/
  register here. MSI lifecycle, Notepad/Chrome/Word, installed-host language
  cases, sensitive-field host validation and the 30-minute soak are NOT RUN.
- **Complete reference editor model:** existing KO/JA/EditPlan component stress
  is not full document editing with independent undo/caret/selection behavior
  and failure-sequence minimization. The stronger aggregate gate remains blocked.
- **Performance:** product-owner-approved acceptance thresholds and continuous
  host resource observations are required; the agent cannot approve them.
- **Complete CI:** no new run can exist before the source is available to an
  authorized runner; privileged self-hosted acceptance must not execute
  untrusted pull requests. Local tests do not close provenance.
- **Public distribution:** remains unauthorized. Internal unsigned scope never
  implies public redistribution permission, signature verification or a network
  sandbox for an ordinary TSF DLL. The Windows guarantee is architectural;
  Android can enforce no network permission by omitting INTERNET.

## 6. Completion rule

Do not label this round `INTERNAL_ACCEPTANCE_COMPLETE` while any mandatory
condition is missing. The generated final `release-readiness.json`, actual
process exit codes, JUnit files and immutable candidate identity are decisive.
Any partial artifact is for review/internal development only, not an accepted
Windows product or a public release.


The final serial run used the dirty source identity recorded at
`build/acceptance-20261114/final-source-identity.json`:
`0b449da52dcb80660928ee10b45ec1df570c7810+dirty.bb26d18db181`.

- Root Debug: **103/103 passed**, zero failures/errors/skips; exact inventory/JUnit
  verifier exit 0. The count includes the new report-unwritable stress negative
  control and privacy/evidence parser contract tests where registered.
- Product Release: build command exit 0 and receipt `verified`, but **251/252
  passed with one skipped**: `windows.tsf_context_observer.real_focus`. The
  receipt deliberately records `skipped=1`; `build-tests` therefore remains
  blocked. No skip was removed or waived.
- Reference component stress: report result `passed`, five fixed seeds × 100,000
  operations = 500,000; report write failure and `--force-fail` negative controls
  are covered. This is not the stronger full-editor stress gate.
- Persistence regression: 100 repeated `learning.repository.persistence` runs,
  exit 0, 100 pass lines.
- Release-gate parser regression: second run exit 0, 148 assertions. First run
  exit 14 exposed and fixed a 1,799-second soak-duration boundary acceptance.
- Privacy producer regression: 9 assertions exit 0; forced negative exit 14.
  It covers large/cross-block scans, scan failures, pre-persistence redaction and
  observation failure fail-closed behavior. It is synthetic producer evidence,
  not installed-host privacy acceptance.
- CI parser regression: 20 Python unit tests exit 0. Workflow PowerShell blocks:
  23 parsed successfully after replacing GitHub expressions; full YAML parsing was
  not available because no YAML parser was installed. The root CI job was not run
  remotely.
- Candidate input/WiX regression: corrected run exit 0; custom manifest input
  identity and exact local WiX `6.0.2+b3f3403` behavior pass. WiX source provenance
  remains unverified; no MSI build or install was executed.
- Current read-only release gate exit 1: `acceptance_ready=false`,
  `technical_ready=false`, `distribution_authorized=false`,
  `release_ready=false`. The full machine report is
  `build/acceptance-20261114/final-release-readiness.json`.

The machine-readable summary is `build/acceptance-20261114/final-results-summary.json`.
It is a local development record, not release evidence and not an approval.

## 8. Final blockers

1. Source is dirty because this round's changes are uncommitted; the final build
   receipt preserves that fact and aggregate acceptance rejects it.
2. One product test skipped on this environment; zero-skip Release must be rerun
   in an environment where `windows.tsf_context_observer.real_focus` genuinely
   executes. Do not convert SKIP 77 into PASS.
3. The current package's corresponding-source archive/evidence outputs remain
   stale/absent for the dirty candidate; old `1e073e4` artifacts were explicitly
   rejected. Re-run `stage_source.ps1` successfully against the exact current
   receipt before any candidate can be considered.
4. No current hash-bound release evidence exists for package diagnosis, license or
   lexicon owner records, candidate quality, installer, lifecycle, real hosts,
   language acceptance, offline privacy, full-editor stress, host soak or complete
   acceptance CI provenance.
5. Candidate corpus has only 15 authored rows (en 2, zh 4, fr 3, ja 4, ko 2),
   versus 4,504 required Tier B rows. Tier B is `not_approved`; absolute quality
   thresholds are `pending_product_owner`. No agent may invent these approvals.
6. This synchronized workspace has no authorized installed-host environment;
   no MSI install/upgrade/repair/uninstall/rollback, TSF registration, Notepad/
   Chromium/Word, or 30-minute host soak was performed.
7. Public distribution and signing remain unauthorized/not verified. Internal
   unsigned metadata is not public approval or a signature.

Do not label this candidate `INTERNAL_ACCEPTANCE_COMPLETE`, `RELEASE_READY` or
publicly distributable. The correct final state is **BLOCKED_NOT_COMPLETE**.
