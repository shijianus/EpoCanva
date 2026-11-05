# EpoCanva internal portable product layout.
#
# This is NOT a release package. It assembles the verified product binaries, the
# product data and the reviewed five-language runtime resources into one folder so
# an internal tester can run EpoCanva.exe and inspect the real profile list. It
# writes no package.manifest and makes no distribution-authorization claim.
#
# All output stays below <repo>/build/<preset>/internal-product/. No system
# configuration, registration, installer, download or privilege change is performed.
[CmdletBinding()]
param(
    [string]$BuildDir = '',
    [string]$BackendResourcesRoot = '',
    [string]$RimeDll = '',
    [string]$StageRoot = '',
    [switch]$Replace
)

$ErrorActionPreference = 'Stop'
function Fail([string]$Message, [int]$Code = 1) {
    Write-Error $Message
    exit $Code
}
function Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Assert-NoLink([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.LinkType -in @('SymbolicLink', 'Junction')) {
        Fail "path redirection is not allowed: $Path" 2
    }
    if ($item.PSIsContainer) {
        foreach ($child in Get-ChildItem -LiteralPath $Path -Recurse -Force) {
            if ($child.LinkType -in @('SymbolicLink', 'Junction')) {
                Fail "path redirection is not allowed below: $Path" 2
            }
        }
    }
}

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build\windows-product' }
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir).TrimEnd('\', '/')
$buildBase = [System.IO.Path]::GetFullPath((Join-Path $repo 'build'))
if ((Split-Path -Parent $BuildDir) -ine $buildBase -or
        (Split-Path -Leaf $BuildDir) -cnotmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$') {
    Fail 'BuildDir must be exactly <repo>/build/<preset>' 2
}
if (-not $BackendResourcesRoot) { $BackendResourcesRoot = Join-Path $repo 'build\product-resources\product' }
if (-not (Test-Path -LiteralPath (Join-Path $BackendResourcesRoot 'data\backend\resources.tsv') -PathType Leaf)) {
    $fallback = Join-Path $repo 'build\ime-resources\product'
    if (Test-Path -LiteralPath (Join-Path $fallback 'data\backend\resources.tsv') -PathType Leaf) {
        $BackendResourcesRoot = $fallback
    } else {
        Fail 'verified multilingual resources are missing; run tools/backend/stage_resources.py first' 3
    }
}
$BackendResourcesRoot = [System.IO.Path]::GetFullPath($BackendResourcesRoot)
if (-not $RimeDll) { $RimeDll = Join-Path $repo 'build\librime-english\rime\install\lib\rime.dll' }
$RimeDll = [System.IO.Path]::GetFullPath($RimeDll)
foreach ($required in @(
        (Join-Path $BuildDir 'product\bin\EpoCanva.exe'),
        (Join-Path $BuildDir 'product\bin\epocanva_tsf.dll'),
        $RimeDll)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        Fail "required product file is missing: $required" 3
    }
}
$manifestPath = Join-Path $BackendResourcesRoot 'data\backend\staging-manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    Fail 'backend staging-manifest.json is missing; the resource set is not verified' 3
}
$staging = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
if (-not $staging.files -or $staging.files.Count -lt 1) {
    Fail 'backend staging manifest lists no runtime files' 3
}

if (-not $StageRoot) { $StageRoot = Join-Path $BuildDir 'internal-product\EpoCanva-internal' }
$StageRoot = [System.IO.Path]::GetFullPath($StageRoot)
$internalBase = [System.IO.Path]::GetFullPath((Join-Path $BuildDir 'internal-product'))
if (-not $StageRoot.StartsWith($internalBase + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase) -or
        -not (Split-Path -Leaf $StageRoot).StartsWith('EpoCanva-')) {
    Fail 'StageRoot must be an EpoCanva-* directory below <BuildDir>/internal-product' 2
}
if (Test-Path -LiteralPath $StageRoot) {
    Assert-NoLink $StageRoot
    if (-not $Replace) { Fail "stage exists; pass -Replace: $StageRoot" 2 }
    Remove-Item -LiteralPath $StageRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $StageRoot | Out-Null

function Copy-Checked([string]$Source, [string]$Destination, [string]$ExpectedSha, [long]$ExpectedBytes) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { Fail "missing source file: $Source" 4 }
    $sourceBytes = (Get-Item -LiteralPath $Source).Length
    if ($ExpectedBytes -gt 0 -and $sourceBytes -ne $ExpectedBytes) {
        Fail "source byte count differs: $Source" 4
    }
    if ($ExpectedSha) {
        if ((Sha256 $Source) -cne $ExpectedSha) { Fail "source digest differs: $Source" 4 }
    }
    $parent = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
    if ($ExpectedSha -and (Sha256 $Destination) -cne $ExpectedSha) {
        Fail "copied digest differs: $Destination" 4
    }
    return $sourceBytes
}

