# Read-only acceptance/release gate. Never builds, stages, installs or starts a GUI.
# Missing technical evidence blocks either profile; only internal signing is not required.
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$BuildDir = '',
    [string]$StageRoot = '',
    [string]$RimeDll = '',
    [string]$EvidencePath = '',
    [ValidateSet('internal-unsigned', 'public-signed')]
    [string]$AcceptanceProfile = 'internal-unsigned'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'build_identity.ps1')
$sourceContractPath = Join-Path $PSScriptRoot 'source_contract.ps1'
$sourceContractLoaded = $false
if (Test-Path -LiteralPath $sourceContractPath -PathType Leaf) {
    . $sourceContractPath
    $sourceContractLoaded = $true
}

. (Join-Path $PSScriptRoot 'release_verifiers/common.ps1')
. (Join-Path $PSScriptRoot 'release_verifiers/host_report.ps1')
. (Join-Path $PSScriptRoot 'release_verifiers/candidate_quality.ps1')
# Validate the output boundary before creating anything. Invalid output paths exit 2.
try {
    if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
    $RepoRoot = (Get-Item -LiteralPath $RepoRoot).FullName
    $buildBase = [IO.Path]::GetFullPath((Join-Path $RepoRoot 'build'))
    if (-not $BuildDir) { $BuildDir = Join-Path $buildBase 'windows-product' }
    $BuildDir = [IO.Path]::GetFullPath($BuildDir).TrimEnd('\', '/')
    if ((Split-Path -Parent $BuildDir) -ne $buildBase -or
            (Split-Path -Leaf $BuildDir) -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$') {
        throw 'BuildDir must be exactly <repo>/build/<preset>.'
    }
    $outputPath = Join-Path $BuildDir 'release-readiness.json'
    Assert-PlainPath $outputPath $buildBase
    if (-not (Test-Path -LiteralPath $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null }
    $checkLock = [IO.File]::Open((Assert-ProductPlainPath (Join-Path $BuildDir 'release-check.lock') $buildBase),
        [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    Write-ProductJson $outputPath ([ordered]@{ format = 2; status = 'in_progress'; acceptance_profile = $AcceptanceProfile
        acceptance_ready = $false; technical_ready = $false; release_ready = $false; exit_code = $null })
} catch {
    Write-Error -ErrorAction Continue ('Release gate output unavailable: ' + $_.Exception.Message)
    exit 2
}

$checks = [System.Collections.Generic.List[object]]::new()
$source = $null
$receipt = $null
$receiptHash = ''
$version = ''
$policy = @{}
$policyHash = ''
$manifest = $null
$evidence = $null
$evidenceHash = ''
$licenseDecisionHash = ''
$languagePolicyHash = ''
$languagePolicyPath = Join-Path $RepoRoot 'data\manifests\language-readiness.json'
$sourceManifestHash = ''
$installer = $null
$references = [System.Collections.Generic.List[object]]::new()
$tools = [ordered]@{}
$receiptPath = Join-Path $BuildDir 'product-build-receipt.json'
$policyPath = Join-Path $RepoRoot 'data\manifests\product-package-files.inc'
if (-not $RimeDll) { $RimeDll = Join-Path $RepoRoot 'build\librime-english\rime\install\lib\rime.dll' }
if (-not $EvidencePath) { $EvidencePath = Join-Path $BuildDir 'release-evidence.json' }

. (Join-Path $PSScriptRoot 'release_verifiers/source_build.ps1')
. (Join-Path $PSScriptRoot 'release_verifiers/package.ps1')
. (Join-Path $PSScriptRoot 'release_verifiers/toolchain_inputs.ps1')
. (Join-Path $PSScriptRoot 'release_verifiers/release_evidence.ps1')
. (Join-Path $PSScriptRoot 'release_verifiers/corresponding_source.ps1')
. (Join-Path $PSScriptRoot 'release_verifiers/candidate_quality_gate.ps1')
. (Join-Path $PSScriptRoot 'release_verifiers/installer.ps1')
. (Join-Path $PSScriptRoot 'release_verifiers/host_acceptance.ps1')
. (Join-Path $PSScriptRoot 'release_verifiers/inputs_stable.ps1')
. (Join-Path $PSScriptRoot 'release_verifiers/release_policy.ps1')
try {
    Assert-PlainPath $outputPath $buildBase
    Write-ProductJson $outputPath $report
} catch {
    Write-Error -ErrorAction Continue ('Cannot write release-readiness.json: ' + $_.Exception.Message)
    exit 2
}
$checkLock.Dispose()
Write-Host "Release readiness: $($report.release_ready); blockers: $($blockers.Count)"
foreach ($blocker in $blockers) { Write-Host ("BLOCKED [{0}] {1}" -f $blocker.id, $blocker.detail) }
Write-Host "Report: $outputPath"
exit $exitCode
