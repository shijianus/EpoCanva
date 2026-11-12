# Build a package only from the compiled product policy and a verified build receipt.
# Every source is checked before the existing stage is replaced. This script does no
# network I/O and never copies a build tree. Package integrity is not legal clearance.
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$StageRoot = '',
    [string]$BuildDir = '',
    [string]$ProductDataDir = '',
    [string]$RimeDll = '',
    [string]$BackendResourcesRoot = '',
    [ValidateSet('internal-unsigned', 'public-signed')]
    [string]$AcceptanceProfile = 'internal-unsigned',
    [ValidateSet('internal-only', 'public-candidate')]
    [string]$DistributionPolicy = 'internal-only',
    [switch]$Replace,
    [switch]$StrictPaths,
    [switch]$InternalAllowEmbeddedBuildPaths
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Fail([string]$Message, [int]$Code = 1) {
    Write-Host "FAILED: $Message" -ForegroundColor Red
    exit $Code
}

function WriteText([string]$Path, [string]$Text) {
    $parent = Split-Path -Parent $Path
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

function Assert-NoReparsePoint([string]$Path) {
    $current = [System.IO.Path]::GetFullPath($Path)
    while ($current) {
        $item = Get-Item -LiteralPath $current -Force
        # OneDrive uses non-surrogate cloud reparse tags for ordinary local files.
        # Reject path-redirection links; a cloud placeholder is not a junction.
        if ($item.LinkType -in @('SymbolicLink', 'Junction')) {
            Fail "path-redirection links are not admitted for package inputs or staging: $($item.FullName)" 3
        }
        $current = Split-Path -Parent $current
    }
}

function Assert-FixedFile([string]$Path, $Entry) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Fail "required source is missing: $Path" 3
    }
    Assert-NoReparsePoint $Path
    if ($Entry.Source) {
        $file = Get-Item -LiteralPath $Path
        $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -cne $Entry.Sha256 -or $file.Length -ne $Entry.Bytes) {
            Fail "required content differs from product policy: $($Entry.Path)" 5
        }
    }
}

