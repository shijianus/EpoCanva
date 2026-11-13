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

function Fail([string]$Message, [int]$Code = 1) {
    Write-Host "FAILED: $Message" -ForegroundColor Red
    exit $Code
}
function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function New-Reference([string]$Path, [string]$Boundary) {
    $full = [System.IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($Boundary.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar,
                              [System.StringComparison]::OrdinalIgnoreCase)) {
        Fail "evidence file is outside the build directory: $full" 2
    }
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

. (Join-Path $PSScriptRoot 'build_identity.ps1')
$receiptPath = Join-Path $BuildDir 'product-build-receipt.json'
if (-not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) { Fail 'product-build-receipt.json is missing; run build_product.ps1 with tests' 3 }
$receipt = Get-Content -LiteralPath $receiptPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($receipt.format -ne 1 -or $receipt.status -ne 'verified') { Fail 'the build receipt is not a verified non-trace Release build' 3 }
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
$reportPath = Join-Path $BuildDir 'package-diagnose.txt'
$userRoot = Join-Path $BuildDir 'diagnose-user'
if (Test-Path -LiteralPath $userRoot) {
    # The isolated root is owned by this script and contains only diagnosis state.
    Remove-Item -LiteralPath $userRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $userRoot -Force | Out-Null

$started = (Get-Date).ToUniversalTime().ToString('o')
$previous = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    & $stageExe --diagnose $reportPath --user-data-root $userRoot
    $exitCode = $LASTEXITCODE
} finally { $ErrorActionPreference = $previous }
$completed = (Get-Date).ToUniversalTime().ToString('o')
if ($null -eq $exitCode) { Fail 'the diagnosis produced no process exit code' 5 }
if ($exitCode -ne 0) { Fail "the diagnosis failed with exit $exitCode" 5 }
if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) { Fail 'the diagnosis wrote no report' 5 }

$entry = [ordered]@{
    id = 'package-diagnose'
    kind = 'diagnose'
    exit_code = [int]$exitCode
    started_utc = $started
    completed_utc = $completed
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
$sourceManifest = Get-Content -LiteralPath $sourceManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($sourceManifest.format -ne 1 -or $sourceManifest.commit -cne $current.commit -or
        $sourceManifest.source_sha256 -cne $current.source_sha256 -or
        $sourceManifest.package_manifest_sha256 -cne (Get-Sha256 $stageManifest)) {
    Fail 'source-manifest.json belongs to a different source or package' 5
}
$archiveRelative = [string]$sourceManifest.archive.path
if (-not $archiveRelative -or $archiveRelative.Contains('..') -or
        $archiveRelative.StartsWith('/') -or $archiveRelative -match '^[A-Za-z]:') {
    Fail 'source-manifest.json archive path is not repository-relative' 5
}
$archivePath = Join-Path $script:repoRoot $archiveRelative.Replace('/', '\')
if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) { Fail 'the corresponding source archive is missing' 5 }
if ((Get-Sha256 $archivePath) -cne [string]$sourceManifest.archive.sha256 -or
        (Get-Item -LiteralPath $archivePath -Force).Length -ne [long]$sourceManifest.archive.bytes) {
    Fail 'the corresponding source archive differs from its manifest' 5
}
$document['source_manifest'] = New-Reference $sourceManifestPath $BuildDir
[System.IO.File]::WriteAllText($EvidencePath, ($document | ConvertTo-Json -Depth 8) + "`n",
    (New-Object System.Text.UTF8Encoding($false)))
Write-Host "wrote $EvidencePath"
Write-Host "package diagnosis: exit 0; report $reportPath"
exit 0