$bytes = [long]0
# Binaries and engine DLL.
foreach ($pair in @(
        @((Join-Path $BuildDir 'product\bin\EpoCanva.exe'), (Join-Path $StageRoot 'EpoCanva.exe')),
        @((Join-Path $BuildDir 'product\bin\epocanva_tsf.dll'), (Join-Path $StageRoot 'epocanva_tsf.dll')),
        @($RimeDll, (Join-Path $StageRoot 'rime.dll')))) {
    $bytes += Copy-Checked $pair[0] $pair[1] '' 0
}
# Product data and notices.
$productData = Join-Path $repo 'data\product'
foreach ($relative in @(
        'default.yaml',
        'epocanva_english.schema.yaml',
        'lexicon\README.txt')) {
    $bytes += Copy-Checked (Join-Path $productData $relative) (Join-Path $StageRoot ('data\' + $relative)) '' 0
}
$bytes += Copy-Checked (Join-Path $productData 'NOTICE.txt') (Join-Path $StageRoot 'NOTICE.txt') '' 0
$bytes += Copy-Checked (Join-Path $repo 'build\product-data\epocanva_en_alpha.tsv') `
    (Join-Path $StageRoot 'data\lexicon\epocanva_en_alpha.tsv') '' 0
$bytes += Copy-Checked (Join-Path $repo 'build\product-data\epocanva_en_alpha.provenance.txt') `
    (Join-Path $StageRoot 'data\lexicon\epocanva_en_alpha.provenance.txt') '' 0
foreach ($relative in @('LICENSE', 'data\manifests\license-decision.json',
        'data\manifests\product-dependencies.json')) {
    $bytes += Copy-Checked (Join-Path $repo $relative) (Join-Path $StageRoot (Split-Path -Leaf $relative)) '' 0
}
if (Test-Path -LiteralPath (Join-Path $productData 'licenses')) {
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $productData 'licenses') -File -Recurse -Force) {
        $relative = $file.FullName.Substring((Join-Path $productData 'licenses').Length).TrimStart('\')
        $bytes += Copy-Checked $file.FullName (Join-Path $StageRoot ('licenses\' + $relative)) '' 0
    }
}

# Backend runtime data. Provenance reports and the staging manifest itself are
# audit material, not runtime payload, and are deliberately not copied here.
$backendCount = 0
foreach ($entry in $staging.files) {
    $relative = [string]$entry.path
    if ($relative -like 'data/backend/provenance/*' -or $relative -eq 'data/backend/staging-manifest.json') {
        continue
    }
    if ($relative -notmatch '^data/(backend/|epocanva_backend_[A-Za-z0-9_.-]+\.(schema\.yaml|dict\.yaml)$|zhuyin\.yaml$)') {
        Fail "unexpected backend payload path: $relative" 4
    }
    $source = Join-Path $BackendResourcesRoot ($relative -replace '/', '\')
    $destination = Join-Path $StageRoot ($relative -replace '/', '\')
    $bytes += Copy-Checked $source $destination ([string]$entry.sha256) ([long]$entry.bytes)
    ++$backendCount
}
if ($backendCount -lt 1) { Fail 'no backend runtime files were copied' 4 }
if (-not (Test-Path -LiteralPath (Join-Path $StageRoot 'data\backend\resources.tsv') -PathType Leaf)) {
    Fail 'the staged layout has no data/backend/resources.tsv' 4
}

# Identity and scope. No package.manifest is written: this layout cannot be
# mistaken for a verified release package and cannot satisfy a release gate.
$version = (Get-Content -LiteralPath (Join-Path $repo 'platforms\windows\product\VERSION') -Raw).Trim()
$commit = (& git -C $repo rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $commit -notmatch '^[0-9a-f]{40}$') { Fail 'cannot read the source commit' 5 }
$dirty = if (@(& git -C $repo status --porcelain).Count -gt 0) { 'yes' } else { 'no' }
$versionText = @(
    'EpoCanva internal portable layout - NOT A RELEASE PACKAGE',
    "Version: $version",
    "Commit: $commit",
    "Source-dirty: $dirty",
    'Acceptance: internal diagnostics only',
    'Distribution: not authorized; third-party data rights remain unresolved',
    'This folder has no package.manifest and must not be published or passed to check_release.ps1.',
    'Runtime network I/O: none by design.'
) -join "`n"
[System.IO.File]::WriteAllText((Join-Path $StageRoot 'VERSION.txt'), $versionText + "`n",
    (New-Object System.Text.UTF8Encoding($false)))
$scope = @(
    'INTERNAL ONLY - NOT A DISTRIBUTION PACKAGE',
    '',
    'This layout exists to exercise the current product build with the reviewed five-language',
    'runtime resources. It is not signed, not authorized for public distribution, and its',
    'third-party models/dictionaries retain unresolved redistribution terms.',
    '',
    'The TSF DLL is not registered by this script. To test the text service, register it from',
    'an elevated prompt with:',
    '    regsvr32 "' + (Join-Path $StageRoot 'epocanva_tsf.dll') + '"',
    'and unregister it afterwards with the same command plus /u. Review docs/product/release-gate.md',
    'before treating any result as release evidence.'
) -join "`n"
[System.IO.File]::WriteAllText((Join-Path $StageRoot 'INTERNAL-ONLY.txt'), $scope + "`n",
    (New-Object System.Text.UTF8Encoding($false)))

$files = @(Get-ChildItem -LiteralPath $StageRoot -Recurse -File -Force)
$total = ($files | Measure-Object -Property Length -Sum).Sum
Write-Host "internal layout: $StageRoot"
Write-Host ("payload: {0} files, {1} bytes (backend runtime files: {2})" -f $files.Count, $total, $backendCount)
Write-Host 'not a release package; no package.manifest was written.'
exit 0
