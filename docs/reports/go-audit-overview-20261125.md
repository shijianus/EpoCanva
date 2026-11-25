# Independent GO sprint audit — 2026-11-25

Status at source freeze: **BLOCKED_NOT_COMPLETE**. Windows internal technical,
Windows public release and full cross-platform product remain **NO-GO**.
This is a pre-freeze engineering record, not a successful release approval.
The subsequent actual candidate SHA, complete regressions, remote run URLs,
artifacts and checker output are recorded in
`build/go-audit-20261125/final-result.json` and its referenced raw files.

## Baseline and preservation

The fetched/local baseline is `3401bf40c8701032fcf08cb8b886f2d17b50a71e`, not
the task's older `80cb0bbdd78825036e062416137fb61a06998734`. The original
`feat/internal-acceptance-round` checkout was preserved. Development uses the
independent `codex/go-sprint-20261123` worktree. Explicit task authorization
permits tested commits, branch push and a draft PR; it does not authorize main
merge, public tags, signing keys, system registration or installation.

All 25 historical changed-source snapshot hashes and all 277 evidence entries
were reopened and verified. The historical patch covers 18 paths; another seven
have snapshot bodies. Historical patch SHA-256:
`91811f2a13d4ee9b3d6ae423a6c7ccb7dd5111077037c33d462373d05e25b610`.
There is no SOURCE_TRANSFER_BLOCKED. The current independent checkout inherited
58 changed/new files; `initial-source-snapshot/`, `initial-working-tree.patch`
and `baseline.json` preserve that exact entry state. Neither index JSON was
mistaken for source. The final delivery includes a new patch and source snapshot.

## Environment and scope

Windows x64 build 26200, SDK 10.0.26100.0, VS 2022 Professional,
MSVC 19.44.35209.0, CMake/CTest 3.31.6-msvc6, Python 3.14.3,
Windows PowerShell 5.1 and PowerShell 7.6.9. C++17, extensions disabled,
`/permissive-`, Release product and static CRT were checked. WiX 6.0.2 already
exists in the build tree; no missing tool was installed. `where.exe gh` returned
1; Git and the GitHub API are available instead. Machine-specific executable
paths remain in local command evidence, not committed build configuration.

Read AGENTS, requirements, release-gate, Windows focus/edit/lifetime contracts,
CI documentation and the applicable ADRs. Module reports below identify the
actual SDK headers, pinned native sources and Microsoft interface text used.
No runtime networking or user-content logging was introduced. Ordinary Windows
TSF DLL offline behavior is architectural; Android can enforce no INTERNET
permission, but no Android implementation/device result was produced here.

## Observed verification before final freeze

| Actual execution | Result | Actual exit |
| --- | --- | --- |
| Historical source/evidence verification | 25/25 sources, 277/277 entries | 0 |
| Controlled real focus, user-assisted foreground/button | 67/67 assertions; real callback entries/exits 21/21; no poll changes | 0 |
| First full product Release | 260/264 passing, four failures, zero skips | 8 |
| Latest pre-freeze backend Release | 61/61, zero failures/skips | 0 |
| Latest pre-freeze root Debug | 107/108, pilot P95 failed, zero skips | 8 |
| Isolated serial pilot cost check, unchanged threshold | 1/1 | 0 |
| Complete receipt mutation script | Real 267-test contract + schema/hash/process/JUnit/binary negatives; 174 seconds | 0 |
| Actual registration contract, PowerShell 5.1 and 7 | 19 assertions each, 267 names/commands/properties | 0 / 0 |
| Quality evidence tests, PowerShell 5.1 and 7 | 86 assertions each | 0 / 0 |
| Python candidate evaluation contracts | 79 tests | 0 |
| Candidate evaluator in current product build | Two actual native tests | 0 |
| Release gate synthetic/negative suite | 150 assertions | 0 |
| Stage-product producer fixtures | Seven child executions, 23 assertions | 0 |
| Corresponding Source producer fixtures | 22 child executions | 0 |
| Installer helper new format-4 producer | Real build, one CTest / 68 policy assertions | 0 |
| Refreshed English build and independent review | 571 unchanged ABI exports, static CRT; identical DLL | 0 / 0 |
| Workspace / new immutable input snapshot | 22/22 each | 0 / 0 |
| Independent provenance consumer, workspace / snapshot | Current source, receipt and referenced bytes match | 0 / 0 |

These rows are not a claim that the final full suites passed. The first product
failures were foreground focus, two receipt fixtures and an obsolete release-gate
fixture. Failures and their original JUnit remain. The root P95 failure occurred
during concurrent load; its cause is not proved solely by a later isolated pass.
Final whole-suite verification is serial and retains the same performance budget.

Adding full registration/tamper coverage increased receipt validation from the
old wrapper's 120-second allowance to an observed 174 seconds. Only this I/O
contract suite now has 600-second child/630-second CTest timeouts. Product
latency thresholds, assertions, nonzero failures and zero-skip policy are intact.

## Audit records and reproducibility

`build/go-audit-20261125/commands.jsonl` and per-command JSON record exact argv,
working directory, UTC start/end, real child exit and original log path. An outer
PowerShell runner may display 1 for a child's 8 or 14; the ledger retains the
child value. Original failed runs are not deleted or relabeled.
`changed-files.json` names every delivered path and SHA-256; `source-snapshot/`
contains the files; `final-working-tree.patch` contains the reviewable diff.
`evidence-manifest.json` indexes actual original evidence bodies.

The engine was incrementally rebuilt, with zero objects recompiled in the final
identity-only refresh. This is not a clean-build reproducibility proof. The DLL
remains `fcd61cc5005517a6a57b893e9e73c8ca8ec38f08dd02ab5d41536ed8687437a9`.
Current engine input fingerprint is
`7f2630e875cae74d92c196154dcc54f0d5abba50cf935bf74e17f45d99ed4e42`.
The refreshed source/receipt snapshot was copied to a new cache; the prior cache
was not changed. DLL lock SHA was not edited to conceal this identity refresh.

Detailed scope reports: `go-audit-tsf-20261125.md`,
`go-audit-ci-20261125.md`, `go-audit-installer-20261125.md`,
`go-audit-helper-20261125.md`, `go-audit-stress-20261125.md`, and
`go-sprint-french-20261123.md`. Their earlier handoff results remain historical;
the main-chain evidence supplement records subsequent integration.
