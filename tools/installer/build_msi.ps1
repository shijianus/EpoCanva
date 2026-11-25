# Build a reproducible Windows x64 per-machine MSI from the verified staged package.
# Immutable runtime files live under Program Files; all writable settings, engine state,
# logs and learning data remain under LocalAppData/EpoCanva. The installer helper is
# installed outside the package root and is removed on uninstall.
#
# The WiX source is generated into the build directory (never the source tree) from the
# package manifest, which is itself hash-bound to the tested build.
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$BuildDir = '',
    [string]$StageRoot = '',
    [string]$WixPath = '',
    [string]$HelperBuildDir = '',
    [string]$OutputPath = '',
    [string]$ProductVersion = '',
    [switch]$SkipExtractionCheck
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installer_contract.ps1')
. (Join-Path $PSScriptRoot 'wix_tool.ps1')
. (Join-Path $PSScriptRoot 'static_msi.ps1')

function Fail([string]$Message, [int]$Code = 1) {
    Write-Host "FAILED: $Message" -ForegroundColor Red
    exit $Code
}
function ConvertTo-MsiVersion([string]$Version) {
    $match = [regex]::Match($Version, '^(\d{1,3})\.(\d{1,3})\.(\d{1,5})(?:-[A-Za-z0-9.-]+)?$')
    if (-not $match.Success) { return '' }
    $major = [int]$match.Groups[1].Value
    $minor = [int]$match.Groups[2].Value
    $patch = [int]$match.Groups[3].Value
    if ($major -gt 255 -or $minor -gt 255 -or $patch -gt 65535) { return '' }
    return "$major.$minor.$patch"
}
function Get-Sha256([string]$Path) {
    return (Get-InstallerFileIdentity $Path).sha256
}
function Normalize-MsiRelativePath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or $Path.Contains([char]92) -or
        $Path.StartsWith('/') -or $Path.Contains(':')) { throw 'Unsafe MSI-relative path.' }
    $parts = @($Path.Split('/'))
    if (@($parts | Where-Object { -not $_ -or $_ -in @('.', '..') -or $_.EndsWith('.') -or $_.EndsWith(' ') -or
        $_ -match '[<>:"|?*\x00-\x1f]|^(CON|PRN|AUX|NUL|COM[1-9\u00b9\u00b2\u00b3]|LPT[1-9\u00b9\u00b2\u00b3])(?:\.|$)' }).Count -ne 0) {
        throw 'Unsafe MSI-relative path component.'
    }
    return ($parts -join '/')
}
function Test-PathWithin([string]$Path, [string]$Boundary) {
    $full = [IO.Path]::GetFullPath($Path).TrimEnd([char]92, [char]47)
    $base = [IO.Path]::GetFullPath($Boundary).TrimEnd([char]92, [char]47)
    return $full.Equals($base, [StringComparison]::OrdinalIgnoreCase) -or
        $full.StartsWith($base + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
}
function Assert-ExactFileSet([string]$Root, [object[]]$Expected) {
    $byPath = [Collections.Generic.Dictionary[string,object]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($item in $Expected) {
        $relative = Normalize-MsiRelativePath ([string]$item.path)
        if ($relative -cne [string]$item.path -or $byPath.ContainsKey($relative)) {
            throw "Installer payload has a duplicate or case-colliding path: $relative"
        }
        $byPath.Add($relative, $item)
    }
    $observed = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $directories = [Collections.Generic.Queue[string]]::new()
    $directories.Enqueue([IO.Path]::GetFullPath($Root))
    while ($directories.Count -gt 0) {
        foreach ($entry in Get-ChildItem -LiteralPath $directories.Dequeue() -Force) {
            $attributes = [IO.File]::GetAttributes($entry.FullName)
            if ($attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Installer input contains a reparse point.' }
            if (($attributes -band [IO.FileAttributes]::Directory) -ne 0) {
                $directories.Enqueue($entry.FullName)
                continue
            }
            $relative = $entry.FullName.Substring([IO.Path]::GetFullPath($Root).TrimEnd([char]92, [char]47).Length + 1).Replace([char]92, [char]47)
            $relative = Normalize-MsiRelativePath $relative
            if (-not $observed.Add($relative) -or -not $byPath.ContainsKey($relative)) {
                throw "Extracted MSI contains an extra or duplicate path: $relative"
            }
            $expectedFile = $byPath[$relative]
            if ($relative -cne [string]$expectedFile.path -or $entry.Length -ne [long]$expectedFile.bytes -or
                (Get-Sha256 $entry.FullName) -cne [string]$expectedFile.sha256) {
                throw "Extracted MSI file identity differs: $relative"
            }
        }
    }
    if ($observed.Count -ne $byPath.Count) {
        $missing = @($byPath.Keys | Where-Object { -not $observed.Contains($_) })
        throw ("Extracted MSI file set is incomplete: " + ($missing -join ', '))
    }
}
function Assert-WixVersion([string]$Version, [int]$ExitCode) {
    # The probe is pinned to the complete provisioned build, not a compatible major.
    if ($ExitCode -ne 0 -or $Version -cne '6.0.2+b3f3403') {
        throw 'The installer requires the exactly pinned WiX 6.0.2+b3f3403 build.'
    }
}
function XmlEscape([string]$Text) {
    return [Security.SecurityElement]::Escape($Text)
}

if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
$RepoRoot = (Get-Item -LiteralPath $RepoRoot).FullName
$buildBase = [IO.Path]::GetFullPath((Join-Path $RepoRoot 'build'))
$BuildDir = Get-InstallerBuildDirectory $RepoRoot $BuildDir 'windows-product'
if (-not [Environment]::Is64BitProcess) { Fail 'Installer production requires x64 PowerShell.' 2 }
$versionText = (Get-Content -LiteralPath (Join-Path $RepoRoot 'platforms\windows\product\VERSION') -Raw -Encoding UTF8).Trim()
if (-not (ConvertTo-MsiVersion $versionText)) { Fail 'Invalid installer product VERSION.' 2 }
if (-not $StageRoot) { $StageRoot = Join-Path $BuildDir "stage\EpoCanva-$versionText" }
$StageRoot = Assert-InstallerBuildPath $StageRoot $BuildDir
if (-not (Test-Path -LiteralPath (Join-Path $StageRoot 'package.manifest') -PathType Leaf)) {
    Fail 'The staged package.manifest is missing; run stage_product.ps1' 3
}
if (-not $ProductVersion) { $ProductVersion = ConvertTo-MsiVersion $versionText }
if ($ProductVersion -cne (ConvertTo-MsiVersion $versionText)) { Fail 'MSI ProductVersion must match product VERSION.' 2 }

$outputDir = Assert-InstallerBuildPath (Join-Path $BuildDir 'installer') $BuildDir
if (-not $OutputPath) { $OutputPath = Join-Path $outputDir "EpoCanva-$versionText-x64.msi" }
$OutputPath = Assert-InstallerBuildPath $OutputPath $outputDir
if ([IO.Path]::GetDirectoryName($OutputPath) -ine $outputDir -or [IO.Path]::GetExtension($OutputPath) -ine '.msi') {
    Fail 'MSI output must be an .msi file directly in the installer output directory.' 2
}
$manifestPath = Assert-InstallerBuildPath (Join-Path $outputDir 'installer-manifest.json') $outputDir

if (-not $WixPath) {
    $candidates = @(
        (Join-Path $buildBase '_local\wix-tool\wix.exe'),
        (Join-Path $buildBase 'tools\wix\wix.exe'),
        'wix.exe')
    foreach ($candidate in $candidates) {
        if ($candidate -eq 'wix.exe') {
            $command = Get-Command wix.exe -CommandType Application -ErrorAction SilentlyContinue
            if ($null -ne $command) { $WixPath = $command.Source; break }
        } elseif (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $WixPath = $candidate
            break
        }
    }
}
if (-not $WixPath -or -not (Test-Path -LiteralPath $WixPath -PathType Leaf)) {
    Fail 'wix.exe not found. Provision the pinned WiX tool (build/_local/wix-tool) first.' 3
}
$WixPath = (Get-Item -LiteralPath $WixPath).FullName
$wix = Get-PinnedInstallerWix $WixPath
$WixPath = $wix.executable.path

# The helper is project-owned and built separately from the product package.
$HelperBuildDir = Get-InstallerBuildDirectory $RepoRoot $HelperBuildDir 'installer-tools'
$helper = Join-Path $HelperBuildDir 'bin\Release\epocanva_tip_profile_helper.exe'
if (-not (Test-Path -LiteralPath $helper -PathType Leaf)) {
    $helper = Join-Path $HelperBuildDir 'bin\epocanva_tip_profile_helper.exe'
}
if (-not (Test-Path -LiteralPath $helper -PathType Leaf)) {
    Fail 'epocanva_tip_profile_helper.exe is missing; run tools/installer/build_helper.ps1' 3
}
$helper = (Get-Item -LiteralPath $helper).FullName
$helperReceiptPath = Join-Path $HelperBuildDir 'helper-build.json'
if (-not (Test-Path -LiteralPath $helperReceiptPath -PathType Leaf)) {
    Fail 'helper-build.json is missing; rebuild the helper with tools/installer/build_helper.ps1' 3
}
$verifiedHelper = Assert-InstallerHelperReceipt $RepoRoot $helperReceiptPath $helper
$helperReceipt = $verifiedHelper.receipt
$helperReceiptSha = Get-Sha256 $helperReceiptPath
$helperSha = Get-Sha256 $helper
$helperBytes = (Get-Item -LiteralPath $helper).Length
if ($helperReceipt.format -ne 3 -or $helperReceipt.status -ne 'verified' -or
    -not (Test-InstallerInteger $helperReceipt.build_exit) -or $helperReceipt.build_exit -ne 0 -or
    $helperReceipt.binary.sha256 -cne $helperSha -or -not (Test-InstallerInteger $helperReceipt.binary.bytes) -or
    $helperReceipt.binary.bytes -ne $helperBytes) {
    Fail 'The installer helper does not match its verified source/build receipt' 4
}

# Read and verify the staged package manifest against the actual files.
$metadata = @{}
$files = [System.Collections.Generic.List[object]]::new()
$manifestFile = Join-Path $StageRoot 'package.manifest'
$manifestSha = Get-Sha256 $manifestFile
$manifestBytes = (Get-Item -LiteralPath $manifestFile).Length
$manifestRow = [pscustomobject]@{ relative = 'package.manifest'; path = $manifestFile;
    sha256 = $manifestSha; bytes = [long]$manifestBytes; role = 'package-metadata' }
$seenPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$manifestHeaders = 0
foreach ($line in (Get-Content -LiteralPath $manifestFile -Encoding UTF8)) {
    if (-not $line -or $line.StartsWith('#')) { continue }
    if ($line -ceq 'epocanva-package-manifest 2') { $manifestHeaders++; continue }
    if ($line -cmatch '^(version|commit|acceptance-profile|distribution-policy|path-exception)=(.+)$') {
        $metadata.Add($Matches[1], $Matches[2])
    } elseif ($line -cmatch '^file\t([^\t]+)\t([0-9a-f]{64})\t(\d+)$') {
        $relative = Normalize-MsiRelativePath $Matches[1]
        $hash = $Matches[2]
        $bytes = [uint64]$Matches[3]
        if (-not $seenPaths.Add($relative)) { Fail "Duplicate or case-colliding package path: $relative" 4 }
        $path = Assert-InstallerBuildPath (Join-Path $StageRoot ($relative.Replace('/', '\'))) $StageRoot
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or -not (Test-PathWithin $path $StageRoot)) {
            Fail "Staged package path is missing or redirects outside the package: $relative" 4
        }
        $file = Get-Item -LiteralPath $path -Force
        if ($file.PSIsContainer -or $file.Length -ne $bytes -or (Get-Sha256 $path) -cne $hash) {
            Fail "Staged file differs from its manifest: $relative" 4
        }
        $files.Add([pscustomobject]@{ relative = $relative; path = $file.FullName;
            sha256 = $hash; bytes = [long]$bytes; role = 'runtime' })
    } else {
        Fail 'Unrecognised package manifest row' 4
    }
}
if ($files.Count -eq 0 -or $manifestHeaders -ne 1) { Fail 'The package requires exactly one format-2 header and a nonempty payload.' 4 }
if ($metadata['version'] -cne $versionText) { Fail 'The staged package version differs from product VERSION' 4 }
if ($metadata.Count -ne 5 -or $metadata['commit'] -cnotmatch '^[0-9a-f]{40}$' -or
    $metadata['acceptance-profile'] -cnotin @('internal-unsigned', 'public-signed') -or
    $metadata['distribution-policy'] -cnotin @('internal-only', 'public-candidate') -or
    $metadata['path-exception'] -cne 'none') { Fail 'The staged package lacks strict candidate identity metadata.' 4 }

$packagePayloadFiles = $files.Count
$stageExpected = @($files | ForEach-Object {
    [pscustomobject]@{ path = $_.relative; bytes = $_.bytes; sha256 = $_.sha256 }
}) + @([pscustomobject]@{ path = 'package.manifest'; bytes = $manifestBytes; sha256 = $manifestSha })
try { Assert-ExactFileSet -Root $StageRoot -Expected $stageExpected }
catch { Fail ('Staged product file set is not exact: ' + $_.Exception.Message) 4 }
$files.Add([pscustomobject]@{ relative = 'package.manifest'; path = $manifestFile;
    sha256 = $manifestSha; bytes = [long]$manifestBytes; role = 'package-metadata' })

New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$runDir = New-InstallerRunDirectory $outputDir 'msi'
$installerSource = Get-InstallerSources $RepoRoot 'msi'
$wxsPath = Join-Path $runDir 'EpoCanva.wxs'
$extractDir = Join-Path $runDir 'extract'
$sourceDir = Join-Path $runDir 'source'
$helperSource = Join-Path (Join-Path $runDir 'installer-helper') 'epocanva_tip_profile_helper.exe'

# Copy the application payload and helper into private build inputs so WiX never reads
# mutable user/build output while constructing the MSI. The helper is outside the product root.
New-Item -ItemType Directory -Path $sourceDir -Force | Out-Null
foreach ($file in $files) {
    $destination = Join-Path $sourceDir ($file.relative.Replace('/', '\'))
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $file.path -Destination $destination -Force
    if ((Get-Sha256 $destination) -cne $file.sha256 -or
        (Get-Item -LiteralPath $destination).Length -ne $file.bytes) {
        Fail "Private installer input differs from verified payload: $($file.relative)" 4
    }
}
New-Item -ItemType Directory -Path (Split-Path -Parent $helperSource) -Force | Out-Null
Copy-Item -LiteralPath $helper -Destination $helperSource -Force
if ((Get-Sha256 $helperSource) -cne $helperSha -or (Get-Item -LiteralPath $helperSource).Length -ne $helperBytes) {
    Fail 'Private installer helper differs from its verified receipt' 4
}

$directories = @{}
$directories[''] = 'EpoCanvaFolder'
function Ensure-Directory([string]$relative) {
    if ($relative -eq '') { return 'EpoCanvaFolder' }
    if ($directories.ContainsKey($relative)) { return $directories[$relative] }
    $parent = if ($relative.Contains('/')) { $relative.Substring(0, $relative.LastIndexOf('/')) } else { '' }
    $name = if ($relative.Contains('/')) { $relative.Substring($relative.LastIndexOf('/') + 1) } else { $relative }
    $parentId = Ensure-Directory $parent
    $id = 'dir_' + ($relative -replace '[^A-Za-z0-9]', '_')
    $directories[$relative] = $id
    return $id
}
foreach ($file in $files) {
    $parts = $file.relative.Split('/')
    if ($parts.Count -gt 1) {
        $null = Ensure-Directory (($parts[0..($parts.Count - 2)]) -join '/')
    }
}

$componentIds = [System.Collections.Generic.List[string]]::new()
$builder = New-Object System.Text.StringBuilder

$null = $builder.AppendLine('<?xml version="1.0" encoding="utf-8"?>')
$null = $builder.AppendLine('<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">')
$null = $builder.AppendLine(('  <Package Name="EpoCanva" Manufacturer="EpoCanva Project" Version="{0}" UpgradeCode="{{4AE1849E-029B-4213-8457-B756806C4FCD}}" ProductCode="*" Scope="perMachine" Compressed="yes" InstallerVersion="500" Language="1033">' -f (XmlEscape $ProductVersion)))
$null = $builder.AppendLine('    <MajorUpgrade Schedule="afterInstallInitialize" DowngradeErrorMessage="A newer EpoCanva version is already installed." />')
$null = $builder.AppendLine('    <MediaTemplate EmbedCab="yes" />')
$icon = Join-Path $RepoRoot 'platforms\windows\product\EpoCanva.ico'
if (Test-Path -LiteralPath $icon -PathType Leaf) {
    $null = $builder.AppendLine(('    <Icon Id="EpoCanvaIcon" SourceFile="{0}" />' -f (XmlEscape $icon)))
    $null = $builder.AppendLine('    <Property Id="ARPPRODUCTICON" Value="EpoCanvaIcon" />')
}
$null = $builder.AppendLine('    <StandardDirectory Id="ProgramFiles64Folder">')
$null = $builder.AppendLine('      <Directory Id="EpoCanvaFolder" Name="EpoCanva">')

# Emit the directory tree with components. Directory IDs are stable per path.
$emitDirectory = {
    param([string]$relative, [int]$indent)
    $pad = ' ' * $indent
    $id = $directories[$relative]
    if ($relative -ne '') {
        $name = $relative.Substring($relative.LastIndexOf('/') + 1)
        $null = $builder.AppendLine(('{0}<Directory Id="{1}" Name="{2}">' -f $pad, $id, (XmlEscape $name)))
    }
    $children = @($directories.Keys | Where-Object {
        $_ -ne '' -and (Split-Path -Parent ($_.Replace('/', '\'))) -eq ($relative.Replace('/', '\'))
    })
    foreach ($child in ($children | Sort-Object)) { & $emitDirectory $child ($indent + 2) }
    foreach ($file in ($files | Where-Object {
            (Split-Path -Parent ($_.relative.Replace('/', '\'))) -eq ($relative.Replace('/', '\')) })) {
        $index = [array]::IndexOf($files.ToArray(), $file)
        $componentId = "cmp_$index"
        $fileId = "fil_$index"
        $componentIds.Add($componentId)
        $null = $builder.AppendLine(('{0}<Component Id="{1}" Guid="*">' -f $pad, $componentId))
        $null = $builder.AppendLine(('{0}  <File Id="{1}" Source="{2}" KeyPath="yes" />' -f $pad, $fileId, (XmlEscape (Join-Path $sourceDir ($file.relative.Replace('/', '\'))))))
        $null = $builder.AppendLine(('{0}</Component>' -f $pad))
    }
    if ($relative -ne '') { $null = $builder.AppendLine(('{0}</Directory>' -f $pad)) }
}
& $emitDirectory '' 6
$null = $builder.AppendLine('      </Directory>')
$null = $builder.AppendLine('    </StandardDirectory>')
$null = $builder.AppendLine('    <StandardDirectory Id="CommonFiles64Folder">')
$null = $builder.AppendLine('      <Directory Id="EpoCanvaInstallerFolder" Name="EpoCanvaInstaller">')
$helperComponentId = 'cmp_tip_profile_helper'
$helperFileId = 'fil_tip_profile_helper'
$componentIds.Add($helperComponentId)
$null = $builder.AppendLine(('        <Component Id="{0}" Guid="*">' -f $helperComponentId))
$null = $builder.AppendLine(('          <File Id="{0}" Source="{1}" KeyPath="yes" />' -f $helperFileId, (XmlEscape $helperSource)))
$null = $builder.AppendLine('        </Component>')
$null = $builder.AppendLine('      </Directory>')
$null = $builder.AppendLine('    </StandardDirectory>')
$null = $builder.AppendLine('    <StandardDirectory Id="LocalAppDataFolder">')
$null = $builder.AppendLine('      <Directory Id="EpoCanvaUserDataFolder" Name="EpoCanva" />')
$null = $builder.AppendLine('    </StandardDirectory>')
$null = $builder.AppendLine('    <Feature Id="ProductFeature" Title="EpoCanva" Level="1">')
$null = $builder.AppendLine('      <ComponentGroupRef Id="ProductComponents" />')
$null = $builder.AppendLine(('      <ComponentRef Id="{0}" />' -f $helperComponentId))
$null = $builder.AppendLine('    </Feature>')
$null = $builder.AppendLine('    <ComponentGroup Id="ProductComponents">')
foreach ($componentId in $componentIds | Where-Object { $_ -cne $helperComponentId }) {
    $null = $builder.AppendLine(('      <ComponentRef Id="{0}" />' -f $componentId))
}
$null = $builder.AppendLine('    </ComponentGroup>')
# The helper invokes the installed DLL's ownership-aware DllRegisterServer/DllUnregisterServer;
# no MSI Registry rows touch the CLSID, so foreign registrations are never overwritten.
$null = $builder.AppendLine(('    <CustomAction Id="RegisterTipProfile" FileRef="{0}" ExeCommand="register &quot;[EpoCanvaFolder]epocanva_tsf.dll&quot;" Execute="deferred" Impersonate="yes" Return="check" />' -f $helperFileId))
$null = $builder.AppendLine(('    <CustomAction Id="UnregisterTipProfile" FileRef="{0}" ExeCommand="unregister &quot;[EpoCanvaFolder]epocanva_tsf.dll&quot;" Execute="deferred" Impersonate="yes" Return="check" />' -f $helperFileId))
$null = $builder.AppendLine(('    <CustomAction Id="RollbackRegisterTipProfile" FileRef="{0}" ExeCommand="unregister &quot;[EpoCanvaFolder]epocanva_tsf.dll&quot;" Execute="rollback" Impersonate="yes" Return="check" />' -f $helperFileId))
$null = $builder.AppendLine(('    <CustomAction Id="RollbackUnregisterTipProfile" FileRef="{0}" ExeCommand="register &quot;[EpoCanvaFolder]epocanva_tsf.dll&quot;" Execute="rollback" Impersonate="yes" Return="check" />' -f $helperFileId))
$null = $builder.AppendLine('    <InstallExecuteSequence>')
$null = $builder.AppendLine('      <Custom Action="RollbackUnregisterTipProfile" Before="UnregisterTipProfile" Condition="REMOVE=&quot;ALL&quot;" />')
$null = $builder.AppendLine('      <Custom Action="RollbackRegisterTipProfile" Before="RegisterTipProfile" Condition="NOT REMOVE" />')
$null = $builder.AppendLine('      <Custom Action="RegisterTipProfile" After="InstallFiles" Condition="NOT REMOVE" />')
$null = $builder.AppendLine('      <Custom Action="UnregisterTipProfile" Before="RemoveFiles" Condition="REMOVE=&quot;ALL&quot;" />')
$null = $builder.AppendLine('    </InstallExecuteSequence>')
$null = $builder.AppendLine('  </Package>')
$null = $builder.AppendLine('</Wix>')
[IO.File]::WriteAllText($wxsPath, $builder.ToString(), (New-Object System.Text.UTF8Encoding($false)))

$pendingMsi = Join-Path $runDir ([IO.Path]::GetFileName($OutputPath))
$intermediate = Join-Path $runDir 'intermediate'
New-Item -ItemType Directory -Path $intermediate | Out-Null
$buildArgs = @('build', '-arch', 'x64', '-intermediatefolder', $intermediate, '-o', $pendingMsi, $wxsPath)
$previous = $ErrorActionPreference
$savedTemp = $env:TEMP
$savedTmp = $env:TMP
Push-Location -LiteralPath $runDir
try {
    $env:TEMP = $intermediate
    $env:TMP = $intermediate
    $ErrorActionPreference = 'Continue'
    $PSNativeCommandUseErrorActionPreference = $false
    $wixVersion = (& $WixPath --version 2>&1 | Out-String).Trim()
    $probeExit = $LASTEXITCODE
    Assert-WixVersion $wixVersion $probeExit
    $buildOutput = & $WixPath @buildArgs 2>&1 | Out-String
    $buildExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $previous
    $env:TEMP = $savedTemp
    $env:TMP = $savedTmp
    Pop-Location
}
$buildLog = Join-Path $runDir 'wix-build.log'
[IO.File]::WriteAllText($buildLog, ("method=wix-build-x64`nexit=$buildExit`n" + $buildOutput), (New-Object System.Text.UTF8Encoding($false)))
if ($buildExit -ne 0 -or -not (Test-Path -LiteralPath $pendingMsi -PathType Leaf)) {
    Write-Host $buildOutput
    Fail "WiX MSI build failed with exit $buildExit" 5
}

$builtMsi = Get-InstallerFileIdentity $pendingMsi
$msiFacts = Get-InstallerMsiFacts $pendingMsi
if ($msiFacts.version -cne $ProductVersion) { Fail 'Generated MSI ProductVersion differs from the requested version.' 5 }

# Pure static extraction never invokes MSI actions or the Installer service.
# Build-only artifacts never become verified receipts.
$extractVerified = $false
$extractedFiles = 0
$extractExit = $null
$diagnoseExit = $null
$extractLog = $null
$diagnoseReport = $null
$extractLogSha256 = $null
$diagnoseSha256 = $null
if (-not $SkipExtractionCheck) {
    $msiLease = [IO.File]::OpenRead($pendingMsi)
    try {
        if ((Get-InstallerSegmentSha256 $msiLease 0 $msiLease.Length) -cne $builtMsi.sha256) {
            Fail 'MSI changed before administrative extraction.' 6
        }
        $extract = Export-InstallerMsiStatic $pendingMsi $WixPath $runDir $extractDir
        $extractExit = $extract.exit_code
        $extractLog = $extract.log.path
    } finally { $msiLease.Dispose() }
    if ($extractExit -ne 0) { Fail "Administrative extraction failed with exit $extractExit" 6 }

    # The install root is discovered as the unique directory holding
    # package.manifest; the helper must appear exactly once outside it, and nothing else
    # may exist anywhere in the extraction.
    $extractRootFull = [IO.Path]::GetFullPath($extractDir).TrimEnd([char]92, [char]47)
    $allExtracted = [System.Collections.Generic.List[object]]::new()
    $pendingDirectories = [System.Collections.Generic.Queue[string]]::new()
    $pendingDirectories.Enqueue($extractRootFull)
    while ($pendingDirectories.Count -gt 0) {
        foreach ($entry in (Get-ChildItem -LiteralPath $pendingDirectories.Dequeue() -Force)) {
            $attributes = [IO.File]::GetAttributes($entry.FullName)
            if ($attributes -band [IO.FileAttributes]::ReparsePoint) { Fail 'Extracted MSI contains a reparse point.' 6 }
            if (($attributes -band [IO.FileAttributes]::Directory) -ne 0) {
                $pendingDirectories.Enqueue($entry.FullName)
                continue
            }
            $allExtracted.Add($entry)
        }
    }
    $manifestCopies = @($allExtracted | Where-Object { $_.Name -ceq 'package.manifest' })
    if ($manifestCopies.Count -ne 1) {
        Fail ("Administrative extraction must contain exactly one package.manifest, found " + $manifestCopies.Count) 6
    }
    $installRoot = $manifestCopies[0].DirectoryName
    $outsideFiles = [System.Collections.Generic.List[object]]::new()
    foreach ($entry in $allExtracted) {
        if (-not (Test-PathWithin $entry.FullName $installRoot) -or
            [IO.Path]::GetFullPath($entry.FullName).TrimEnd([char]92, [char]47) -eq [IO.Path]::GetFullPath($installRoot).TrimEnd([char]92, [char]47)) {
            $outsideFiles.Add($entry)
        }
    }
    $helperOutside = @($outsideFiles | Where-Object { $_.Name -ceq 'epocanva_tip_profile_helper.exe' })
    if ($helperOutside.Count -ne 1 -or
        (Get-Sha256 $helperOutside[0].FullName) -cne $helperSha -or
        $helperOutside[0].Length -ne $helperBytes) {
        Fail 'The extracted helper must exist exactly once outside the install root with verified bytes' 6
    }
    if ($outsideFiles.Count -ne 1) {
        $allowedFiles = @($helperOutside[0].FullName)
        $unexpected = @($outsideFiles | Where-Object { $_.FullName -cnotin $allowedFiles })
        Fail ('Administrative extraction contains unexpected files outside the install root: ' +
            (($unexpected | ForEach-Object { $_.FullName }) -join ', ')) 6
    }

    # $files already contains package.manifest as the appended metadata row, so the
    # install-root expectation is exactly $files plus nothing else.
    $expectedRows = @($files | ForEach-Object {
        [pscustomobject]@{ path = $_.relative; bytes = $_.bytes; sha256 = $_.sha256 }
    })
    try { Assert-ExactFileSet -Root $installRoot -Expected $expectedRows }
    catch { Fail ('Administrative extraction file set is not exact: ' + $_.Exception.Message) 6 }

    # Run the extracted product's own integrity diagnosis against an isolated user root.
    $userData = Join-Path $runDir 'diagnose-user'
    New-Item -ItemType Directory -Path $userData -Force | Out-Null
    $exeCandidates = @($allExtracted | Where-Object {
        (Test-PathWithin $_.FullName $installRoot) -and $_.Name -ceq 'EpoCanva.exe' })
    if ($exeCandidates.Count -ne 1) {
        Fail ("The install root must contain exactly one EpoCanva.exe, found " + $exeCandidates.Count) 6
    }
    $diagnoseReport = Join-Path $userData 'diagnose.txt'
    $diagnoseArgs = '--diagnose "{0}" --user-data-root "{1}"' -f $diagnoseReport, (Join-Path $userData 'user-data')
    $diagnose = Start-Process -FilePath $exeCandidates[0].FullName -ArgumentList $diagnoseArgs `
        -WorkingDirectory $runDir -WindowStyle Hidden -Wait -PassThru `
        -RedirectStandardOutput (Join-Path $runDir 'extracted-diagnose.stdout.log') `
        -RedirectStandardError (Join-Path $runDir 'extracted-diagnose.stderr.log')
    $diagnoseExit = $diagnose.ExitCode
    if ($diagnoseExit -ne 0 -or -not (Test-Path -LiteralPath $diagnoseReport -PathType Leaf)) {
        Fail "Extracted EpoCanva.exe --diagnose failed with exit $diagnoseExit" 6
    }
    Assert-InstallerDiagnoseReport $diagnoseReport
    $extractLogSha256 = Get-Sha256 $extractLog
    $diagnoseSha256 = Get-Sha256 $diagnoseReport
    $extractedFiles = $allExtracted.Count
    $extractVerified = $true
}

Assert-InstallerSources $installerSource $RepoRoot 'msi'
$null = Assert-InstallerHelperReceipt $RepoRoot $helperReceiptPath $helper
if ((Get-Sha256 $manifestFile) -cne $manifestSha -or (Get-Sha256 $helperReceiptPath) -cne $helperReceiptSha -or
    (Get-Sha256 $pendingMsi) -cne $builtMsi.sha256 -or (Get-Sha256 $WixPath) -cne $wix.executable.sha256) {
    Fail 'An installer input or output changed during the build.' 6
}
$document = [ordered]@{
    format = 4
    source = $installerSource
    product_version = $versionText
    msi_product_version = $ProductVersion
    package_manifest_sha256 = $manifestSha
    package_manifest_bytes = [long]$manifestBytes
    destination_commit = $metadata['commit']
    acceptance_profile = $metadata['acceptance-profile']
    distribution_policy = $metadata['distribution-policy']
    install_directory = 'ProgramFiles64Folder/EpoCanva'
    user_data_policy = 'LocalAppData/EpoCanva; preserved on uninstall'
    builder = [ordered]@{
        name = 'WiX Toolset'
        version = $wixVersion
        path = $WixPath
        sha256 = $wix.executable.sha256
        package_sha256 = $wix.package_sha256
        source_commit = $wix.source_commit
        probe_exit = $probeExit
        build_exit = $buildExit
        build_log = $buildLog
        build_log_sha256 = Get-Sha256 $buildLog
    }
    helper = [ordered]@{
        receipt_path = $helperReceiptPath
        binary_path = $helper
        source_sha256 = $helperReceipt.source_sha256
        toolchain_sha256 = $helperReceipt.toolchain_sha256
        configuration_sha256 = $helperReceipt.configuration_sha256
        sha256 = $helperSha
        bytes = [long]$helperBytes
        receipt_sha256 = $helperReceiptSha
        installed_relative_path = 'EpoCanvaInstaller/epocanva_tip_profile_helper.exe'
    }
    installer = [ordered]@{
        path = $OutputPath
        sha256 = $builtMsi.sha256
        bytes = $builtMsi.bytes
        architecture = 'x64'
        scope = 'per-machine'
        payload_files = $packagePayloadFiles
        manifest_included = $true
        helper_included = $true
    }
    extraction = [ordered]@{ verified = $extractVerified; files = $extractedFiles; method = 'wix-msi-decompile-static';
        static_exit = $extractExit; static_log = $extractLog;
        static_log_sha256 = $extractLogSha256;
        diagnose_exit = $diagnoseExit; diagnose_report = $diagnoseReport; diagnose_report_sha256 = $diagnoseSha256 }
}
if ($extractVerified) {
    Publish-InstallerFile $pendingMsi $OutputPath
    Write-InstallerJson $manifestPath $document
} else {
    $OutputPath = $pendingMsi
    $document.installer.path = $pendingMsi
    $manifestPath = Join-Path $runDir 'installer-manifest.json'
    Write-InstallerJson $manifestPath $document
}
Write-Host "Built $OutputPath"
Write-Host "MSI SHA-256: $($builtMsi.sha256) ($($builtMsi.bytes) bytes)"
Write-Host "Manifest: $manifestPath"
exit 0
