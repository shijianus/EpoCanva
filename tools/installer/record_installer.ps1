[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$BuildDir = '',
    [string]$StageRoot = '',
    [string]$EvidencePath = '',
    [string]$InstallerManifest = '',
    [string]$BootstrapperManifest = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installer_contract.ps1')

function Fail([string]$Message, [int]$Code = 1) {
    Write-Host "FAILED: $Message" -ForegroundColor Red
    exit $Code
}
if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
$RepoRoot = (Get-Item -LiteralPath $RepoRoot).FullName
$BuildDir = Get-InstallerBuildDirectory $RepoRoot $BuildDir 'windows-product'
$installerDir = Assert-InstallerBuildPath (Join-Path $BuildDir 'installer') $BuildDir
$version = (Get-Content -LiteralPath (Join-Path $RepoRoot 'platforms\windows\product\VERSION') -Raw -Encoding UTF8).Trim()
if (-not $StageRoot) { $StageRoot = Join-Path $BuildDir "stage\EpoCanva-$version" }
$StageRoot = Assert-InstallerBuildPath $StageRoot $BuildDir
if (-not $EvidencePath) { $EvidencePath = Join-Path $BuildDir 'release-evidence.json' }
$EvidencePath = Assert-InstallerBuildPath $EvidencePath $BuildDir
if (-not $BootstrapperManifest) { $BootstrapperManifest = Join-Path $installerDir 'bootstrapper-manifest.json' }
$BootstrapperManifest = Assert-InstallerBuildPath $BootstrapperManifest $installerDir

. (Join-Path $PSScriptRoot '..\product\acceptance_evidence.ps1')
$identity = Get-AcceptanceIdentity -RepoRoot $RepoRoot -BuildDir $BuildDir -StageRoot $StageRoot
$bootstrapper = Read-ProductJson $BootstrapperManifest
$package = Assert-EpoCanvaInstallerPackage -BuildDir $BuildDir -ExePath $bootstrapper.bootstrapper.path `
    -ExpectedCommit $identity.commit -ExpectedPackageSha256 $identity.package_manifest_sha256 `
    -InstallerManifest $InstallerManifest -BootstrapperManifest $BootstrapperManifest
$manifest = $package.msi.manifest
if (-not (Test-InstallerInteger $manifest.package_manifest_bytes) -or
    $manifest.package_manifest_bytes -ne (Get-Item -LiteralPath (Join-Path $StageRoot 'package.manifest')).Length) {
    Fail 'The installer package manifest byte count differs from the staged package' 4
}
if ($manifest.destination_commit -cne $identity.commit -or
        $manifest.acceptance_profile -cne $identity.acceptance_profile -or
        $manifest.distribution_policy -cne $identity.distribution_policy) {
    Fail 'The installer manifest identity differs from the current candidate' 4
}
$helperPath = $manifest.helper.receipt_path
$helperReceipt = (Assert-InstallerHelperReceipt $RepoRoot $helperPath $manifest.helper.binary_path).receipt
if ($helperReceipt.format -ne 4 -or $helperReceipt.status -ne 'verified' -or
        (Get-InstallerFileIdentity $helperPath).sha256 -cne $manifest.helper.receipt_sha256 -or
        $manifest.helper.sha256 -cne $helperReceipt.binary.sha256 -or $manifest.helper.bytes -ne $helperReceipt.binary.bytes -or
        $manifest.helper.source_sha256 -cne $helperReceipt.source_sha256 -or
        $manifest.helper.toolchain_sha256 -cne $helperReceipt.toolchain_sha256 -or
        $manifest.helper.configuration_sha256 -cne $helperReceipt.configuration_sha256) {
    Fail 'The installer helper receipt does not match the recorded helper identity' 4
}
$payloadCount = @(Get-Content -LiteralPath (Join-Path $StageRoot 'package.manifest') -Encoding UTF8 |
    Where-Object { $_ -cmatch '^file\t' }).Count
if (-not (Test-InstallerInteger $manifest.installer.payload_files) -or $manifest.installer.payload_files -ne $payloadCount) {
    Fail 'The installer payload file count differs from the staged package manifest' 4
}
$document = Read-ProductJson $EvidencePath
$bindings = @{
    version = $identity.version
    commit = $identity.commit
    source_sha256 = $identity.source_sha256
    package_manifest_sha256 = $identity.package_manifest_sha256
    build_receipt_sha256 = $identity.build_receipt_sha256
    package_policy_sha256 = $identity.package_policy_sha256
    acceptance_profile = $identity.acceptance_profile
    distribution_policy = $identity.distribution_policy
    license_decision_sha256 = $identity.license_decision_sha256
    language_policy_sha256 = $identity.language_policy_sha256
}
foreach ($key in $bindings.Keys) {
    if ([string]$document.$key -cne [string]$bindings[$key]) {
        Fail "release-evidence.json identity mismatch: $key" 5
    }
}
$document | Add-Member -NotePropertyName installer -NotePropertyValue ([pscustomobject]@{
    path = $package.bootstrapper.path.Substring($RepoRoot.Length + 1).Replace('\', '/')
    sha256 = $package.bootstrapper.sha256
    bytes = $package.bootstrapper.bytes
}) -Force
$document | Add-Member -NotePropertyName bootstrapper_manifest -NotePropertyValue ([pscustomobject]@{
    path = $package.bootstrapper_manifest.path.Substring($RepoRoot.Length + 1).Replace('\', '/')
    sha256 = $package.bootstrapper_manifest.sha256
    bytes = $package.bootstrapper_manifest.bytes
}) -Force
Write-InstallerJson $EvidencePath $document
Write-Host "Recorded installer $($package.bootstrapper.path)"
Write-Host "SHA-256: $($package.bootstrapper.sha256) ($($package.bootstrapper.bytes) bytes)"
exit 0
