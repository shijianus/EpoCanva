[CmdletBinding()]
param(
    [switch]$Download,
    [switch]$SkipDictionaryCheck
)
# Preparation ONLY. No installs, Git checkout, environment changes or build-time
# downloads. The dictionary belongs to the separate acquisition component and
# is verified in place, never copied, shortened or reencoded here.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$ManifestPath = Join-Path $RepoRoot 'data\manifests\backend-hunspell.sources.json'
$Manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
$Pin = $Manifest.hunspell
$DependencyRoot = Join-Path $RepoRoot 'build\backend-french-dependency'
$Downloads = Join-Path $DependencyRoot 'downloads'
$Archive = Join-Path $Downloads ($Pin.archive_root + '.tar.gz')
$SourceRoot = Join-Path $DependencyRoot $Pin.archive_root

function Assert-Hash([string]$Path, [string]$Expected) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file unavailable: $Path"
    }
    $Actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($Actual -cne $Expected) { throw "SHA-256 mismatch: $Path" }
}
function Invoke-OfficialDownload([string]$Url, [string]$Destination) {
    if (!$Download) {
        throw 'Pinned Hunspell dependency missing. Re-run tools/backend/prepare_french.ps1 -Download to allow official HTTPS acquisition.'
    }
    $Uri = [Uri]$Url
    if ($Uri.Scheme -ne 'https' -or $Uri.Host -notin @('api.github.com', 'codeload.github.com')) {
        throw 'Refusing a non-official dependency URL.'
    }
    # curl.exe must already be present. Never install a fallback package.
    $Curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    if (!$Curl) { throw 'Missing tool: Get-Command curl.exe. Provide curl.exe on PATH; no installation was attempted.' }
    & $Curl.Source --fail --location --proto '=https' --proto-redir '=https' --tlsv1.2 `
        --silent --show-error --output $Destination $Url
    if ($LASTEXITCODE -ne 0) { throw "Official HTTPS acquisition failed with exit $LASTEXITCODE." }
}

if (!(Test-Path -LiteralPath $Archive -PathType Leaf)) {
    New-Item -ItemType Directory -Path $Downloads -Force | Out-Null
    $CommitRecord = Join-Path $Downloads 'commit.json'
    Invoke-OfficialDownload $Pin.commit_url $CommitRecord
    $Commit = Get-Content -LiteralPath $CommitRecord -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($Commit.sha -cne $Pin.revision) { throw 'Official GitHub commit SHA does not match the pinned full revision.' }
    $Part = $Archive + '.part'
    try {
        Invoke-OfficialDownload $Pin.archive_url $Part
        Assert-Hash $Part $Pin.archive_sha256
        if ((Get-Item -LiteralPath $Part).Length -ne $Pin.archive_bytes) { throw 'Archive byte count mismatch.' }
        Move-Item -LiteralPath $Part -Destination $Archive
    } finally {
        if (Test-Path -LiteralPath $Part -PathType Leaf) { Remove-Item -LiteralPath $Part }
    }
}
Assert-Hash $Archive $Pin.archive_sha256
if ((Get-Item -LiteralPath $Archive).Length -ne $Pin.archive_bytes) { throw 'Archive byte count mismatch.' }

if (!(Test-Path -LiteralPath $SourceRoot)) {
    $Tar = Get-Command tar.exe -ErrorAction SilentlyContinue
    if (!$Tar) { throw 'Missing tool: Get-Command tar.exe. Provide tar.exe on PATH; no installation was attempted.' }
    # A pinned archive is trusted only after its hash matched. Still enforce a
    # single root/no traversal before using the platform's existing tar utility.
    $Members = @(& $Tar.Source -tzf $Archive)
    if ($LASTEXITCODE -ne 0) { throw "Archive listing failed with exit $LASTEXITCODE." }
    foreach ($Member in $Members) {
        $Normalized = $Member.TrimEnd('/')
        $Parts = $Normalized.Split('/')
        if ($Parts[0] -cne $Pin.archive_root -or $Parts -contains '..' -or
            $Parts -contains '.git' -or $Member.Contains('\') -or $Member.Contains(':')) {
            throw 'Unexpected path in pinned archive.'
        }
    }
    & $Tar.Source -xzf $Archive -C $DependencyRoot
    if ($LASTEXITCODE -ne 0) { throw "Archive extraction failed with exit $LASTEXITCODE." }
}
if (Test-Path -LiteralPath (Join-Path $SourceRoot '.git')) {
    throw 'Nested Git checkout refused; an archive-only dependency is required.'
}
foreach ($File in @($Pin.library_files) + @($Pin.license_files)) {
    Assert-Hash (Join-Path $SourceRoot $File.path) $File.sha256
}

# Compare the ENTIRE extracted tree against the exact archive, including files
# not compiled here that must remain intact for later corresponding-source use.
# PowerShell 5.1 has GZipStream but no TarReader; this narrow archive envelope
# reader checks regular files/directories, size, bytes and path set. It neither
# rewrites source nor interprets any Hunspell grammar.
function Read-Exact([IO.Stream]$Stream, [byte[]]$Buffer, [int]$Count) {
    $ReadBytes = 0
    while ($ReadBytes -lt $Count) {
        $GotBytes = $Stream.Read($Buffer, $ReadBytes, $Count - $ReadBytes)
        if ($GotBytes -eq 0) { throw 'Unexpected end of archive stream.' }
        $ReadBytes += $GotBytes
    }
}
function Read-TarAscii([byte[]]$Buffer, [int]$OffsetBytes, [int]$CountBytes) {
    return [Text.Encoding]::UTF8.GetString($Buffer, $OffsetBytes, $CountBytes).Trim([char]0).Trim()
}
$FileStream = [IO.File]::OpenRead($Archive)
$Gzip = New-Object IO.Compression.GZipStream($FileStream, [IO.Compression.CompressionMode]::Decompress)
$SeenFiles = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
try {
    $Header = New-Object byte[] 512
    $Sha = [Security.Cryptography.SHA256]::Create()
    try {
        while ($true) {
            Read-Exact $Gzip $Header 512
            if ($Header[0] -eq 0) { break }
            $Name = Read-TarAscii $Header 0 100
            $Prefix = Read-TarAscii $Header 345 155
            if ($Prefix) { $Name = $Prefix + '/' + $Name }
            $SizeText = Read-TarAscii $Header 124 12
            $SizeBytes = [Convert]::ToInt32($SizeText, 8)
            $Kind = [char]$Header[156]
            if ($SizeBytes -lt 0 -or $SizeBytes -gt 16777216) { throw 'Unexpected tar entry byte count.' }
            $Payload = New-Object byte[] $SizeBytes
            Read-Exact $Gzip $Payload $SizeBytes
            $PaddingBytes = (512 - ($SizeBytes % 512)) % 512
            if ($PaddingBytes) { Read-Exact $Gzip (New-Object byte[] $PaddingBytes) $PaddingBytes }
            if ($Kind -eq 'g') { continue } # GitHub global PAX metadata, not a file
            $Normalized = $Name.TrimEnd('/')
            $Parts = $Normalized.Split('/')
            if ($Parts[0] -cne $Pin.archive_root -or $Parts -contains '..' -or
                $Parts -contains '.git' -or $Name.Contains('\') -or $Name.Contains(':')) {
                throw 'Unexpected tar path.'
            }
            $Path = Join-Path $DependencyRoot $Name
            if ($Kind -eq '5') {
                if (!(Test-Path -LiteralPath $Path -PathType Container)) { throw 'Extracted directory missing.' }
                continue
            }
            if ($Kind -ne '0' -and $Header[156] -ne 0) { throw 'Unsupported tar entry type; nothing was silently skipped.' }
            if (!$SeenFiles.Add([IO.Path]::GetFullPath($Path))) { throw 'Duplicate archive file.' }
            $Expected = [BitConverter]::ToString($Sha.ComputeHash($Payload)).Replace('-', '').ToLowerInvariant()
            Assert-Hash $Path $Expected
        }
    } finally { $Sha.Dispose() }
} finally { $Gzip.Dispose(); $FileStream.Dispose() }
$ExtractedFiles = @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -Force -File)
if ($ExtractedFiles.Count -ne $SeenFiles.Count) { throw 'Extracted source tree has missing/extra files.' }
foreach ($File in $ExtractedFiles) {
    if (!$SeenFiles.Contains($File.FullName)) { throw 'Unexpected extracted source file.' }
    if (($File.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Source file reparse point refused.' }
}

if (!$SkipDictionaryCheck) {
    $Pair = $Manifest.french_pair
    Assert-Hash (Join-Path $RepoRoot $Pair.aff_path) $Pair.aff_sha256
    Assert-Hash (Join-Path $RepoRoot $Pair.dic_path) $Pair.dic_sha256
    Assert-Hash (Join-Path $RepoRoot $Pair.notice_path) $Pair.notice_sha256
    Write-Output 'French pair: both native files and notice verified in place.'
} else {
    Write-Output 'French pair: not checked (explicit -SkipDictionaryCheck); no availability claim.'
}
Write-Output ("Hunspell prepared: revision {0}; {1} source files match pinned archive; license LGPL-2.1-or-later." -f $Pin.revision, $SeenFiles.Count)
Write-Output 'No build/configure/install or runtime network operation was performed.'
