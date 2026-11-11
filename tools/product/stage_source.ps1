# Offline Corresponding Source producer. Integrity/bindings are not release approval.
# Only the two fixed outputs and private temporary files beneath build/<preset>/
# are written; existing unrelated directories/files are never removed.
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$BuildDir = '',
    [string]$StageRoot = '',
    [string]$RimeDll = '',
    [switch]$Replace
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'source_contract.ps1')
. (Join-Path $PSScriptRoot 'build_identity.ps1')
Add-Type -AssemblyName System.IO.Compression

function Get-SourceStageRecord {
    param([string]$Path, [Collections.Generic.HashSet[string]]$VerifiedDirectories = $null)
    $full = [IO.Path]::GetFullPath($Path)
    $prefix = $RepoRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Source path is outside the repository.'
    }
    $relative = $full.Substring($prefix.Length).Replace('\', '/')
    # The shared helper performs the sole path walk as well as fresh leaf/hash
    # checks. Do not repeat the same ancestor walk in this wrapper.
    return Get-ProductSourceFileRecord -RepoRoot $RepoRoot -RelativePath $relative -VerifiedDirectories $VerifiedDirectories
}
function Assert-SourceStageRecord {
    param([string]$Path, $Expected, [Collections.Generic.HashSet[string]]$VerifiedDirectories = $null)
    $actual = Get-SourceStageRecord $Path -VerifiedDirectories $VerifiedDirectories
    if ($actual.sha256 -cne $Expected.sha256 -or $actual.bytes -ne $Expected.bytes) {
        throw "Source or binding bytes changed: $($Expected.path)"
    }
}
function Read-SourceStagePackage([string]$Path, [string]$Version, [string]$Commit) {
    $lines = @(Get-Content -LiteralPath $Path -Encoding UTF8)
    if ($lines.Count -eq 0 -or $lines[0] -cne 'epocanva-package-manifest 2') {
        throw 'Corresponding Source requires package.manifest format 2; legacy format 1 must be restaged.'
    }
    $metadata = @{}
    $payload = [Collections.Generic.Dictionary[string,object]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($line in $lines | Select-Object -Skip 1) {
        if (-not $line) { continue }
        if ($line -cmatch '^(version|commit|acceptance-profile|distribution-policy|path-exception)=(.+)$') {
            if ($metadata.ContainsKey($Matches[1])) { throw 'Duplicate package identity metadata.' }
            $metadata[$Matches[1]] = $Matches[2]
        } elseif ($line -cmatch '^file\t([^\t]+)\t([0-9a-f]{64})\t(\d+)$') {
            $relative, $hash, $byteCount = $Matches[1], $Matches[2], [long]$Matches[3]
            Assert-ProductSourceRelativePath $relative
            if ($relative -ieq 'package.manifest' -or $payload.ContainsKey($relative)) {
                throw 'Duplicate or self-referential package file.'
            }
            $record = Get-SourceStageRecord (Join-Path $StageRoot $relative)
            if ($record.sha256 -cne $hash -or $record.bytes -ne $byteCount) { throw "Staged package bytes differ: $relative" }
            $payload.Add($relative, $record)
        } else { throw 'Invalid format-2 package manifest row.' }
    }
    if ($metadata.Count -ne 5 -or $metadata['version'] -cne $Version -or $metadata['commit'] -cne $Commit -or
            $metadata['acceptance-profile'] -cnotin @('internal-unsigned', 'public-signed') -or
            $metadata['distribution-policy'] -cnotin @('internal-only', 'public-candidate') -or
            $metadata['path-exception'] -cnotin @('none', 'embedded-build-paths') -or $payload.Count -eq 0) {
        throw 'Format-2 package identity does not match the current verified source.'
    }
    return $payload
}
function Assert-SourceStageZip([string]$Path, [object[]]$Records) {
    $expected = [Collections.Generic.Dictionary[string,object]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($record in $Records) {
        Assert-ProductSourceRelativePath $record.path
        if ($expected.ContainsKey($record.path)) { throw 'Duplicate source manifest path.' }
        $expected.Add($record.path, $record)
    }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $zip = $null
    try {
        $zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Read, $true)
        if ($zip.Entries.Count -ne $expected.Count) { throw 'ZIP entry set differs from the source manifest.' }
        foreach ($entry in $zip.Entries) {
            Assert-ProductSourceRelativePath $entry.FullName
            $unixType = ($entry.ExternalAttributes -shr 16) -band 0xF000
            if (($entry.ExternalAttributes -band 0x10) -ne 0 -or $unixType -notin @(0, 0x8000) -or
                    -not $seen.Add($entry.FullName) -or -not $expected.ContainsKey($entry.FullName)) {
                throw 'ZIP contains a directory, link, duplicate or extra entry.'
            }
            $record = $expected[$entry.FullName]
            if ($record.path -cne $entry.FullName -or $entry.Length -ne $record.bytes) { throw 'ZIP entry path/byte count mismatch.' }
            $inputStream = $entry.Open()
            $algorithm = [Security.Cryptography.SHA256]::Create()
            try {
                # Hash the uncompressed bytes actually read, not central-directory metadata.
                $hash = ([BitConverter]::ToString($algorithm.ComputeHash($inputStream))).Replace('-', '').ToLowerInvariant()
            } finally { $algorithm.Dispose(); $inputStream.Dispose() }
            if ($hash -cne $record.sha256) { throw "ZIP byte digest mismatch: $($record.path)" }
        }
    } finally { if ($null -ne $zip) { $zip.Dispose() }; $stream.Dispose() }
}
function Assert-SourceStageOwnedOutputs([string]$Archive, [string]$Manifest) {
    if (-not (Test-Path -LiteralPath $Archive -PathType Leaf) -or
            -not (Test-Path -LiteralPath $Manifest -PathType Leaf)) {
        throw 'Refusing replacement: both producer-owned output files must already exist.'
    }
    $document = Get-Content -LiteralPath $Manifest -Raw -Encoding UTF8 | ConvertFrom-Json
    $fields = @('format', 'version', 'commit', 'source_sha256', 'package_manifest_sha256',
        'build_receipt_sha256', 'package_policy_sha256', 'archive', 'files')
    if ($document.format -ne 1 -or $document.files -isnot [array] -or $document.files.Count -eq 0 -or
            @($document.PSObject.Properties).Count -ne $fields.Count) { throw 'Existing outputs are not a producer manifest/archive pair.' }
    foreach ($field in $fields) {
        if ($null -eq $document.PSObject.Properties[$field]) { throw 'Existing source manifest is not producer-owned.' }
    }
    $actual = Get-SourceStageRecord $Archive
    if ($document.archive.path -cne $actual.path -or $document.archive.sha256 -cne $actual.sha256 -or
            $document.archive.bytes -ne $actual.bytes) { throw 'Existing source archive does not match its manifest; refusing overwrite.' }
    Assert-SourceStageZip $Archive @($document.files)
}

$archiveTemp = $null
$manifestTemp = $null
$publishedArchive = $false
$publishedManifest = $false
$archivePath = $null
$manifestPath = $null
try {
    if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
    $RepoRoot = (Get-Item -LiteralPath $RepoRoot -Force).FullName.TrimEnd('\', '/')
    $buildBase = Join-Path $RepoRoot 'build'
    if (-not $BuildDir) { $BuildDir = Join-Path $buildBase 'windows-product' }
    $BuildDir = [IO.Path]::GetFullPath($BuildDir).TrimEnd('\', '/')
    if ((Split-Path -Parent $BuildDir) -ine $buildBase -or
            (Split-Path -Leaf $BuildDir) -cnotmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$' -or $BuildDir.EndsWith('.')) {
        throw 'BuildDir must be exactly <repo>/build/<preset>.'
    }
    $archivePath = Join-Path $BuildDir 'corresponding-source.zip'
    $manifestPath = Join-Path $BuildDir 'source-manifest.json'
    foreach ($path in @($BuildDir, $archivePath, $manifestPath)) { Assert-ProductSourcePlainPath $path $RepoRoot }
    if (Test-Path -LiteralPath $BuildDir) {
        if (-not (Test-Path -LiteralPath $BuildDir -PathType Container)) { throw 'BuildDir is not a directory.' }
    } else { $null = New-Item -ItemType Directory -Path $BuildDir }
    $oldArchive = $null; $oldManifest = $null
    if ((Test-Path -LiteralPath $archivePath) -or (Test-Path -LiteralPath $manifestPath)) {
        if (-not $Replace) { throw 'Source outputs already exist; use -Replace only for a valid previous producer pair.' }
        Assert-SourceStageOwnedOutputs $archivePath $manifestPath
        $oldArchive = Get-SourceStageRecord $archivePath
        $oldManifest = Get-SourceStageRecord $manifestPath
    }
    $versionPath = Join-Path $RepoRoot 'platforms/windows/product/VERSION'
    $versionRecord = Get-SourceStageRecord $versionPath
    $version = (Get-Content -LiteralPath $versionPath -Raw -Encoding UTF8).Trim()
    if ($version -cnotmatch '^[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?$') { throw 'Invalid product VERSION.' }
    if (-not $StageRoot) { $StageRoot = Join-Path $BuildDir "stage/EpoCanva-$version" }
    $StageRoot = [IO.Path]::GetFullPath($StageRoot).TrimEnd('\', '/')
    Assert-ProductSourcePlainPath $StageRoot $RepoRoot
    if (-not $RimeDll) { $RimeDll = Join-Path $RepoRoot 'build/librime-english/rime/install/lib/rime.dll' }
    $RimeDll = [IO.Path]::GetFullPath($RimeDll)
    Assert-ProductSourcePlainPath $RimeDll $RepoRoot
    $receiptPath = Join-Path $BuildDir 'product-build-receipt.json'
    $policyPath = Join-Path $RepoRoot 'data/manifests/product-package-files.inc'
    $packagePath = Join-Path $StageRoot 'package.manifest'
    $receiptRecord = Get-SourceStageRecord $receiptPath
    $policyRecord = Get-SourceStageRecord $policyPath
    $packageRecord = Get-SourceStageRecord $packagePath
    $source = Get-ProductSourceIdentity -RepoRoot $RepoRoot
    if ($source.commit -cnotmatch '^[0-9a-f]{40}(?:\+dirty\.[0-9a-f]{12})?$' -or
            $source.source_sha256 -cnotmatch '^[0-9a-f]{64}$') { throw 'Invalid current product source identity.' }
    $receipt = Assert-ProductBuildReceipt -RepoRoot $RepoRoot -BuildDir $BuildDir -RimeDll $RimeDll
    $engine = Assert-ProductEngineReceipt -RepoRoot $RepoRoot -RimeDll $RimeDll
    if ($receipt.source.commit -cne $source.commit -or $receipt.source.source_sha256 -cne $source.source_sha256 -or
            $receipt.engine_source_sha256 -cne $engine.source_sha256) { throw 'Product and engine source receipts do not agree.' }
    $payload = Read-SourceStagePackage $packagePath $version $source.commit
    $paths = @(Get-ProductCorrespondingSourceFiles -RepoRoot $RepoRoot)
    if ($paths.Count -eq 0) { throw 'Corresponding Source inventory is empty.' }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $records = [Collections.Generic.List[object]]::new()
    $previous = $null
    # Each pass validates every encountered ancestor chain once, not just the
    # repository root. Never reuse a directory cache across validation phases.
    $initialDirectories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($relative in $paths) {
        if ($relative -isnot [string]) { throw 'Source inventory must contain path strings.' }
        Assert-ProductSourceRelativePath $relative
        if (-not $seen.Add($relative) -or ($null -ne $previous -and [StringComparer]::Ordinal.Compare($previous, $relative) -ge 0)) {
            throw 'Source inventory is not ordinal-sorted and unique.'
        }
        if ($relative -ceq "build/$(Split-Path -Leaf $BuildDir)/corresponding-source.zip" -or
                $relative -ceq "build/$(Split-Path -Leaf $BuildDir)/source-manifest.json") { throw 'Source inventory includes its own output.' }
        $records.Add((Get-ProductSourceFileRecord $RepoRoot $relative -VerifiedDirectories $initialDirectories))
        $previous = $relative
    }
    $initialDirectories = $null
    $token = [guid]::NewGuid().ToString('N')
    $archiveTemp = Join-Path $BuildDir ".source-$token.zip.tmp"
    $manifestTemp = Join-Path $BuildDir ".source-$token.json.tmp"
    Assert-ProductSourcePlainPath $archiveTemp $RepoRoot
    $output = [IO.File]::Open($archiveTemp, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $zip = $null
    try {
        $zip = [IO.Compression.ZipArchive]::new($output, [IO.Compression.ZipArchiveMode]::Create, $true)
        $preCopyDirectories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($record in $records) {
            $path = Join-Path $RepoRoot $record.path
            Assert-SourceStageRecord $path $record -VerifiedDirectories $preCopyDirectories
            $inputStream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
            $entryStream = $null
            try {
                $entry = $zip.CreateEntry($record.path, [IO.Compression.CompressionLevel]::Optimal)
                $entry.LastWriteTime = [DateTimeOffset]::new(2000, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
                $entry.ExternalAttributes = 0
                $entryStream = $entry.Open()
                $inputStream.CopyTo($entryStream)
            } finally { if ($null -ne $entryStream) { $entryStream.Dispose() }; $inputStream.Dispose() }
        }
    } finally { if ($null -ne $zip) { $zip.Dispose() }; $output.Dispose(); $preCopyDirectories = $null }
    Assert-SourceStageZip $archiveTemp $records.ToArray()
    # Re-enumeration detects additions/deletions and revalidates pinned checkouts;
    # per-file hashes cover ignored, explicitly admitted dependency/data inputs too.
    $currentPaths = @(Get-ProductCorrespondingSourceFiles -RepoRoot $RepoRoot)
    if ($currentPaths.Count -ne $paths.Count) { throw 'Required source file set changed while archiving.' }
    $finalDirectories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    for ($fileIndex = 0; $fileIndex -lt $paths.Count; ++$fileIndex) {
        if ($currentPaths[$fileIndex] -cne $paths[$fileIndex]) { throw 'Required source file set changed while archiving.' }
        Assert-SourceStageRecord (Join-Path $RepoRoot $paths[$fileIndex]) $records[$fileIndex] -VerifiedDirectories $finalDirectories
    }
    $finalDirectories = $null
    $current = Get-ProductSourceIdentity -RepoRoot $RepoRoot
    if ($current.commit -cne $source.commit -or $current.source_sha256 -cne $source.source_sha256) {
        throw 'Product source identity changed while archiving.'
    }
    $null = Assert-ProductBuildReceipt -RepoRoot $RepoRoot -BuildDir $BuildDir -RimeDll $RimeDll
    $currentEngine = Assert-ProductEngineReceipt -RepoRoot $RepoRoot -RimeDll $RimeDll
    if ($currentEngine.source_sha256 -cne $engine.source_sha256) { throw 'Engine source identity changed while archiving.' }
    foreach ($binding in @(@($versionPath, $versionRecord), @($receiptPath, $receiptRecord),
            @($policyPath, $policyRecord), @($packagePath, $packageRecord))) { Assert-SourceStageRecord $binding[0] $binding[1] }
    foreach ($record in $payload.Values) { Assert-SourceStageRecord (Join-Path $RepoRoot $record.path) $record }
    $archiveRecord = Get-SourceStageRecord $archiveTemp
    $archiveRecord.path = "build/$(Split-Path -Leaf $BuildDir)/corresponding-source.zip"
    $document = [ordered]@{
        format = 1; version = $version; commit = $source.commit; source_sha256 = $source.source_sha256
        package_manifest_sha256 = $packageRecord.sha256; build_receipt_sha256 = $receiptRecord.sha256
        package_policy_sha256 = $policyRecord.sha256; archive = $archiveRecord; files = $records.ToArray()
    }
    Assert-ProductSourcePlainPath $manifestTemp $RepoRoot
    $jsonBytes = [Text.UTF8Encoding]::new($false).GetBytes(($document | ConvertTo-Json -Depth 8) + "`n")
    $manifestStream = [IO.File]::Open($manifestTemp, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $manifestStream.Write($jsonBytes, 0, $jsonBytes.Length) } finally { $manifestStream.Dispose() }
    if ($null -ne $oldArchive) {
        Assert-SourceStageRecord $archivePath $oldArchive
        Assert-SourceStageRecord $manifestPath $oldManifest
        # Replace only previously validated files, never the enclosing BuildDir.
        [IO.File]::Replace($archiveTemp, $archivePath, [NullString]::Value)
        $archiveTemp = $null
        $publishedArchive = $true
        [IO.File]::Replace($manifestTemp, $manifestPath, [NullString]::Value)
        $manifestTemp = $null
        $publishedManifest = $true
    } else {
        [IO.File]::Move($archiveTemp, $archivePath)
        $archiveTemp = $null
        $publishedArchive = $true
        [IO.File]::Move($manifestTemp, $manifestPath)
        $manifestTemp = $null
        $publishedManifest = $true
    }
    Assert-SourceStageRecord $archivePath $archiveRecord
    Write-Host "Corresponding Source: $($records.Count) files; archive $($archiveRecord.bytes) bytes."
    Write-Host "Archive SHA-256: $($archiveRecord.sha256)"
    Write-Host "Manifest: $manifestPath"
    exit 0
} catch {
    # No manifest is published before all checks pass. A rare publication failure
    # can leave a mismatched old pair; it is deliberately not treated as success.
    Write-Host ('FAILED: ' + $_.Exception.Message)
    exit 1
} finally {
    foreach ($temporary in @($archiveTemp, $manifestTemp)) {
        if ($temporary -and (Test-Path -LiteralPath $temporary -PathType Leaf)) {
            try { Assert-ProductSourcePlainPath $temporary $RepoRoot; [IO.File]::Delete($temporary) }
            catch { Write-Warning 'Could not remove a private source temporary file.' }
        }
    }
}
