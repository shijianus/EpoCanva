# EpoCanva W1 -- build the offline product staging directory from an explicit whitelist.
#
# This never copies build/ as a tree. Every payload path is named below; the only files
# admitted are the two product binaries, rime.dll, the product data, the generated lexicon
# and provenance, the notices and the actual upstream licence files. A package manifest
# with SHA-256 and byte counts is generated last.
#
# The staging directory is always below <repo>/build/product/stage/EpoCanva-<version>.
# Replacing an existing one requires -Replace; the path guard refuses anything else.
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$StageRoot = '',
    [string]$BuildDir = '',
    [string]$ProductDataDir = '',
    [switch]$Replace,
    [switch]$StrictPaths
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

function Add-Copy([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        Fail "whitelisted source is missing: $Source" 3
    }
    $parent = Split-Path -Parent $Destination
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

if (-not $RepoRoot) {
    $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
$RepoRoot = (Get-Item -LiteralPath $RepoRoot).FullName
if (-not $BuildDir) {
    $BuildDir = Join-Path $RepoRoot 'build\windows-product'
}
if (-not $ProductDataDir) {
    $ProductDataDir = Join-Path $RepoRoot 'build\product-data'
}
$version = (Get-Content -LiteralPath (Join-Path $RepoRoot 'platforms\windows\product\VERSION') -Raw).Trim()
if (-not $version) {
    Fail 'platforms/windows/product/VERSION is empty' 2
}
$commit = ''
$git = Get-Command git -ErrorAction SilentlyContinue
if ($git) {
    $commit = (& git -C $RepoRoot rev-parse --short HEAD 2>$null)
    if ($LASTEXITCODE -ne 0) { $commit = '' }
}
if (-not $StageRoot) {
    $StageRoot = Join-Path $RepoRoot "build\product\stage\EpoCanva-$version"
}
$stageFull = [System.IO.Path]::GetFullPath($StageRoot)
$stageBase = [System.IO.Path]::GetFullPath((Join-Path $RepoRoot 'build\product\stage'))
if (-not $stageFull.StartsWith($stageBase + [System.IO.Path]::DirectorySeparatorChar,
                               [System.StringComparison]::OrdinalIgnoreCase)) {
    Fail "staging path is outside build/product/stage: $stageFull" 2
}
if (-not (Split-Path -Leaf $stageFull).StartsWith('EpoCanva-')) {
    Fail "staging leaf must start with EpoCanva-: $stageFull" 2
}
if (Test-Path -LiteralPath $stageFull) {
    if (-not $Replace) {
        Fail "staging directory already exists; pass -Replace to rebuild it: $stageFull" 2
    }
    Remove-Item -LiteralPath $stageFull -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stageFull | Out-Null

$productBin = Join-Path $BuildDir 'product\bin'
$exe = Join-Path $productBin 'EpoCanva.exe'
$tsf = Join-Path $productBin 'epocanva_tsf.dll'
$rime = Join-Path $RepoRoot 'build\librime-english\rime\install\lib\rime.dll'
$lexicon = Join-Path $ProductDataDir 'epocanva_en_alpha.tsv'
$provenance = Join-Path $ProductDataDir 'epocanva_en_alpha.provenance.txt'
$dataProduct = Join-Path $RepoRoot 'data\product'

Add-Copy $exe (Join-Path $stageFull 'EpoCanva.exe')
Add-Copy $tsf (Join-Path $stageFull 'epocanva_tsf.dll')
Add-Copy $rime (Join-Path $stageFull 'rime.dll')
Add-Copy (Join-Path $dataProduct 'default.yaml') (Join-Path $stageFull 'data\default.yaml')
Add-Copy (Join-Path $dataProduct 'epocanva_english.schema.yaml') `
    (Join-Path $stageFull 'data\epocanva_english.schema.yaml')
Add-Copy (Join-Path $dataProduct 'lexicon\README.txt') `
    (Join-Path $stageFull 'data\lexicon\README.txt')
Add-Copy $lexicon (Join-Path $stageFull 'data\lexicon\epocanva_en_alpha.tsv')
Add-Copy $provenance (Join-Path $stageFull 'data\lexicon\epocanva_en_alpha.provenance.txt')
Add-Copy (Join-Path $dataProduct 'NOTICE.txt') (Join-Path $stageFull 'NOTICE.txt')

# Actual upstream licence files. A missing file is a hard failure, not a silent omission.
$licenceSources = @(
    @{ Source = 'third_party\librime\LICENSE'; Destination = 'librime-LICENSE.txt'; What = 'librime 1.17.0' },
    @{ Source = 'third_party\librime\deps\yaml-cpp\LICENSE'; Destination = 'yaml-cpp-LICENSE.txt'; What = 'yaml-cpp' },
    @{ Source = 'third_party\librime\deps\leveldb\LICENSE'; Destination = 'leveldb-LICENSE.txt'; What = 'leveldb' },
    @{ Source = 'third_party\librime\deps\marisa-trie\COPYING.md'; Destination = 'marisa-trie-COPYING.md'; What = 'marisa-trie' },
    @{ Source = 'third_party\librime\deps\opencc\LICENSE'; Destination = 'opencc-LICENSE.txt'; What = 'OpenCC' },
    @{ Source = 'build\librime-baseline\dependencies\boost\boost_1_89_0\LICENSE_1_0.txt'; Destination = 'boost-LICENSE_1_0.txt'; What = 'Boost 1.89.0' },
    @{ Source = 'third_party\librime\include\utf8.h'; Destination = 'utfcpp-utf8.h'; What = 'utfcpp' },
    @{ Source = 'third_party\librime\include\COPYING.darts-clone'; Destination = 'darts-clone-COPYING.txt'; What = 'darts-clone' },
    @{ Source = 'build\third-party-fetch\esdb\alt12dicts\README'; Destination = 'alt12dicts-README.txt'; What = 'Alt12Dicts' }
)
$licenceLines = @()
$licenceLines += 'Third-party licence files for this EpoCanva package.'
$licenceLines += ''
$licenceLines += 'Every file below is copied byte-for-byte from the pinned checkout or the verified'
$licenceLines += 'Boost archive named in the EpoCanva source repository''s third_party/sources.lock.json.'
$licenceLines += 'The data notice is in the package NOTICE.txt and in'
$licenceLines += 'data/lexicon/epocanva_en_alpha.provenance.txt.'
$licenceLines += ''
foreach ($entry in $licenceSources) {
    $source = Join-Path $RepoRoot $entry.Source
    $destination = Join-Path $stageFull (Join-Path 'licenses' $entry.Destination)
    Add-Copy $source $destination
    $licenceLines += ("{0}  <-  {1} ({2})" -f $entry.Destination, $entry.Source, $entry.What)
}
$x11 = Join-Path $RepoRoot 'third_party\librime\include\X11\keysym.h'
if (-not (Test-Path -LiteralPath $x11 -PathType Leaf)) {
    Fail "the X11 keysym header notice is missing: $x11" 3
}
$keysymNotice = Get-Content -LiteralPath $x11 -TotalCount 60 | Out-String
WriteText (Join-Path $stageFull 'licenses\x11-keysym-NOTICE.txt') $keysymNotice
$licenceLines += 'x11-keysym-NOTICE.txt  <-  third_party/librime/include/X11/keysym.h (notice)'
WriteText (Join-Path $stageFull 'licenses\README.txt') (($licenceLines -join "`r`n") + "`r`n")

$versionText = @(
    'EpoCanva Windows English Alpha (W1)',
    "Version: $version",
    "Commit: $commit",
    'Built from the product staging whitelist by tools/product/stage_product.ps1.',
    'Runtime network I/O: none by design. See NOTICE.txt.'
) -join "`r`n"
WriteText (Join-Path $stageFull 'VERSION.txt') ($versionText + "`r`n")

# Required payload and forbidden artefacts.
$required = @(
    'EpoCanva.exe', 'epocanva_tsf.dll', 'rime.dll', 'NOTICE.txt', 'VERSION.txt',
    'data\default.yaml', 'data\epocanva_english.schema.yaml',
    'data\lexicon\epocanva_en_alpha.tsv',
    'data\lexicon\epocanva_en_alpha.provenance.txt',
    'licenses\README.txt'
)
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $stageFull $relative) -PathType Leaf)) {
        Fail "required staged file is missing: $relative" 5
    }
}
$forbiddenExtensions = @('.pdb', '.lib', '.exp', '.obj', '.res', '.log', '.ilk', '.tmp', '.bak')
$forbiddenPatterns = @('*test*', '*scratch*', 'epocanva_tsf_hosted_test*', 'tsf_lab*',
                       'EpoCanvaTest*')
