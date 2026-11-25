# Editor reference-model / stress evidence audit — 2026-11-25

## Scope and status

This is the delegated editor/stress workstream, not a whole-product GO decision.
All code changes below are frozen for the main agent's full regression. No commit,
push, installation, TSF registration, system change or approval was performed.
Branch: `codex/go-sprint-20261123`; starting HEAD:
`3401bf40c8701032fcf08cb8b886f2d17b50a71e`. The shared branch remains dirty.

**Development verification: PASS. Formal stress acceptance: BLOCKED.**
The real resource policy is `pending_product_owner`; no absolute threshold was
invented. A clean candidate's product receipt/package are still required. Windows
internal/public and cross-platform GO are not established by this workstream.

All work used `D:/EpoGO`. Outputs are isolated in `build/go-stress-20261125`;
no other agent's build output was used or modified. The five pre-existing scoped
files exactly matched the sealed `build/go-audit-20261125` snapshots by SHA-256
before editing (`scope-baseline.json`). Existing CMake test registration was
reviewed and preserved without further changes.

## Environment and source contracts

- Windows 10.0.26200.0, x64; MSVC 19.44.35209.0, Windows SDK 10.0.26100.0.
- Visual Studio 17 2022 generator, C++17, extensions OFF, `/permissive-`.
- Python 3.14.3; real Windows PowerShell 5.1 and bundled pwsh 7 executions.
- `cmake` / `ctest` were absent from PATH (`Get-Command python,cmake,ctest,pwsh,powershell`, exit 1).
  Their installed Visual Studio paths were read from the existing independent
  CMake cache and used explicitly. No tools were installed.
- Read AGENTS, product requirements, release-gate, ADR 0002/0003,
  `docs/windows/tsf-edit-contract.md`, acceptance audit/gap reports, and the
  existing 2026-11-22 claims. Prior report statuses were not treated as evidence.
- Verified implementation boundaries in `platforms/windows/tsf_edit/edit_plan.cc`
  (`BuildBegin` / `BuildCommit` reject noncollapsed UTF-16 selections),
  `edit_session.h` / `.cc` (`Invalidate`, `Bind`, `Snapshot`),
  `tests/windows/tsf_edit/test_host.cc` (actual host edit sessions), and the
  current acceptance identity/receipt helpers. No upstream behavior is inferred
  from a blog or this report alone.

## Files changed in this workstream

| Path | Change and reason | Verification |
| --- | --- | --- |
| `tests/editor_reference_model/main.cc` | Preserve the scalar oracle and real TSF store; check composition UTF-16 start/extent, reject both noncollapsed plans and host selections, cover five invalid UTF-8 forms, reject old-generation resubmission after rebind, preserve replay before a run can throw | Real Release CTest and both producers |
| `tests/editor_reference_model/test_stress_contract.py` | Expand from 12 to 37 positive/negative tests; real exclusive Windows file lock; current identity helper refusal; source/package/JUnit/resource mutations | 37 passed inside CTest |
| `tests/editor_reference_model/resource-policy.json` | Explicit source-controlled pending resource policy; all four limits null; approval null | Pending policy remains BLOCKED; synthetic fixtures alone exercise approved/over-limit branches |
| `tools/product/run_stress_reference_model.ps1` | Native execution JUnit, expected and real exits, execution source snapshot and post-run recheck, independent raw validation before writing acceptance results, resource assessment, formal blocking | Windows PowerShell 5.1 and pwsh 7 exit 0 in Development mode |
| `tools/product/verify_stress_reference_model.py` | Reopen raw files, exact CTest inventory/counts, native JUnit/elapsed time, source identity, package payload/binaries, current source/engine/build helper, current resource policy and recomputed metrics | 37 contract tests; real consumer runs |
| `docs/reports/go-audit-stress-20261125.md` | This scoped audit and frozen handoff | File/diff checks and evidence manifest |

Commit SHA for these modifications: **none, as instructed**. The inherited
uncommitted CMake change registering `evidence_contract` remains intact.
No other `tools/product` files were changed by this workstream.

## Independently reproduced defects and fixes

The sealed original verifier and original fixture were executed without editing
the snapshot. `sealed-baseline-reproductions.json` records three accepted bad
synthetic chains: a JUnit declaring 999 tests despite only five cases, an extra
registered required case that was never executed, and sampled private memory of
10^12 bytes. These are mutation fixtures, not product measurements or approvals.

