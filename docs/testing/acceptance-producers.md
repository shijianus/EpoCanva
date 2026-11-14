# Acceptance evidence producers

These producers execute real tests and write entries into
`build/<preset>/release-evidence.json`. They are not owner approvals, signing
tools, or installers.

## Prerequisites

A verified product build and a staged format-2 package:

```powershell
powershell -NoProfile -File tools/product/build_product.ps1 -Configuration Release
powershell -NoProfile -File tools/product/stage_product.ps1 `
  -AcceptanceProfile internal-unsigned -DistributionPolicy internal-only -StrictPaths
powershell -NoProfile -File tools/product/record_package_diagnose.ps1
powershell -NoProfile -File tools/product/stage_source.ps1
```

`record_package_diagnose.ps1` creates the format-2 evidence document and binds
the source manifest. The runners below append entries to it.

## English capabilities

```powershell
powershell -NoProfile -File tools/product/run_english_acceptance.ps1
```

Executes six `--capability` cases against the real English core and one
`product_sink_tests --mode multi` case. Writes
`build/<preset>/english-acceptance/process.log`,
`english-acceptance.junit.xml` and `report.json`, then appends the
`english-capabilities` evidence entry. It exits nonzero on any failed case.

The cross-session case is not a real Notepad/Chrome/Word run. The separate
`real-host` gate is the only claim of real applications. Aggregate English
acceptance now also requires passing `real-host`, including its explicit
`english-cross-app-consistency` case; a successful producer execution alone is
not formal acceptance of all seven capabilities.

## Offline privacy

```powershell
powershell -NoProfile -File tools/product/run_offline_privacy.ps1
```

Runs `dumpbin /imports` on the three staged binaries, launches the product
privacy mode with a fresh canary, polls TCP/UDP endpoints for the process tree,
and scans stdout/stderr and the isolated user-data tree for the canary. Writes
the four exact gate cases and the `offline-privacy` evidence entry. A missing
`dumpbin` or network cmdlet fails the corresponding case; it is never skipped.
This is controlled-process development evidence. Endpoint polling can miss
short-lived events and does not establish installed-host privacy. The aggregate
`offline-privacy` gate also requires authorized real-host sensitive-field
acceptance, including applicable logs/temporary data and persistence lifecycle.

## Reference-model stress

```powershell
build/<preset>/reference_model_tests/<Config>/epocanva_reference_model_stress_tests.exe `
  --ops 100000 --report build/<preset>/reference-model-report.json
```

Five fixed seeds, 100,000 edits each. The negative control is
`--force-fail` (must exit 1). The CTest cases include
`composition.core.reference_model_stress` and
`composition.core.reference_model_can_fail`.

This suite covers Korean/Japanese composers and TSF EditPlan validation; it is
not an independent model of the full host document editor. It does not yet
provide minimized failing edit sequences or full document caret/selection/undo
state comparison. Do not translate its passing result into the stronger
`stress-reference-model` aggregate gate; that gate stays BLOCKED until a genuine
producer satisfies its complete contract.

## Installed-host soak and complete CI

`host-soak` and `acceptance-ci-provenance` are mandatory fail-closed release
gates. This checkout has no authorized installed-host soak producer or complete
acceptance-runner execution. Their required measured-report contracts are in
`docs/product/release-gate.md`. Missing authorization, absent reports, shortened
runs, incomplete CI layers and mismatched commits remain BLOCKED. No passing
reports or approvals are hand-created by these tools.

## Installer

```powershell
powershell -NoProfile -File tools/installer/build_helper.ps1
powershell -NoProfile -File tools/installer/build_msi.ps1
powershell -NoProfile -File tools/installer/record_installer.ps1
```

Builds the HKCU-scoped per-user MSI and verifies its payload by administrative
extraction. This does not install the MSI. `installer-lifecycle` and
`real-host` require an authorized isolation environment and remain separate.
