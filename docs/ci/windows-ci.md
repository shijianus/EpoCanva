# Windows CI: candidate, execution and input evidence

Status (2026-11-25): all six required jobs are defined in the working tree. This
is an implementation, **not a successful remote acceptance run**. The repository
runner API snapshot obtained for this audit returned `total_count: 0`. There is
no authorized isolated runner or six-layer successful candidate to claim.
Historical hosted runs and local parser fixtures cannot satisfy acceptance.

## Job graph and trust boundary

The required serial graph is:

`contract-gate -> root-unit-tests -> backend -> windows-product -> windows-integration-acceptance -> full-quality-stress-acceptance`.

The first two jobs run on `windows-2022`. PR and push execution stays in these
hosted layers. The separate `windows-latest` canary uses continue-on-error and
is never required acceptance evidence. The four acceptance layers require a
`workflow_dispatch` on a protected branch of `shijianus/EpoCanva`, a configured
runner group and a designated unique runner label, and the protected environment
`epocanva-acceptance`. The group must restrict workflow/repository access; the
environment must require an authorized reviewer. YAML cannot create those controls
or certify their configuration.

Every acceptance job selects all labels `self-hosted`, `Windows`, `X64`,
`epocanva-acceptance-x64`, and the unique
`vars.EPOCANVA_ACCEPTANCE_RUNNER_LABEL`, in
`vars.EPOCANVA_ACCEPTANCE_RUNNER_GROUP`. GitHub routes a job to any eligible
runner; serial `needs` alone does not pin a machine. Provision the unique label
on exactly one authorized runner. The script also checks `RUNNER_NAME` against
independently provisioned `EPOCANVA_CI_RUNNER_NAME`. A different runner fails
closed because it lacks the same run/attempt predecessor evidence.