| ID / severity | Root cause / location | Resolution |
| --- | --- | --- |
| STRESS-01 / P1 | `verify_receipt` checked only the editor subset, not whole inventory or JUnit/receipt totals | Require unique exact registered/executed sets, exact root count, zero skips/errors/failures, matching typed receipt counts |
| STRESS-02 / P1 | `verify_resources` checked handles/cleanup but implied formal resource success without a memory policy | Separate structural resource checks from current approved absolute policy; pending policy blocks formal evidence; recompute all metrics and reject rehashed false assessments |
| STRESS-03 / P1 | Mutable report/receipt could agree without revalidating current source; raw execution lacked source binding | Capture execution source and repository fingerprints, recheck after execution; formal standalone CLI reruns the shared read-only current source/engine/build identity verifier |
| STRESS-04 / P1 | Package binding stopped at manifest identity; native command log had no independent execution JUnit | Reopen every manifest payload, enforce exact payload inventory and three receipt binary digests; emit and revalidate native JUnit with actual exits, times and executable hash |
| STRESS-05 / P1 | Noncollapsed rejection tested only Begin-plan construction; old generation and composition range assertions incomplete | Exercise Begin/Commit refusal and executor refusal with a noncollapsed host selection; reapply old plan after rebind; compare UTF-16 composition extent each operation |
| STRESS-06 / P2 | Seed replay was written after execution; an assertion/infrastructure exception could lose reproducible input | Write deterministic seed replay before running; preserve failed measurement before attempting minimization |

Before: measurement/log -> report with successful resource case -> partial
receipt checks. After: native exits/logs/JUnit -> strict raw measurement/replay/
cleanup validation -> recomputed resource assessment -> full receipt inventory,
current source/engine identity and package contents -> formal entry only on pass.

Rejected alternatives: weakening oracle assertions, skipping native failures,
equating developer memory samples with approved performance, sharing another
agent's build directory, replacing the scalar oracle with the implementation,
or modifying production selection semantics outside the assigned write scope.

## Actual verification and evidence

Exact command argument arrays, UTC intervals and observed native exit codes are
in `build/go-stress-20261125/commands.jsonl`. Each step has its raw `.log`;
`ctest-inventory.log` is raw CTest JSON. Earlier logs/results remain preserved.

| Executed step | Actual result / exit |
| --- | --- |
| `baseline-contract` | Original 12 tests passed / 0 |
| `configure` | Root source configured in independent build directory / 0 |
| `build` | Release `epocanva_editor_reference_tests` target built / 0 |
| `ctest-inventory` | Five editor tests registered / 0 |
| `freeze-ctest-editor-release` | Five registered, five executed/passed, zero failed/errors/skipped / 0 |
| `editor.reference_model.evidence_contract` inside that CTest | 37 tests passed, including real locked-file and current helper negative / 0 |
| `freeze-powershell51-development` | Five seeds ×100000, zero mismatches, raw consumer verified / 0 |
| `freeze-pwsh7-development` | Five seeds ×100000, zero mismatches, raw consumer verified / 0 |
| Each producer's stress / self-test / replay-mutant / force-fail | Actual native exits 0 / 0 / 1 / 1; last two are required negative controls |
| `sealed-baseline-reproductions` | Three old-consumer vulnerabilities reproduced using sealed synthetic fixtures / 0 |
| `freeze-whitespace`, `freeze-inventory-check` | Scoped diff and exact registered/executed name set check / 0 |

Final CTest JUnit: `build/go-stress-20261125/freeze-ctest-editor-release.xml`.
Native executable:
`build/go-stress-20261125/tests/editor_reference_model/Release/epocanva_editor_reference_tests.exe`,
169984 bytes, SHA-256
`da607a2cc342cdd791128aaceac33f332b45578972f8009c71a413c53e3b23fd`.

Latest native evidence directories (under `build/go-stress-20261125/stress-reference-model/`):

- PowerShell 5.1: `run-20261125T064626331Z-77d0d1daa2944d428fde773da23f7cfc`.
- pwsh 7: `run-20261125T064723326Z-053bf19bda774b368be80c5a6dcab29c`.

Each contains `commands.json`, `native.junit.xml`, four native process logs,
five deterministic replay files, original/minimal mutation replay,
`measurements.json`, `negative-control.json`, `mutant-replay.json`,
`replay-log.json`, `resource-samples.json`, and `resource-assessment.json`.
Every seed has baseline, each 10000 operations, and post-teardown samples.

Latest pwsh run: each seed retained 159 -> 159 process handles, zero surviving
stores/sinks. Peak working set 15740928 bytes; maximum sampled private bytes
3641344; greatest post-seed working-set growth 163840; private growth 147456.
These are observations, **not approved limits or performance acceptance**.
`peak_private_bytes` is the maximum sampled private usage, not a continuous
OS high-water mark. Working-set peak uses the OS process high-water counter.
Memory growth is the maximum of all five post-teardown samples above the warmed
first baseline, so a middle-seed spike cannot be hidden by a smaller final sample.

The two producer runs captured different dirty source fingerprints because other
authorized workstreams were active. Each run checked its own source snapshot
before/after execution. Neither is a unified frozen-candidate receipt, and the
execution source snapshot alone is not binary build provenance.

## Exact handoff commands

The main agent must use its **actual final product BuildDir**, a direct child of
`D:/EpoGO/build/`, containing the verified Release receipt, full CTest inventory/
JUnit and staged package. Do not use this workstream's root-only build as a
formal product build. The example below uses the product builder's default
directory; change only `$candidateBuild` if the main chain uses another preset.

