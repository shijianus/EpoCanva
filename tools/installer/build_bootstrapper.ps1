# Build the project-owned offline EXE installer bootstrapper.
#
# The bootstrapper embeds the exact verified MSI as a payload blob with the magic
# header EPOINST1 + payload byte count + SHA-256, verifies both digests at runtime
# before writing any file, and delegates the actual install to msiexec. It is a real
# offline EXE entry point, not a renamed MSI.
#
# The embedded payload is appended AFTER the compiled image; the compiled image's
# bytes therefore never depend on the MSI. The MSI SHA-256 is compiled into the
# image as EPOCANVA_EMBEDDED_MSI_SHA256.
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$BuildDir = '',
    [string]$MsiPath = '',
    [string]$InstallerManifest = '',
    [string]$OutputPath = ''
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

if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
$RepoRoot = (Get-Item -LiteralPath $RepoRoot).FullName
$buildBase = [IO.Path]::GetFullPath((Join-Path $RepoRoot 'build'))
if (-not $BuildDir) { $BuildDir = Join-Path $buildBase 'installer-tools' }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { Fail 'vswhere.exe not found' 3 }
$installs = @(& $vswhere -all -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
$vs = $null
foreach ($candidate in $installs) {
    $devShell = Join-Path $candidate 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
    $bundledCmake = Join-Path $candidate 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if ((Test-Path -LiteralPath $devShell -PathType Leaf) -and (Test-Path -LiteralPath $bundledCmake -PathType Leaf)) {
        $vs = $candidate
        break
    }
}
if (-not $vs) { Fail 'No Visual Studio installation with DevShell and bundled CMake' 3 }
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=amd64 -host_arch=amd64' | Out-Null

if (-not $MsiPath) {
    $version = (Get-Content -LiteralPath (Join-Path $RepoRoot 'platforms\windows\product\VERSION') -Raw -Encoding UTF8).Trim()
    $windowsProduct = Join-Path $buildBase 'windows-product'
    $MsiPath = Join-Path $windowsProduct ("installer\EpoCanva-$version-x64.msi")
}
$MsiPath = [IO.Path]::GetFullPath($MsiPath)
if (-not (Test-Path -LiteralPath $MsiPath -PathType Leaf)) {
    Fail "MSI not found: $MsiPath; run tools/installer/build_msi.ps1 first" 3
}
if (-not $InstallerManifest) { $InstallerManifest = Join-Path (Split-Path -Parent $MsiPath) 'installer-manifest.json' }
$InstallerManifest = [IO.Path]::GetFullPath($InstallerManifest)
if (-not (Test-Path -LiteralPath $InstallerManifest -PathType Leaf)) {
    Fail 'installer-manifest.json is missing; run tools/installer/build_msi.ps1' 3
}
$manifest = Get-Content -LiteralPath $InstallerManifest -Raw -Encoding UTF8 | ConvertFrom-Json
if ($manifest.format -ne 2) { Fail 'installer-manifest.json is not format 2' 4 }
$msiSha = Get-Sha256 $MsiPath
$msiBytes = (Get-Item -LiteralPath $MsiPath).Length
if ($manifest.installer.sha256 -cne $msiSha -or [long]$manifest.installer.bytes -ne $msiBytes) {
    Fail 'The MSI does not match its installer manifest; rebuild the MSI first' 4
}
if ($manifest.installer.manifest_included -ne $true -or $manifest.extraction.verified -ne $true) {
    Fail 'The MSI manifest lacks the package-manifest binding or a verified extraction' 4
}

if (-not $OutputPath) {
    $version = [string]$manifest.product_version
    $OutputPath = Join-Path (Split-Path -Parent $MsiPath) "EpoCanva-$version-x64-setup.exe"
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)

$imageDir = Join-Path $BuildDir 'bootstrapper'
New-Item -ItemType Directory -Force -Path $imageDir | Out-Null
$cl = (Get-Command cl.exe).Source
$image = Join-Path $imageDir 'epocanva_setup_image.exe'
# The digest reaches the compiler through a generated identity header: cl strips quotes
# from /D defines and response files alike, so a quoted hex digest cannot survive any
# command-line path. The header is written next to the image and found via /I.
$identityHeader = Join-Path $imageDir 'epocanva_embedded_identity.h'
[System.IO.File]::WriteAllText($identityHeader,
    ('#define EPOCANVA_EMBEDDED_MSI_SHA256 "' + $msiSha + '"' + [Environment]::NewLine),
    (New-Object System.Text.UTF8Encoding($false)))
& $cl /nologo /std:c++17 /permissive- /utf-8 /W4 /EHsc /O2 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN `
    ('/I' + $imageDir) `
    /Fe:$image (Join-Path $RepoRoot 'tools\installer\bootstrapper.cc') /link /SUBSYSTEM:WINDOWS
if ($LASTEXITCODE -ne 0) { Fail "bootstrapper compile failed with exit $LASTEXITCODE" 5 }

# Append the payload blob: magic(8) + byte count(8 LE) + sha256(32) + msi bytes.
$imageBytes = [IO.File]::ReadAllBytes($image)
$msiBytes2 = [IO.File]::ReadAllBytes($MsiPath)
$header = [System.Collections.Generic.List[byte]]::new()
$header.AddRange([Text.Encoding]::ASCII.GetBytes('EPOINST1'))
$lengthBytes = [BitConverter]::GetBytes([UInt64]$msiBytes2.Length)
$header.AddRange($lengthBytes)
$digestBytes = ([Security.Cryptography.SHA256]::Create().ComputeHash($msiBytes2))
if (([BitConverter]::ToString($digestBytes)).Replace('-', '').ToLowerInvariant() -cne $msiSha) {
    Fail 'MSI digest recomputation disagrees with the manifest identity' 5
}
$header.AddRange($digestBytes)
$out = New-Object IO.MemoryStream
$out.Write($imageBytes, 0, $imageBytes.Length)
$out.Write($header.ToArray(), 0, $header.Count)
$out.Write($msiBytes2, 0, $msiBytes2.Length)
[IO.File]::WriteAllBytes($OutputPath, $out.ToArray())

$outSha = Get-Sha256 $OutputPath
$outBytes = (Get-Item -LiteralPath $OutputPath).Length
$imageSha = Get-Sha256 $image
$document = [ordered]@{
    format = 1
    product_version = [string]$manifest.product_version
    embedded_msi_sha256 = $msiSha
    embedded_msi_bytes = [long]$msiBytes
    image_sha256 = $imageSha
    bootstrapper = [ordered]@{
        path = $OutputPath
        sha256 = $outSha
        bytes = [long]$outBytes
        architecture = 'x64'
        payload_magic = 'EPOINST1'
        source = 'tools/installer/bootstrapper.cc'
        installer_manifest_sha256 = Get-Sha256 $InstallerManifest
    }
}
$bootstrapperManifest = Join-Path (Split-Path -Parent $OutputPath) 'bootstrapper-manifest.json'
[IO.File]::WriteAllText($bootstrapperManifest, (($document | ConvertTo-Json -Depth 8) + "`n"),
    (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Built $OutputPath"
Write-Host "Bootstrapper SHA-256: $outSha ($outBytes bytes)"
Write-Host "Embedded MSI: $msiSha ($msiBytes bytes)"
Write-Host "Manifest: $bootstrapperManifest"
exit 0
