# Produce the release-evidence record for a native package diagnosis.
#
# This script only runs the staged executable's headless `--diagnose` against an
# isolated user-data root below the build directory and writes the hash-bound
# evidence entry the release gate reads. It never registers, installs, activates,
# signs or touches a real user profile, and it refuses to reuse another candidate's
# receipt or package. It deliberately writes no owner, installer, host or signing
# entry: those require records this script cannot create.
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$BuildDir = '',
    [string]$StageRoot = '',
    [string]$EvidencePath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'acceptance_evidence.ps1')

function Fail([string]$Message, [int]$Code = 1) {
    Write-Host "FAILED: $Message" -ForegroundColor Red
    exit $Code
}
function Get-Sha256([string]$Path) {
    return Get-ProductIdentityFileSha256 $Path
}
function New-Reference([string]$Path, [string]$Boundary) {
    $full = [System.IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($Boundary.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar,
                              [System.StringComparison]::OrdinalIgnoreCase)) {
        Fail "evidence file is outside the build directory: $full" 2
    }
    $full = Assert-ProductPlainPath $full $Boundary
    $file = Get-Item -LiteralPath $full -Force
    if ($file.PSIsContainer -or $file.Length -eq 0) { Fail "evidence file is empty: $full" 2 }
    $relative = $full.Substring($script:repoRoot.Length + 1).Replace('\', '/')
    return [ordered]@{ path = $relative; sha256 = (Get-Sha256 $full); bytes = [long]$file.Length }
}
function Read-PackageMetadata([string]$Path) {
    $result = @{}
    foreach ($line in (Get-Content -LiteralPath $Path -Encoding UTF8)) {
        if ($line -cmatch '^(version|commit|acceptance-profile|distribution-policy|path-exception)=(.+)$') {
            if ($result.ContainsKey($Matches[1])) { Fail "duplicate package metadata: $($Matches[1])" 2 }
            $result[$Matches[1]] = $Matches[2]
        }
    }
    return $result
}

if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
$script:repoRoot = (Get-Item -LiteralPath $RepoRoot).FullName
$buildBase = [System.IO.Path]::GetFullPath((Join-Path $script:repoRoot 'build'))
if (-not $BuildDir) { $BuildDir = Join-Path $buildBase 'windows-product' }
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir).TrimEnd('\', '/')
if ((Split-Path -Parent $BuildDir) -ne $buildBase -or
        (Split-Path -Leaf $BuildDir) -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$') {
    Fail 'BuildDir must be exactly <repo>/build/<preset>' 2
}
$productVersion = (Get-Content -LiteralPath (Join-Path $script:repoRoot 'platforms\windows\product\VERSION') -Raw -Encoding UTF8).Trim()
if (-not $StageRoot) { $StageRoot = Join-Path $BuildDir "stage\EpoCanva-$productVersion" }
$StageRoot = [System.IO.Path]::GetFullPath($StageRoot).TrimEnd('\', '/')
$stageBase = [System.IO.Path]::GetFullPath((Join-Path $BuildDir 'stage'))
if ((Split-Path -Parent $StageRoot) -ne $stageBase) { Fail 'StageRoot must be directly below BuildDir/stage' 2 }
if (-not $EvidencePath) { $EvidencePath = Join-Path $BuildDir 'release-evidence.json' }
$EvidencePath = [System.IO.Path]::GetFullPath($EvidencePath)
if ((Split-Path -Parent $EvidencePath) -ne $BuildDir) { Fail 'EvidencePath must be inside BuildDir' 2 }

$receiptPath = Join-Path $BuildDir 'product-build-receipt.json'
if (-not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) { Fail 'product-build-receipt.json is missing; run build_product.ps1 with tests' 3 }
$identity = Get-AcceptanceIdentity -RepoRoot $script:repoRoot -BuildDir $BuildDir -StageRoot $StageRoot
$receipt = $identity.receipt
$current = Get-ProductSourceIdentity -RepoRoot $script:repoRoot
if ($receipt.source.commit -cne $current.commit -or
        $receipt.source.source_sha256 -cne $current.source_sha256 -or
        $receipt.source.head -cne $current.head) {
    Fail 'the current source differs from the tested build receipt; rebuild and retest' 3
}

$stageExe = Join-Path $StageRoot 'EpoCanva.exe'
$stageManifest = Join-Path $StageRoot 'package.manifest'
foreach ($path in @($stageExe, $stageManifest)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { Fail "staged file is missing: $path" 3 }
}
$metadata = Read-PackageMetadata $stageManifest
if ($metadata['commit'] -cne $current.commit) { Fail 'the staged commit differs from the current tested source' 4 }

$version = (Get-Content -LiteralPath (Join-Path $script:repoRoot 'platforms\windows\product\VERSION') -Raw -Encoding UTF8).Trim()
$reportPath = Assert-ProductPlainPath (Join-Path $BuildDir 'package-diagnose.txt') $BuildDir
$userRoot = Assert-ProductPlainPath (Join-Path $BuildDir 'diagnose-user') $BuildDir
$EvidencePath = Assert-ProductPlainPath $EvidencePath $BuildDir
$history = Assert-ProductPlainPath (Join-Path $BuildDir ('diagnose-history-' + [guid]::NewGuid().ToString('N'))) $BuildDir
foreach ($name in @('diagnose-user', 'package-diagnose.txt', 'diagnose-stdout.log', 'diagnose-stderr.log')) {
    $previous = Assert-ProductPlainPath (Join-Path $BuildDir $name) $BuildDir
    if (Test-Path -LiteralPath $previous) {
        $null = New-Item -ItemType Directory -Path $history -Force
        $destination = Assert-ProductPlainPath (Join-Path $history $name) $BuildDir
        Move-Item -LiteralPath $previous -Destination $destination -ErrorAction Stop
    }
}
New-Item -ItemType Directory -Path $userRoot -Force | Out-Null

$started = [DateTimeOffset]::UtcNow
# EpoCanva.exe is a GUI-subsystem binary: PowerShell's & returns immediately and
# the report would be hashed mid-write. Start-Process -Wait is required.
$diagnose = Start-Process -FilePath $stageExe `
    -ArgumentList @('--diagnose', ('"' + $reportPath + '"'), '--user-data-root', ('"' + $userRoot + '"')) `
    -Wait -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $BuildDir 'diagnose-stdout.log') `
    -RedirectStandardError (Join-Path $BuildDir 'diagnose-stderr.log')
$exitCode = $diagnose.ExitCode
$completed = [DateTimeOffset]::UtcNow
if ($exitCode -isnot [int] -or $exitCode -ne 0) { Fail "the diagnosis failed with exit $exitCode" 5 }
if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) { Fail 'the diagnosis wrote no report' 5 }

$entry = [ordered]@{
    id = 'package-diagnose'
    kind = 'diagnose'
    exit_code = [int]$exitCode
    started_utc = $started.ToString('o')
    completed_utc = $completed.ToString('o')
    command = @($stageExe, '--diagnose', $reportPath, '--user-data-root', $userRoot)
    report = New-Reference $reportPath $BuildDir
}
$document = [ordered]@{
    format = 2
    version = $version
    commit = $current.commit
    source_sha256 = $current.source_sha256
    package_manifest_sha256 = (Get-Sha256 $stageManifest)
    build_receipt_sha256 = (Get-Sha256 $receiptPath)
    package_policy_sha256 = (Get-Sha256 (Join-Path $script:repoRoot 'data\manifests\product-package-files.inc'))
    acceptance_profile = [string]$metadata['acceptance-profile']
    distribution_policy = [string]$metadata['distribution-policy']
    license_decision_sha256 = (Get-Sha256 (Join-Path $script:repoRoot 'data\manifests\license-decision.json'))
    language_policy_sha256 = (Get-Sha256 (Join-Path $script:repoRoot 'data\manifests\language-readiness.json'))
    entries = @($entry)
}
# Bind the Corresponding Source producer output. The release gate requires this
# top-level reference; without it a valid archive is unreachable. The manifest must
# belong to the same source/package as this diagnosis, and its archive bytes must
# match the manifest before the reference is recorded.
$sourceManifestPath = Join-Path $BuildDir 'source-manifest.json'
if (-not (Test-Path -LiteralPath $sourceManifestPath -PathType Leaf)) {
    Fail 'source-manifest.json is missing; run tools/product/stage_source.ps1 before recording release evidence' 5
}
$sourceManifest = Read-ProductJson (Assert-ProductPlainPath $sourceManifestPath $BuildDir)
if (-not (Test-ProductJsonInteger $sourceManifest.format) -or $sourceManifest.format -ne 1 -or $sourceManifest.commit -cne $current.commit -or
        $sourceManifest.source_sha256 -cne $current.source_sha256 -or
        $sourceManifest.package_manifest_sha256 -cne (Get-Sha256 $stageManifest)) {
    Fail 'source-manifest.json belongs to a different source or package' 5
}
$archiveRelative = $sourceManifest.archive.path
if ($archiveRelative -isnot [string]) { Fail 'source archive path is not a string' 5 }
Assert-ProductRelativePath $archiveRelative
$archivePath = Assert-ProductPlainPath (Join-Path $script:repoRoot $archiveRelative) $BuildDir
if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) { Fail 'the corresponding source archive is missing' 5 }
if (-not (Test-ProductSha256 $sourceManifest.archive.sha256) -or
        -not (Test-ProductJsonInteger $sourceManifest.archive.bytes) -or
        (Get-Sha256 $archivePath) -cne $sourceManifest.archive.sha256 -or
        (Get-Item -LiteralPath $archivePath -Force).Length -ne [long]$sourceManifest.archive.bytes) {
    Fail 'the corresponding source archive differs from its manifest' 5
}
$document['source_manifest'] = New-Reference $sourceManifestPath $BuildDir
$after = Get-AcceptanceIdentity -RepoRoot $script:repoRoot -BuildDir $BuildDir -StageRoot $StageRoot
foreach ($field in @('commit', 'source_sha256', 'package_manifest_sha256', 'build_receipt_sha256')) {
    if ($identity.$field -cne $after.$field) { Fail 'Candidate changed during package diagnosis' 5 }
}
Write-ProductJson $EvidencePath $document
Write-Host "wrote $EvidencePath"
Write-Host "package diagnosis: exit 0; report $reportPath"
exit 0