```powershell
$repo = 'D:/EpoGO'
$candidateBuild = 'D:/EpoGO/build/windows-product'
$version = (Get-Content -LiteralPath "$repo/platforms/windows/product/VERSION" -Raw).Trim()
$stage = Join-Path $candidateBuild "stage/EpoCanva-$version"
$harness = Join-Path $candidateBuild 'editor_reference_model_tests/Release/epocanva_editor_reference_tests.exe'
powershell.exe -NoProfile -NonInteractive -File "$repo/tools/product/run_stress_reference_model.ps1" `
  -RepoRoot $repo -BuildDir $candidateBuild -StageRoot $stage `
  -EvidencePath (Join-Path $candidateBuild 'release-evidence.json') -Harness $harness
$stressExit = $LASTEXITCODE
```

`pwsh.exe` can replace `powershell.exe`; both were tested. Product harness path
is defined by `platforms/windows/engine_bridge/CMakeLists.txt:297`. The producer
also discovers it automatically if `-Harness` is omitted. The five registered
test names are `editor.reference_model.stress`, `.replay_minimization`,
`.evidence_contract`, `.can_fail`, `.report_unwritable`.

Targeted product precheck (does not replace the full product suite):

```powershell
ctest --test-dir $candidateBuild -C Release -R '^editor\.reference_model\.' `
  --no-tests=error --output-on-failure --output-junit "$candidateBuild/editor-focused.xml"
```

Standalone consumer, where `$run` is the exact unique directory printed by the
producer (not an arbitrary older directory):

```powershell
python -B "$repo/tools/product/verify_stress_reference_model.py" `
  --repo $repo --build-dir $candidateBuild --stage-root $stage --runner $harness `
  --log "$run/commands.json" --report "$run/stress-reference-model.json" `
  --junit "$run/stress-reference-model.junit.xml"
```

Development-only measurement in the independent workstream build:

```powershell
powershell.exe -NoProfile -NonInteractive -File D:/EpoGO/tools/product/run_stress_reference_model.ps1 `
  -RepoRoot D:/EpoGO -BuildDir D:/EpoGO/build/go-stress-20261125 -Development
```

Development returns 0 for successful structural measurement and explicitly
prints formal resource status. It emits no formal acceptance entry. Formal mode
requires a clean SHA/current verified product receipt. With those prerequisites
satisfied but resource approval still pending, it preserves measured evidence,
writes a blocked formal report/failing `resource-growth` JUnit case, exits 1 and
does **not** add a passing entry. There is no skip or threshold-override switch.

## Resource approval and residual blockers

| Blocker | Engineering / external | Current evidence and completion condition |
| --- | --- | --- |
| STRESS_RESOURCE_APPROVAL_REQUIRED | External owner decision, implementation ready to consume reviewed policy | Real source policy pending with null limits/approval. Obtain actual reviewed owner record and integer byte limits for peak working set, sampled private maximum, working-set growth and private growth; commit them with the candidate before building. Do not derive thresholds automatically from these measurements. |
| STRESS_FROZEN_PRODUCT_REQUIRED | Main-chain engineering | This scoped checkout is dirty; no final product receipt/package produced here. Freeze all workstreams, build and pass full product Release CTest, stage exact package, then run formal producer above. |
| STRESS_FORMAL_POSITIVE_NOT_RUN | Depends on preceding two | Positive structural chain is synthetic only. The real current-identity negative ran; a complete real formal success was not run. Rerun producer and current release consumer at the final fixed candidate. |
| EDITOR_SELECTION_REPLACEMENT_UNIMPLEMENTED | Product capability, outside assigned write scope | Noncollapsed selection replacement remains refused; complete text/selection preservation is checked. Do not call this complete general editor selection support. |
| INSTALLED_HOST_NOT_COVERED | Authorized host workstream | Controlled real TSF store is not installed Notepad/Chromium/Word, host undo or 1800-second interaction soak. |

The policy approval reference is source-controlled and byte-checked, but JSON
owner/record strings do not authenticate an approver. Human review and the
release process must validate authority. Approved resource fixtures exist only
in temporary test directories and grant no real approval. Handle/store cleanup
checks have no user-tunable budget; they remain mandatory in development too.

Other model limits: composition range is observed from the production session's
diagnostics; no claim of an independent TSF composition enumeration is made.
Undo is application-owned controlled editing; cursor/selection boundaries are
UTF-16 code units at Unicode scalar boundaries, not grapheme clusters. Replay
minimization handles observed document/selection/composition mismatches;
infrastructure/assertion exceptions retain the full input replay and fail
nonzero rather than being treated as a successful reduced semantic mismatch.

Windows runtime network restrictions are architectural (ordinary in-process TSF
DLLs have no OS-enforced network prohibition). Android can enforce no network
permission by omitting `android.permission.INTERNET`; Android is not tested here.

Shortest remaining dependency path: owner policy review may proceed in parallel
with main regression; once all source/policy changes are frozen, build full
product -> verified receipt -> exact stage -> real formal stress producer ->
release checker. Any subsequent source/policy/package change requires rebuilding
and rebinding evidence. Old dirty-run measurements must not be relabeled.