if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
$RepoRoot = (Get-Item -LiteralPath $RepoRoot).FullName
if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot 'build\windows-product' }
$BuildDir = [IO.Path]::GetFullPath($BuildDir).TrimEnd('\', '/')
if ((Split-Path -Parent $BuildDir) -ne [IO.Path]::GetFullPath((Join-Path $RepoRoot 'build')) -or
        (Split-Path -Leaf $BuildDir) -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$') {
    Fail 'BuildDir must be exactly <repo>/build/<preset>' 2
}
if (-not $ProductDataDir) { $ProductDataDir = Join-Path $RepoRoot 'build\product-data' }
if (-not $RimeDll) { $RimeDll = Join-Path $RepoRoot 'build\librime-english\rime\install\lib\rime.dll' }
if ($StrictPaths -and $InternalAllowEmbeddedBuildPaths) {
    Fail '-StrictPaths cannot be combined with -InternalAllowEmbeddedBuildPaths' 2
}
if ($AcceptanceProfile -ceq 'public-signed' -and $DistributionPolicy -cne 'public-candidate') {
    Fail 'public-signed acceptance requires explicit public-candidate distribution policy' 2
}
if ($InternalAllowEmbeddedBuildPaths -and
        ($AcceptanceProfile -cne 'internal-unsigned' -or $DistributionPolicy -cne 'internal-only')) {
    Fail 'embedded build-path exceptions are limited to internal diagnostic packages' 2
}
if ($BackendResourcesRoot -and
        ($AcceptanceProfile -cne 'internal-unsigned' -or $DistributionPolicy -cne 'internal-only')) {
    Fail 'the backend runtime payload is limited to internal-unsigned/internal-only packages' 2
}

$version = (Get-Content -LiteralPath (Join-Path $RepoRoot 'platforms\windows\product\VERSION') -Raw).Trim()
if ($version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?$') {
    Fail 'product VERSION must be a single semantic-version value' 2
}
$identityHelper = Join-Path $PSScriptRoot 'build_identity.ps1'
if (-not (Test-Path -LiteralPath $identityHelper -PathType Leaf)) {
    Fail 'tools/product/build_identity.ps1 is required' 3
}
. $identityHelper
$receipt = Assert-ProductBuildReceipt -RepoRoot $RepoRoot -BuildDir $BuildDir -RimeDll $RimeDll
$engineReceipt = Assert-ProductEngineReceipt -RepoRoot $RepoRoot -RimeDll $RimeDll
if ($receipt.engine_source_sha256 -ne $engineReceipt.source_sha256) {
    Fail 'product and English engine source receipts do not agree' 5
}
$commit = [string]$receipt.source.commit
if ($commit -notmatch '^[0-9a-f]{40}(?:\+dirty\.[0-9a-f]{12})?$') {
    Fail 'verified receipt has no full product source identity' 5
}

$policy = Join-Path $RepoRoot 'data\manifests\product-package-files.inc'
$entries = @()
$destinations = @{}
foreach ($line in (Get-Content -LiteralPath $policy -Encoding UTF8)) {
    if (-not $line.Trim() -or $line.Trim().StartsWith('//')) { continue }
    if ($line -cnotmatch '^EPOCANVA_PACKAGE_FILE\("([A-Za-z0-9_./-]+)", "([A-Za-z0-9_./-]*)", "([0-9a-f]*)", ([0-9]+)\)$') {
        Fail "invalid current product policy row: $line" 2
    }
    $entry = [pscustomobject]@{
        Path = $Matches[1]; Source = $Matches[2]; Sha256 = $Matches[3]; Bytes = [long]$Matches[4]
    }
    if ($entry.Path -match '(^|/)\.{1,2}(/|$)' -or $destinations.ContainsKey($entry.Path)) {
        Fail "unsafe or duplicate policy destination: $($entry.Path)" 2
    }
    if ($entry.Source -and ($entry.Sha256.Length -ne 64 -or $entry.Bytes -le 0)) {
        Fail "fixed policy row has no exact digest and byte count: $($entry.Path)" 2
    }
    $destinations[$entry.Path] = $true
    $entries += $entry
}
if ($entries.Count -eq 0) { Fail 'product payload policy is empty' 2 }
$backendEntries = @()
if ($BackendResourcesRoot) {
    $BackendResourcesRoot = [System.IO.Path]::GetFullPath($BackendResourcesRoot)
    $stagingManifest = Join-Path $BackendResourcesRoot 'data\backend\staging-manifest.json'
    if (-not (Test-Path -LiteralPath $stagingManifest -PathType Leaf)) {
        Fail 'backend staging-manifest.json is missing; the resource set is not verified' 3
    }
    $staging = Get-Content -LiteralPath $stagingManifest -Raw -Encoding UTF8 | ConvertFrom-Json
    if (-not $staging.files -or $staging.files.Count -lt 1) {
        Fail 'backend staging manifest lists no runtime files' 3
    }
    $hasResources = $false
    foreach ($row in $staging.files) {
        $relative = [string]$row.path
        if ($relative -like 'data/backend/provenance/*' -or $relative -eq 'data/backend/staging-manifest.json') {
            continue
        }
        if ($relative -notmatch '^data/(backend/|epocanva_backend_[A-Za-z0-9_.-]+\.(schema\.yaml|dict\.yaml)$|zhuyin\.yaml$)') {
            Fail "unexpected backend payload path: $relative" 4
        }
        if ($destinations.ContainsKey($relative)) {
            Fail "backend payload duplicates a fixed policy destination: $relative" 2
        }
        $destinations[$relative] = $true
        $backendEntries += [pscustomobject]@{
            Path = $relative
            Source = Join-Path $BackendResourcesRoot ($relative -replace '/', '\')
            Sha256 = [string]$row.sha256
            Bytes = [long]$row.bytes
        }
        if ($relative -eq 'data/backend/resources.tsv') { $hasResources = $true }
        Assert-FixedFile $backendEntries[-1].Source $backendEntries[-1]
    }
    if (-not $hasResources) { Fail 'backend payload has no data/backend/resources.tsv' 4 }
    if ($backendEntries.Count -lt 1) { Fail 'backend payload lists no runtime files' 4 }
}
$productBin = Join-Path $BuildDir 'product\bin'
$sources = @{}
foreach ($entry in $entries) {
    if (-not $entry.Source) {
        switch -CaseSensitive ($entry.Path) {
            'EpoCanva.exe' { $sources[$entry.Path] = Join-Path $productBin 'EpoCanva.exe' }
            'epocanva_tsf.dll' { $sources[$entry.Path] = Join-Path $productBin 'epocanva_tsf.dll' }
            'rime.dll' { $sources[$entry.Path] = $RimeDll }
            'VERSION.txt' { continue }
            default { Fail "unknown generated policy path: $($entry.Path)" 2 }
        }
    } elseif ($entry.Path -in @('data/lexicon/epocanva_en_alpha.tsv',
                                'data/lexicon/epocanva_en_alpha.provenance.txt')) {
        $sources[$entry.Path] = Join-Path $ProductDataDir (Split-Path -Leaf $entry.Path)
    } else {
        $sources[$entry.Path] = Join-Path $RepoRoot $entry.Source
    }
    if ($sources.ContainsKey($entry.Path)) { Assert-FixedFile $sources[$entry.Path] $entry }
}

if (-not $StageRoot) { $StageRoot = Join-Path $BuildDir "stage\EpoCanva-$version" }
$stageFull = [System.IO.Path]::GetFullPath($StageRoot)
$stageBase = [System.IO.Path]::GetFullPath((Join-Path $BuildDir 'stage'))
if (-not $stageFull.StartsWith($stageBase + [System.IO.Path]::DirectorySeparatorChar,
                               [System.StringComparison]::OrdinalIgnoreCase) -or
    -not (Split-Path -Leaf $stageFull).StartsWith('EpoCanva-')) {
    Fail 'staging must be an EpoCanva-* directory below BuildDir/stage' 2
}
$ancestor = $stageFull
while (-not (Test-Path -LiteralPath $ancestor)) { $ancestor = Split-Path -Parent $ancestor }
Assert-NoReparsePoint $ancestor
if (Test-Path -LiteralPath $stageFull) {
    if (-not $Replace) { Fail "staging directory exists; pass -Replace: $stageFull" 2 }
    # Guard every descendant before Remove-Item can encounter a directory junction.
    foreach ($item in (Get-ChildItem -LiteralPath $stageFull -Recurse -Force)) {
        if ($item.LinkType -in @('SymbolicLink', 'Junction')) {
            Fail 'existing stage contains a path-redirection link; refusing replacement' 2
        }
    }
    Remove-Item -LiteralPath $stageFull -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stageFull | Out-Null
foreach ($entry in $entries) {
    if ($entry.Path -ceq 'VERSION.txt') { continue }
    $destination = Join-Path $stageFull $entry.Path
    $parent = Split-Path -Parent $destination
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    Copy-Item -LiteralPath $sources[$entry.Path] -Destination $destination
    Assert-FixedFile $destination $entry
}
foreach ($entry in $backendEntries) {
    $destination = Join-Path $stageFull $entry.Path
    $parent = Split-Path -Parent $destination
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    Copy-Item -LiteralPath $entry.Source -Destination $destination
    Assert-FixedFile $destination $entry
}
$pathException = if ($InternalAllowEmbeddedBuildPaths) { 'embedded-build-paths' } else { 'none' }
$fixedBackendRuntimeCount = @($entries | Where-Object {
    $_.Path -like 'data/backend/*' -or $_.Path -like 'data/epocanva_backend_*' -or
        $_.Path -eq 'data/zhuyin.yaml'
}).Count
$versionText = @(
    'EpoCanva Windows Multilingual Alpha',
    "Version: $version",
    "Commit: $commit",
    "Acceptance-profile: $AcceptanceProfile",
    "Distribution-policy: $DistributionPolicy",
    "Backend-runtime-files: $($fixedBackendRuntimeCount + $backendEntries.Count)",
    "Path-exception: $pathException",
    'Built from the verified receipt and current product payload policy.',
    'Runtime network I/O: none by design. See NOTICE.txt.'
) -join "`n"
WriteText (Join-Path $stageFull 'VERSION.txt') ($versionText + "`n")

# Strict by default. The explicit internal-only bypass applies to binary build paths,
# never to text payloads or to missing/changed licences. Both narrow and UTF-16 strings
# are checked because either encoding can occur in a Windows PE image.
$personalPatterns = @('D:\Users\', 'C:\Users\', 'OneDrive', $RepoRoot, $RepoRoot.Replace('\', '/'))
if ($env:USERNAME) { $personalPatterns += $env:USERNAME }
$binaryMatches = @()
$payload = @(Get-ChildItem -LiteralPath $stageFull -Recurse -File -Force)
if ($payload.Count -ne ($entries.Count + $backendEntries.Count)) {
    Fail 'staged file set differs from product policy plus backend runtime payload' 5
}
foreach ($file in $payload) {
    $relative = $file.FullName.Substring($stageFull.Length + 1).Replace('\', '/')
    if (-not $destinations.ContainsKey($relative)) { Fail "unlisted staged file: $relative" 5 }
    $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
    $decoded = @([System.Text.Encoding]::UTF8.GetString($bytes),
                 [System.Text.Encoding]::Unicode.GetString($bytes))
    foreach ($pattern in $personalPatterns) {
        if (-not $pattern) { continue }
        $found = @($decoded | Where-Object {
            $_.IndexOf($pattern, [System.StringComparison]::OrdinalIgnoreCase) -ge 0
        }).Count -gt 0
        if (-not $found) { continue }
        if ($file.Extension -notin @('.exe', '.dll')) {
            Fail "embedded machine path in text payload: $relative" 7
        }
        $binaryMatches += $relative
    }
}
if ($binaryMatches.Count -gt 0 -and -not $InternalAllowEmbeddedBuildPaths) {
    Fail "embedded build paths in binaries: $(($binaryMatches | Sort-Object -Unique) -join ', ')" 7
}
if ($InternalAllowEmbeddedBuildPaths) {
    Write-Host 'INTERNAL ONLY: embedded binary build-path check bypassed; package cannot report ready.' -ForegroundColor Yellow
}

# The manifest is an integrity index, not a publisher signature or legal approval.
$manifestLines = @('epocanva-package-manifest 2', "version=$version", "commit=$commit",
                   "acceptance-profile=$AcceptanceProfile", "distribution-policy=$DistributionPolicy",
                   "path-exception=$pathException")
$totalBytes = 0
foreach ($file in ($payload | Sort-Object {
    $_.FullName.Substring($stageFull.Length + 1).Replace('\', '/')
})) {
    $relative = $file.FullName.Substring($stageFull.Length + 1).Replace('\', '/')
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $totalBytes += $file.Length
    $manifestLines += ("file`t{0}`t{1}`t{2}" -f $relative, $hash, $file.Length)
}
WriteText (Join-Path $stageFull 'package.manifest') (($manifestLines -join "`n") + "`n")
Write-Host "staged: $stageFull"
Write-Host "version: $version  commit: $commit  profile: $AcceptanceProfile  distribution: $DistributionPolicy  path-exception: $pathException"
Write-Host ("payload: {0} files, {1} bytes" -f $payload.Count, $totalBytes)
exit 0
