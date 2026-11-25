param([Parameter(Mandatory)][string]$Fixture)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $RepoRoot 'tools/product/build_identity.ps1')
. (Join-Path $RepoRoot 'tools/product/release_verifiers/host_report.ps1')
function Read-Reference($Reference) {
    Assert-ProductObjectFields $Reference @('path', 'sha256', 'bytes')
    Assert-ProductRelativePath $Reference.path
    $full = Assert-ProductPlainPath (Join-Path $RepoRoot $Reference.path) $RepoRoot
    if (-not (Test-ProductJsonInteger $Reference.bytes) -or $Reference.bytes -le 0 -or
            (Get-Item -LiteralPath $full).Length -ne $Reference.bytes -or
            (Get-ProductIdentityFileSha256 $full) -cne $Reference.sha256) { throw 'Fixture reference mismatch.' }
    return $full
}
try {
    $report = Read-ProductJson $Fixture
    $bindings = @{ commit = ('a' * 40); package_manifest_sha256 = $report.package_manifest_sha256;
        installer_sha256 = $report.installer_sha256 }
    $entry = @{ started_utc = $report.started_utc; completed_utc = $report.completed_utc }
    $null = Test-HardAcceptanceReport 'acceptance-ci-provenance' $report $entry $bindings
    Write-Output 'SYNTHETIC CONTRACT ACCEPTED; NOT RELEASE EVIDENCE'
    exit 0
} catch {
    Write-Output ('SYNTHETIC CONTRACT REJECTED: ' + $_.Exception.Message)
    exit 1
}