Sources: GitHub official [runner selection](https://docs.github.com/en/actions/how-tos/write-workflows/choose-where-workflows-run/choose-the-runner-for-a-job)
and [labels and groups](https://docs.github.com/en/actions/how-tos/manage-runners/self-hosted-runners/use-in-a-workflow), checked 2026-11-25.

The acceptance runner needs a restorable isolated interactive Windows session,
operator authorization and the real host applications. Do not give an untrusted
PR access to this machine. No current local run authorizes installation or TSF
registration. CI checkout/upload may use network; runtime code remains offline.
Windows ordinary TSF DLL isolation from network is an architectural guarantee,
not an OS sandbox. Android's separate product must omit INTERNET permission for
platform enforcement; this CI does not certify Android.

## Inputs and recovery

`tools/ci/inputs.lock.json` format 1 pins 21 files and one clean upstream Git
checkout. `verify_inputs.ps1` verifies exact bytes, canonical paths, actual Git
roots, HEAD, dirty/submodule state and rejects assume-unchanged/skip-worktree
index flags. The shared strict JSON parser rejects duplicate/case-aliased keys,
nonfinite numbers, comments, wrong types and unexpected fields. The verifier
never downloads or changes an input. Uninitialized optional upstream submodules
are accepted by this existing lock contract; the separately verified engine
provenance requires the four dependencies actually used by its build.

The 2026-11-25 English input update is documented in
`tools/ci/provenance/english-engine-20261125.json`. It binds the built receipt,
current engine source inventory and source ZIP, real build commands/logs,
compiler/tool hashes, dependency libraries, actual PE headers/imports/exports,
and the 39-case independent English integration run with inventory and JUnit.
The final engine build was incremental and recompiled zero objects. This update
is not a from-zero reproducibility claim, runner authorization, data approval or
public release authorization. The other 21 lock entries retain their prior pins.

`verify_engine_provenance.ps1 -RepoRoot <checkout> -InputRoot <cache>` additionally
compares current engine inputs, receipt, DLL and all provenance references.
Both workspace and immutable-cache copies are checked before acceptance. The
cache must be outside the workflow checkout, preprovisioned by an authorized
maintainer, and protected against concurrent writes. The workflow does not copy
or bless arbitrary cache contents. Local content-addressed recovery and read-only
file attributes are useful integrity controls, not proof of an administratively
protected remote cache.

Local recovery produced
`build/go-input-cache-3d089a3c0b7fffce4e102e23d0e0373b4df736cda63810f1bb79471905be67c3/`.
Its directory suffix is the exact updated lock SHA-256. Both it and the current
workspace independently passed all 22 lock entries. The old cache is retained.
To provision a runner, copy this reviewed snapshot to an unused external cache
path with the same lock identity; provision its mirrored inputs and provenance
files into a fresh checkout; rerun both verifiers before any build. Backend and
product also need their documented toolchains/dependency build inputs: the 22
entry lock alone does not constitute a complete toolchain cache.

Runner settings are independently provisioned:

- `EPOCANVA_CI_RUNNER_NAME`: exact designated runner name.
- `EPOCANVA_CI_INPUT_CACHE`: external protected input snapshot.
- `EPOCANVA_CI_CMAKE_SHA256`, `EPOCANVA_CI_CTEST_SHA256`: reviewed tool binaries.
- `EPOCANVA_CI_PLAN_PATH`, `EPOCANVA_CI_PLAN_SHA256`: independently reviewed
  acceptance plan outside the checkout. This must implement the required real
  host, installer, approved quality and interaction protocols; no substitute
  plan or approval is generated here.

## Producers, artifacts and consumers

`record_hosted_tests.ps1` captures each hosted layer's actual selected CTest
inventory, serial test execution, absolute JUnit path and real native exit
codes. `verify_root_junit.py` requires exact registered/executed names and zero
failures/errors/skips. The existing historical relative-JUnit regression remains
an executable real-CTest test, now also checking the shared hosted producer.

`run_acceptance_layer.ps1` builds the backend, then the product and installer,
then executes the authorized integration and quality plans serially. The full
product build producer supplies its own complete registered suite and verified
receipt. Each layer checks current source/inputs, actual predecessor files,
frozen product hashes and the appropriate release-checker gates. A partial
release-check result may exit 1 while that layer's required checks pass; its
immutable snapshot is parsed again by the consumer. The final release checker
still requires **all** technical gates. Missing approvals are never waived.

Layer summaries use `format: epocanva-windows-ci-layer-v2`:

- full `source_commit`, string `run_id` and `run_attempt` (GitHub environment wire
  representation), `layer`, `runner_name`, workflow and input-lock SHA-256;
- UTC start/end, `status`, integer top-level `exit_code`, and an `exit_codes`
  object of observed native process exits; absent/null/bool/float exits fail;
- `outputs` references `{path, sha256, bytes}` for original inventory, JUnit,
  logs, immutable release-check snapshots and predecessors;
- for product/host/quality, actual package/receipt/source archive/installer and
  executable/DLL references, plus matching `product` digests.

`collect_layer_artifact.ps1` copies only hash-bound files (including referenced
predecessors) into an unused artifact directory and verifies the copies.
Every required job uploads with `if: always()` and `if-no-files-found: error`.
Artifact names are `<layer>-<candidate SHA>-<run ID>-<attempt>`. The artifact
contains `summary.json` plus `payload/<repo-relative reference path>`; restore
payload files at their recorded relative paths for offline inspection. Nothing
in a summary turns a failed GitHub job into a success.

## Final CI provenance contract

The CI branch of `release_verifiers/host_report.ps1` calls the independently
exercised `tools/ci/ci_provenance.ps1`. Required GitHub run/jobs/artifact API
snapshots must be retrieved through a trusted operator path, including all
pagination and the exact run attempt. The consumer performs no network I/O.
It rejects string/float/bool numeric API IDs, incomplete counts, duplicate job
IDs, old candidate/run/attempt, non-dispatch runs, wrong workflow, any missing,
failed, skipped or cancelled required job/step, and wrong runner IDs/groups/labels.
The non-gating canary cannot substitute for a required name.

`artifact_manifest` references a JSON object with:

- `format: epocanva-ci-artifacts-v1`, `repository`, integer `run_id` and
  `run_attempt`, `source_commit`;
- `github_artifacts`: reference to the complete API artifact snapshot;
- exactly six `entries`: `job`, integer `job_id` and `artifact_id`, `archive`
  reference, `summary` reference and `summary_archive_path`.

Each archive must match the GitHub artifact digest and byte count, exact
candidate/run/attempt name and workflow-run linkage, and be unexpired. The
consumer reads the ZIP without extraction, rejects unsafe/duplicate/link paths,
and checks that the referenced summary is exactly the member in that archive.
It reopens all bound files, compares inventory to JUnit, checks actual process
exits and required gate snapshots, and binds all product layers to the same
package and installer under acceptance. Rehashing an outer report cannot bless
a replaced inner file or a mismatched API artifact digest.

`runner_authorization` references `epocanva-ci-runner-authorization-v1` with the
same repository/candidate/integer run ID/attempt, `decision:
approved-isolated-acceptance`, `owner`, `record_id`, actual integer `runner_id`
and `runner_group_id`, exact `runner_name` and unique `runner_label`,
`environment_id`, `snapshot_restore_policy`, and a hashed independent `record`.
These fields are checked for consistency, **not authenticated by their presence**.
A fabricated authorization and fabricated API snapshots remain fabricated even
if their internal hashes agree. Owner review of record authority and trusted
retrieval is still required; tests explicitly label all fixtures synthetic.

## Completion and handoff

A clean candidate SHA, protected authorized runner, reviewed cache/toolchain,
real approved test plan and successful same-attempt six-layer run are still
required. The current dirty working-tree local evidence cannot close that gate.
After source/line-ending freeze, the main agent creates the candidate, performs
serial complete root/backend/product regression, and alone pushes/dispatches.
Retrieve run, attempt jobs, artifacts and authority records; reconstruct and
verify the archive payload; run the actual internal release checker. Public
release and full Windows/Android product GO remain distinct decisions.

## Native Git configuration transport (local CTest regression)

On the installed CTest 3.31.6-msvc6, sequential tests with ENVIRONMENT properties
lose inherited empty environment entries after the first test. The minimal
three-test reproduction is preserved in the audit evidence. This is not fixed
by deleting `GIT_CONFIG_COUNT`, removing credential-helper resets, or guessing a
missing value in a later child process.

Start CTest from an intact parent using:

```text
python -B tools/ci/preserve_git_environment.py -- <ctest.exe> <original arguments>
```

The wrapper carries the complete indexed Git configuration into the nonempty
`GIT_CONFIG_PARAMETERS` transport, preserving empty values, exact quoting and
order. Indexed entries precede existing PARAMETERS, as verified against
Git-for-Windows v2.52.0.windows.1 `config.c:711-766`. It sets the indexed count to
zero only in the child transport, leaving those entries and the parent untouched;
all effective settings remain present. Configuration values are never logged,
persisted or placed in argv. Already missing entries fail rather than being
recreated. Real Git comparisons and three sequential CTests exercise the result,
and a child exit 8 remains exit 8. The workflow's hosted/backend CTest and product
script launches use this wrapper. Final local complete suites must also use it.

The product job additionally runs the artifact-dependent installer contract
suite after package creation:

```text
python -B tests/installer/test_static_contract.py --build-dir <candidate> --junit <evidence>/installer-contract.junit.xml -v
```

Its actual exit and passing JUnit are mandatory product-layer evidence. It is
not discovered in the hosted root suite, which lacks installer build inputs;
no assertion or required package test is skipped by this separation.