$files = @(Get-ChildItem -LiteralPath $stageFull -Recurse -File)
foreach ($file in $files) {
    if ($forbiddenExtensions -contains $file.Extension.ToLowerInvariant()) {
        Fail "forbidden file extension staged: $($file.FullName)" 6
    }
    foreach ($pattern in $forbiddenPatterns) {
        if ($file.Name -like $pattern) {
            Fail "forbidden file name staged: $($file.FullName)" 6
        }
    }
}

# Text payloads must not carry machine paths. Binary matches are reported separately:
# the pinned rime.dll contains upstream __FILE__ strings from the build machine, which is
# a public-release blocker but not a user-data leak; see the W1 report.
$personalPatterns = @('D:\Users\', 'C:\Users\', 'OneDrive')
if ($env:USERNAME) {
    $personalPatterns += $env:USERNAME
}
$textExtensions = @('.txt', '.md', '.yaml', '.ini', '.json', '.tsv')
$textMatches = 0
$binaryMatches = @()
foreach ($file in $files) {
    $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
    $text = [System.Text.Encoding]::UTF8.GetString($bytes)
    foreach ($pattern in $personalPatterns) {
        if ($text.Contains($pattern)) {
            if ($textExtensions -contains $file.Extension.ToLowerInvariant()) {
                Write-Host "PERSONAL PATH in text payload: $($file.FullName) -> $pattern" -ForegroundColor Red
                ++$textMatches
            } elseif ($file.Extension -in @('.exe', '.dll')) {
                $count = ([regex]::Matches($text, [regex]::Escape($pattern))).Count
                $binaryMatches += [pscustomobject]@{ File = $file.Name; Pattern = $pattern; Count = $count }
            }
        }
    }
}
if ($textMatches -gt 0) {
    Fail 'personal paths were found in text payload files' 7
}
if ($binaryMatches.Count -gt 0) {
    $summary = ($binaryMatches | ForEach-Object { "$($_.File): '$($_.Pattern)' x$($_.Count)" }) -join '; '
    if ($StrictPaths) {
        Fail "embedded build paths found in binaries: $summary" 7
    }
    Write-Host "WARNING: embedded build paths in binaries (public-release blocker): $summary" -ForegroundColor Yellow
}

# Package manifest, generated last and itself not listed.
$manifestLines = @(
    'epocanva-package-manifest 1',
    "version=$version",
    "commit=$commit"
)
$payload = @(Get-ChildItem -LiteralPath $stageFull -Recurse -File |
    Where-Object { $_.FullName -ne (Join-Path $stageFull 'package.manifest') } |
    Sort-Object { $_.FullName.Substring($stageFull.Length + 1).Replace('\', '/') })
$totalBytes = 0
foreach ($file in $payload) {
    $relative = $file.FullName.Substring($stageFull.Length + 1).Replace('\', '/')
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $totalBytes += $file.Length
    $manifestLines += ("file`t{0}`t{1}`t{2}" -f $relative, $hash, $file.Length)
}
WriteText (Join-Path $stageFull 'package.manifest') (($manifestLines -join "`n") + "`n")

Write-Host "staged: $stageFull"
Write-Host "version: $version  commit: $commit"
Write-Host ("payload: {0} files, {1} bytes" -f $payload.Count, $totalBytes)
Write-Host "required files and forbidden-artefact checks passed"
exit 0
