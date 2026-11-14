# Windows acceptance and public-release gate

`tools/product/check_release.ps1` checks one exact candidate and reports technical acceptance, public-distribution authorization and signatures separately. The user selected AGPL-3.0-only and internal unsigned acceptance first; `docs/product/license-decision.md` and `data/manifests/license-decision.json` record that scope. They do not approve public release or word-list redistribution jurisdictions.

## Run and result semantics

```powershell
powershell.exe -NoProfile -NonInteractive -File tools/product/check_release.ps1 `
  -BuildDir <repo>/build/<preset> -AcceptanceProfile internal-unsigned
```

Optional inputs are `-RepoRoot`, `-BuildDir`, `-StageRoot`, `-RimeDll`, `-EvidencePath` and `-AcceptanceProfile internal-unsigned|public-signed`. Defaults are the current repository, `build/windows-product`, `<BuildDir>/stage/EpoCanva-<VERSION>`, the existing English DLL at `build/librime-english/rime/install/lib/rime.dll`, and `<BuildDir>/release-evidence.json`. `BuildDir` must be exactly `<repo>/build/<preset>`.

The checker reads files and discovers tools on PATH. It creates only `<BuildDir>/release-readiness.json` and its build directory. It performs no network lookup, build, stage, signature operation, registry change, installation or host input. No command in an evidence file is executed. Inputs must remain unchanged while checking.

| Report field | Meaning |
| --- | --- |
| `technical_ready` | All mandatory technical gates passed for the exact candidate, including clean source, complete payload/data, Corresponding Source, five languages, full English, actual installer and real-host acceptance. |
| `distribution_authorized` | Artifact-bound owner/legal records authorize public/external distribution, including word-list jurisdictions, and the manifest declares a public candidate. Internal acceptance authorization is not this field. |
| `signature_verified` | Current, artifact-bound signing verification actually passed. Never inferred from unsigned acceptance or from an installed signing tool. |
| `release_ready` | Technical readiness, public/external authorization and verified signatures all hold for `public-signed`. |
| `acceptance_ready` | The selected profile passed. For `internal-unsigned`, this equals technical readiness; for `public-signed`, it equals release readiness. |

Exit `0` means `acceptance_ready=true`, not necessarily `release_ready=true`. Exit `1` means profile acceptance is blocked. Exit `2` means an invalid output boundary or failed report write. A previous report must not be reused after an error or changed inputs.

The report has `format: 2`, `checks[]` with `id`, `category` (`technical`, `distribution`, `signature`), `status` (`passed`, `blocked`, `not_required`) and `detail`. `blockers[]` lists every blocked check; `acceptance_blockers[]` includes only blockers required by the selected profile. Internal unsigned acceptance omits signing only. Without signing evidence it reports `signing_status=not_required` and `signature_verified=false`. Any supplied signing evidence is checked; failures remain visible. Public owner records remain separately blocked until supplied. No manual boolean can waive missing technical evidence.

## Package format 2

`stage_product.ps1` takes the same acceptance profile, plus `-DistributionPolicy internal-only|public-candidate`. Its default is `internal-unsigned` / `internal-only` with strict paths. Output is below `<BuildDir>/stage/EpoCanva-*`. The generated `VERSION.txt` and `package.manifest` carry these independent values:

```text
epocanva-package-manifest 2
version=<exact product VERSION>
commit=<full source identity, including dirty fingerprint when present>
acceptance-profile=internal-unsigned
distribution-policy=internal-only
path-exception=none
file<TAB>LICENSE<TAB><sha256><TAB><byte count>
```

Every metadata key occurs exactly once. Recognized acceptance profiles are `internal-unsigned` and `public-signed`; distribution policies are `internal-only` and `public-candidate`; path exceptions are `none` and `embedded-build-paths`. These are metadata, not approval or signature claims. Public acceptance requires public-candidate metadata. `-InternalAllowEmbeddedBuildPaths` can create only an internal diagnostic package, records the exception explicitly, and prevents both local package readiness and technical acceptance. It never excuses text-payload paths or missing/changed licences.

Format 1 is readable for integrity diagnosis, including its legacy `internal-only` field. It cannot report package or acceptance readiness. Restage after rebuilding; do not hand-edit an old manifest to assert new evidence.

The native `--diagnose` report's `package=ready` means **local package integrity and isolated user-data access only**. It emits `manifest_format`, `acceptance_profile`, `distribution_policy`, `path_exception`, `readiness_scope=local-package-integrity-only` and `release_acceptance=not-evaluated-use-check_release.ps1`. It does not run host, installer, language, signature or source-archive acceptance. A strict internal format-2 package can pass local diagnosis while aggregate acceptance remains blocked.

## Mandatory technical gates

- Source/build/engine identity: current fingerprints, complete commit identity, non-trace Release configuration, successful executed CTest with exact registered/executed name sets and no skips, and hashes/byte counts of all three tested binaries. Dirty source identity is retained but `source-clean` blocks aggregate acceptance in both profiles. This checker never commits or strips dirty identity.
- Shared payload policy: `data/manifests/product-package-files.inc` fixes every non-generated source digest and byte count. It includes root `LICENSE`, `license-decision.json`, NOTICE, dependency notices and all data. Only the three binaries and generated `VERSION.txt` may have empty policy sources. Missing/changed files, duplicate/unsafe paths, links, unlisted files and mutable-manifest attempts to bless altered fixed content fail.
- Path hygiene: personal/build-path patterns are inspected in UTF-8 and UTF-16 without logging the matched content. A declared path exception remains blocked even if a later scan happens to find no match.
- Corresponding Source: the actual ZIP and source manifest must match the exact source/build/package/policy, required source set and per-entry bytes. See below.
- Languages: the exact ordered required set is `en,zh,fr,ja,ko`. A central status field cannot by itself satisfy this gate. Each language needs actual data in the fixed package policy, current hashed source references and real core/engine/TSF/installed-host test results. Full English frequency/context learning/morphology/spelling/segmentation/abbreviation/cross-app tests remain separately mandatory.
- Installation and real hosts: exact installer artifact, available builder and actual install/upgrade/repair/uninstall/rollback/cleanup, real Notepad/Chromium/Word input, sensitive-field and cleanup evidence. Registration alone or core algorithms alone do not count.
- Privacy/offline: actual runtime and dependency review plus no-content-logging and sensitive-no-persistence evidence. Windows' ordinary in-process TSF DLL guarantee is architectural; it is not an OS network sandbox. Android can enforce absence of network permission by omitting `android.permission.INTERNET`. This Windows checker does not certify an Android build.
- Reference-model stress: `stress-reference-model` is mandatory. Its measured report must contain all five fixed seeds with exactly 100,000 edits each, an independent editor oracle, insert/delete/UTF-16 caret/UTF-16 selection/undo/invalid-state coverage per seed, replay/minimization, resource samples and a negative control. The existing Korean/Japanese/EditPlan stress suite is useful component evidence, but does not by itself satisfy this complete-editor contract.
- Installed-host soak: `host-soak` requires at least 1,800 measured seconds in an authorized isolated interactive Windows desktop, installed and activated input, continuous input/candidate progress and resource samples no more than 60 seconds apart, approved performance criteria, host observations and cleanup. Sleeping for thirty minutes is not soak evidence.
- Acceptance CI: `acceptance-ci-provenance` requires successful, exact-commit GitHub run/jobs snapshots for all six required layers/jobs, an artifact manifest and runner authorization. Contract-only CI or an older successful commit cannot satisfy product acceptance. The checker performs no network access; hashing API snapshots binds their bytes but does not authenticate the retrieval or authorize a runner.
- Stable inputs: current source, receipt, policy, licence/language decisions, manifest, all payloads, source archive and referenced evidence are rechecked before reporting.

## Release evidence format 2

The checker reads but never creates passing owner/host/installer evidence. Required top-level fields are integer `format: 2`, `version`, `commit`, `source_sha256`, `package_manifest_sha256`, `build_receipt_sha256`, `package_policy_sha256`, `acceptance_profile`, `distribution_policy`, `license_decision_sha256`, `language_policy_sha256`, `source_manifest`, `installer` and `entries` (array of uniquely named entries). All identity values match the current candidate exactly. Format 1 evidence cannot satisfy this contract.

File references have `{path, sha256, bytes}`: repository-relative forward-slash path, lowercase SHA-256 and exact nonzero byte count. Absolute paths, parent traversal, links, mismatches and booleans in place of byte counts fail. Hashes bind records but do not authenticate a signer, decision-maker or test result. The origin and authority of supplied records remain auditable owner responsibilities.

Public authorization IDs `project-license` and `lexicon-redistribution` require `kind: "owner-record"`, attributable `owner`, stable `record_id`, `decision: "approved-public-distribution"`, nonempty string `jurisdictions[]` and `record` (file reference). These are not replaced by the license/profile decision JSON. The record must identify the exact artifact scope and chosen terms; no such approvals are generated here.

The `package-diagnose` entry has `kind: "diagnose"`, execution fields below and `report` (file reference). Its recorded command is exactly:

```powershell
& <StageRoot>/EpoCanva.exe --diagnose <BuildDir>/<report> --user-data-root <BuildDir>/diagnose-user
```

Paths in the argument array are absolute Windows-normalized paths; the report and user root remain under the build directory. Native output must match identity, format-2 policies, file count, `package=ready`, `manifest=ok`, `manifest_failures=0`, `user_data=ok` and the isolated user root.

Test-run entries require `kind: "test-run"`, hashed `runner`, `log`, `junit` file references, plus actual `command` argument array, integer process `exit_code: 0`, `started_utc` and `completed_utc` timestamps. The command begins with the hashed runner's absolute path. JUnit must have executed named cases, no failures/errors/skips and consistent summary counts; DTD/external entities are rejected.

Every JUnit contains exactly one property each named `epocanva.version`, `epocanva.commit`, `epocanva.source_sha256`, `epocanva.package_manifest_sha256`, `epocanva.language_policy_sha256`, `epocanva.acceptance_profile`. Installer, real-host, language-acceptance and signing reports also need `epocanva.installer_sha256`. The runner captures these against the tested candidate; do not retrofit historical results.

| Entry ID | Required case suffixes (each fully named `<id>.<suffix>`) |
| --- | --- |
| `installer-lifecycle` | `install`, `repeat-install`, `upgrade`, `repair`, `uninstall`, `rollback`, `cleanup`, `multi-user`, `registration`, `user-data-policy` |
| `real-host` | `registration`, `test-start`, `notepad`, `chromium`, `word`, `sensitive-fields`, `english-cross-app-consistency`, `cleanup` |
| `english-capabilities` | `frequency`, `context-learning`, `morphology`, `spelling`, `segmentation`, `abbreviation`, `cross-app-consistency` |
| `offline-privacy` | `runtime-no-network`, `dependency-review`, `no-content-logging`, `sensitive-no-persistence` |
| `language-acceptance` | For each of `en`, `zh`, `fr`, `ja`, `ko`: `<language>.core`, `<language>.engine`, `<language>.tsf`, `<language>.installed-host` |
| `stress-reference-model` | `seed-1` … `seed-5`, `negative-control`, `replay-minimization`, `resource-growth` |
| `host-soak` | `duration`, `notepad`, `chromium`, `word`, `language-switch`, `focus-switch`, `candidate-interaction`, `long-input`, `resource-growth`, `cleanup` |
| `acceptance-ci-provenance` | `commit`, `runner`, `build-configuration`, `toolchain`, `junit`, `exit-codes`, `artifact-hashes`, `source-archive` |
| `signing` | `EpoCanva.exe`, `epocanva_tsf.dll`, `rime.dll`, `installer`, `publisher`, `timestamp`, `trust` |

The three hard-acceptance entries additionally require a hash-bound `report` with
`format: epocanva-<entry-id>-v1`, all matching JUnit identity bindings, `result`,
`started_utc` and `completed_utc` exactly matching the recorded execution.
`host-soak` and `acceptance-ci-provenance` also bind the exact installer SHA-256.
Missing, failed, shortened, timed-out, unexecuted or mismatched evidence remains
BLOCKED; adding these contracts does not manufacture a passing producer.

For `stress-reference-model`, `seeds[]` contains the five fixed hexadecimal seed
IDs, integer `operations: 100000`, integer `mismatches: 0` and positive integer
`operation_counts` for each required operation. `total_ops` is 500000,
`oracle` is `independent-editor-reference-model`, `failure_reduction` is
`replay-and-minimize`; `replay_log`, `resource_samples`, `negative_control` are
file references. The existing component stress report intentionally does not
meet this schema until the missing editor/reduction coverage is implemented.

For `host-soak`, `duration_seconds` agrees with elapsed timestamps and is at
least 1800, `environment` is `authorized-isolated-interactive-windows`, and
`installed`/`activated` are true. At least 31 `samples[]` record `utc`, increasing
`completed_interactions`, `working_set_bytes`, `handles`, `cpu_seconds`.
`isolation_authorization`, `performance_approval`, `host_observations` are file
references. The schema enforces continuity and identity, not OS sandboxing or
approval authenticity; actual thresholds and behavioral assertions belong in
the authorized producer and approved test plan.

For `acceptance-ci-provenance`, `github_run` and `github_jobs` reference complete
GitHub REST run/jobs snapshots, `run_id` matches the successful completed run,
and every required job (`contract-gate`, `root-unit-tests`, `backend`,
`windows-product`, `windows-integration-acceptance`,
`full-quality-stress-acceptance`) must have succeeded at this exact commit.
`artifact_manifest` and `runner_authorization` are file references. A protected
runner and controlled retrieval are still required; JSON fields are not signed
attestations.

The central `data/manifests/language-readiness.json` is format 1, with exact `required_languages` and five unique `languages[]`. A completed language row has `id`, `status: "implemented"`, `data_status: "packaged"`, nonempty `payload_paths[]` (all fixed manifest payloads), and nonempty `source_paths[]` (file references matching actual sources). Initial `blocked` / `absent` / `incomplete` rows accurately prevent readiness. Tests must exercise real language behavior, not merely share these names.

## Acceptance evidence producers

The following scripts run real tests and write hash-bound `test-run` entries. They
do not create owner, host, installer-lifecycle or signing records, and a runner
that cannot execute its work exits nonzero rather than skipping.

- `tools/product/run_english_acceptance.ps1` — six separately executed English
  core capabilities plus the product TSF multi-session case. The latter is
  cross-session consistency, not a real-application claim. The aggregate
  `english-capabilities` gate also requires passing `real-host`, including the
  explicit installed-host `english-cross-app-consistency` case; this producer
  alone cannot turn the seventh capability green.
- `tools/product/run_offline_privacy.ps1` — static import review, live TCP/UDP
  endpoint observation of the product sink process tree, and a synthetic canary
  searched through stdout, stderr and the isolated user-data tree, including a
  password-scoped document. A missing monitoring cmdlet or `dumpbin` fails the
  corresponding case. This controlled-process polling is development evidence,
  not proof that no transient network event occurred in installed applications.
  The aggregate privacy gate additionally requires authorized `real-host`
  sensitive-field acceptance; static imports and this producer cannot replace it.
- `tests/reference_model` — the 100,000-edit × 5 fixed-seed aggregate protocol
  with an independent Korean automaton, Japanese history/replay and TSF
  `EditPlan` status models. It has a separate `--force-fail` negative control.
- `tools/installer/build_helper.ps1`, `build_msi.ps1` and `record_installer.ps1`
  — build the project-owned profile helper, a per-user x64 MSI from the verified
  package manifest, and the top-level `installer` evidence reference. The MSI is
  verified by administrative extraction; that is not an install or lifecycle
  claim.

`check_release.ps1` discovers a pinned installer builder under
`build/_local/wix-tool` or `build/tools/wix` after PATH. The pinned location is a
convenience for reproducible local builds; the `installer-artifact` gate still
requires a real installer file and `installer-lifecycle` still requires executed
install/upgrade/repair/uninstall/rollback/cleanup evidence.

## Corresponding Source producer/consumer contract

`tools/product/stage_source.ps1` produces `<BuildDir>/corresponding-source.zip` and `<BuildDir>/source-manifest.json`. Shared `tools/product/source_contract.ps1` exposes `Get-ProductCorrespondingSourceFiles -RepoRoot`, returning sorted unique repository-relative forward-slash string paths; they are the exact ZIP entry names. Generated archives, manifests, receipts and reports never feed back into the product source fingerprint.

The source manifest uses integer `format: 1`, exact `version`, `commit`, `source_sha256`, `package_manifest_sha256`, `build_receipt_sha256`, `package_policy_sha256`, `archive` (file reference to the ZIP) and `files[]` entries `{path, sha256, bytes}`. Source entries may have zero bytes for actual empty source files; evidence references may not. The release-evidence `source_manifest` reference hashes this manifest.

The required source contract includes project source/interfaces, build and installation control scripts, root licence/decision scope, pinned required dependencies and notices, required Boost headers and actual word-list source/lock/converter/provenance inputs. It excludes unrelated build trees, `.git`, user data and generated evidence. The checker opens the actual ZIP without extracting, rejects unsafe/duplicate/extra/link entries, and hashes every entry; counts and each digest/byte count must match both the current source contract and source manifest. An altered outer JSON cannot bless a different archive or source tree. Missing producer inputs or an absent archive remain blocked, including internal acceptance.

## Validation boundaries

Use an isolated `build/<preset>/` for parser checks, native product unit tests and positive/negative gate tests. Unit fixtures must be clearly marked; they are not release, host or owner evidence. Tests should cover strict internal policy, diagnostic-only legacy policy, path exceptions, omitted/altered licence/data payloads, missing/stale source archive, binding mismatches, skipped JUnit and missing real acceptance. Public signing is an acceptance policy, not a claim Windows universally requires signatures to load an ordinary DLL. Existing historical C-layer scripts are not invoked or promoted into new evidence by this checker.
