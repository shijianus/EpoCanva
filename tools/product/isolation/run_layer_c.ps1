# EpoCanva W2-B-R -- W3 Layer C matrix runner template. NOT RUN IN THIS ROUND.
#
# This script performs no registration, no input-method switch and no UI automation by
# itself. It records the package identity and prints the manual matrix; each step must be
# confirmed by the operator on the authorized isolation machine. Extend the fixed-input
# steps in W3 only if an approved UI automation tool is available.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$StageDir,
    [Parameter(Mandatory = $true)][string]$EvidenceDir,
    [switch]$AuthorizedIsolationEnvironment
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $AuthorizedIsolationEnvironment) {
    throw 'Refusing to run the Layer C matrix: pass -AuthorizedIsolationEnvironment only on the authorized isolation machine.'
}
$exe = Join-Path $StageDir 'EpoCanva.exe'
$dll = Join-Path $StageDir 'epocanva_tsf.dll'
foreach ($path in @($exe, $dll)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing staged file: $path"
    }
}
New-Item -ItemType Directory -Path $EvidenceDir -Force | Out-Null
& (Join-Path $PSScriptRoot 'capture_state.ps1') -ReportPath (Join-Path $EvidenceDir 'before.txt') -DllPath $dll
$diagnose = Join-Path $EvidenceDir 'diagnose.txt'
& $exe --diagnose $diagnose
$exit = $LASTEXITCODE
$identity = Get-Content -LiteralPath $diagnose -Raw
Write-Host "diagnose exit=$exit"
Write-Host $identity

Write-Host ''
Write-Host 'Manual Layer C matrix (see docs/windows/isolation-verification-plan.md):'
Write-Host '  [ ] register_tip.ps1 + tip_profile_helper.exe register/activate'
Write-Host '  [ ] Notepad: fixed English token, Space commit, exact document'
Write-Host '  [ ] Candidate window at the real caret rectangle; click does not take focus'
Write-Host '  [ ] Enter/Escape/BackSpace; no late writes; host text preserved'
Write-Host '  [ ] 1202-unit capacity burst and same-session continuation; no crash'
Write-Host '  [ ] Deactivate/switch input method; no crash; unload gate refuses while callbacks live'
Write-Host '  [ ] 100/150/200% DPI and a small/multi-monitor work area'
Write-Host '  [ ] tip_profile_helper.exe deactivate/unregister + unregister_tip.ps1'
Write-Host '  [ ] capture_state.ps1 -ReportPath after.txt; compare registration snapshots'
Write-Host ''
Write-Host "Evidence directory: $EvidenceDir"
