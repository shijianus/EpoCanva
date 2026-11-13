# Record the verified MSI as the release evidence installer reference.
#
# The gate reads evidence.installer as {path, sha256, bytes} and checks it
# against the current candidate. This script only binds an MSI that was built
# from this exact staged package manifest; it never installs or signs anything.
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$BuildDir = '',
    [string]$StageRoot = '',
    [string]$EvidencePath = '',
    [string]$InstallerManifest = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Fail([string]$Message, [int]$Code = 1) {
    Write-Host "FAILED: $Message" -ForegroundColor Red
    exit $Code
}
if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
$RepoRoot = (Get-Item -LiteralPath $RepoRoot).FullName
$buildBase = [IO.Path]::GetFullPath((Join-Path $RepoRoot 'build'))
if (-not $BuildDir) { $BuildDir = Join-Path $buildBase 'windows-product' }
$BuildDir = [IO.Path]::GetFullPath($BuildDir).TrimEnd('\', '/')
$version = (Get-Content -LiteralPath (Join-Path $RepoRoot 'platforms\windows\product\VERSION') -Raw -Encoding UTF8).Trim()
if (-not $StageRoot) { $StageRoot = Join-Path $BuildDir "stage\EpoCanva-$version" }
$StageRoot = [IO.Path]::GetFullPath($StageRoot).TrimEnd('\', '/')
if (-not $EvidencePath) { $EvidencePath = Join-Path $BuildDir 'release-evidence.json' }
$EvidencePath = [IO.Path]::GetFullPath($EvidencePath)
if (-not $InstallerManifest) { $InstallerManifest = Join-Path $BuildDir 'installer\installer-manifest.json' }
$InstallerManifest = [IO.Path]::GetFullPath($InstallerManifest)

. (Join-Path $PSScriptRoot 'acceptance_evidence.ps1')
$identity = Get-AcceptanceIdentity -RepoRoot $RepoRoot -BuildDir $BuildDir -StageRoot $StageRoot
if (-not (Test-Path -LiteralPath $InstallerManifest -PathType Leaf)) {
    Fail 'installer-manifest.json is missing; run tools/installer/build_msi.ps1' 3
}
$manifest = Get-Content -LiteralPath $InstallerManifest -Raw -Encoding UTF8 | ConvertFrom-Json
if ($manifest.format -ne 1) { Fail 'Unsupported installer manifest format' 4 }
if ($manifest.package_manifest_sha256 -cne $identity.package_manifest_sha256) {
    Fail 'The installer was built from a different package manifest' 4
}
if ($manifest.destination_commit -cne $identity.commit -or
        $manifest.acceptance_profile -cne $identity.acceptance_profile -or
        $manifest.distribution_policy -cne $identity.distribution_policy) {
    Fail 'The installer manifest identity differs from the current candidate' 4
}
$installerPath = [IO.Path]::GetFullPath([string]$manifest.installer.path)
if ((Split-Path -Parent $installerPath) -ne (Join-Path $BuildDir 'installer')) {
    Fail 'The installer must be the build-directory installer output' 4
}
$installerFile = Get-Item -LiteralPath $installerPath -Force
$installerHash = Get-AcceptanceSha256 $installerPath
if ($installerHash -cne [string]$manifest.installer.sha256 -or
        $installerFile.Length -ne [long]$manifest.installer.bytes) {
    Fail 'The installer artifact changed after its manifest was written' 4
}
if ([IO.Path]::GetExtension($installerPath) -notin @('.exe', '.msi', '.msix', '.msixbundle')) {
    Fail 'Unsupported installer artifact type' 4
}

$document = Get-Content -LiteralPath $EvidencePath -Raw -Encoding UTF8 | ConvertFrom-Json
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
    path = $installerPath.Substring($RepoRoot.Length + 1).Replace('\', '/')
    sha256 = $installerHash
    bytes = [long]$installerFile.Length
}) -Force
[IO.File]::WriteAllText($EvidencePath, (($document | ConvertTo-Json -Depth 12) + "`n"),
    (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Recorded installer $installerPath"
Write-Host "SHA-256: $installerHash ($($installerFile.Length) bytes)"
exit 0
