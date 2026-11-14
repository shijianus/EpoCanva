# Build a reproducible Windows x64 per-user MSI from the verified staged package.
#
# The MSI installs the complete 66-file runtime package under
# %LOCALAPPDATA%\EpoCanva, writes only HKCU COM/category registration for the
# product CLSID, and runs the project-owned TSF profile helper to add/remove the
# en-US language profile. It does not install a service, driver, or HKLM key.
#
# The WiX source is generated into the build directory (never the source tree)
# from the package manifest, which is itself hash-bound to the tested build.
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$BuildDir = '',
    [string]$StageRoot = '',
    [string]$WixPath = '',
    [string]$OutputPath = '',
    [string]$ProductVersion = '',
    [switch]$SkipExtractionCheck
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
function ConvertTo-MsiVersion([string]$Version) {
    $match = [regex]::Match($Version, '^(\d+)\.(\d+)\.(\d+)')
    if (-not $match.Success) { return '' }
    $major = [int]$match.Groups[1].Value
    $minor = [int]$match.Groups[2].Value
    $patch = [int]$match.Groups[3].Value
    if ($major -gt 255 -or $minor -gt 255 -or $patch -gt 65535) { return '' }
    return "$major.$minor.$patch"
}
function Assert-WixVersion([string]$Version, [int]$ExitCode) {
    # Pin the complete tool build, not any major-compatible 6.x installation.
    # This checks identity output only; it is not package-origin authentication.
    if ($ExitCode -ne 0 -or $Version -cne '6.0.2+b3f3403') {
        throw 'The installer requires WiX 6.0.2+b3f3403; package provenance still requires independent verification.'
    }
}
function XmlEscape([string]$Text) {
    return [Security.SecurityElement]::Escape($Text)
}

if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
$RepoRoot = (Get-Item -LiteralPath $RepoRoot).FullName
$buildBase = [IO.Path]::GetFullPath((Join-Path $RepoRoot 'build'))
if (-not $BuildDir) { $BuildDir = Join-Path $buildBase 'windows-product' }
$BuildDir = [IO.Path]::GetFullPath($BuildDir).TrimEnd('\', '/')
if ((Split-Path -Parent $BuildDir) -ne $buildBase) { Fail 'BuildDir must be directly below build/' 2 }
$versionText = (Get-Content -LiteralPath (Join-Path $RepoRoot 'platforms\windows\product\VERSION') -Raw -Encoding UTF8).Trim()
if (-not $StageRoot) { $StageRoot = Join-Path $BuildDir "stage\EpoCanva-$versionText" }
$StageRoot = [IO.Path]::GetFullPath($StageRoot).TrimEnd('\', '/')
if (-not (Test-Path -LiteralPath (Join-Path $StageRoot 'package.manifest') -PathType Leaf)) {
    Fail 'The staged package.manifest is missing; run stage_product.ps1' 3
}
if (-not $ProductVersion) { $ProductVersion = ConvertTo-MsiVersion $versionText }
if (-not $ProductVersion) { Fail "Cannot derive a numeric MSI ProductVersion from '$versionText'; pass -ProductVersion" 2 }

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
$wixVersion = (& $WixPath --version 2>&1 | Out-String).Trim()
try { Assert-WixVersion $wixVersion $LASTEXITCODE }
catch { Fail $_.Exception.Message 3 }

# The helper is project-owned and built separately from the product package.
$helper = Join-Path $buildBase 'installer-tools\bin\Release\epocanva_tip_profile_helper.exe'
if (-not (Test-Path -LiteralPath $helper -PathType Leaf)) {
    $helper = Join-Path $buildBase 'installer-tools\bin\epocanva_tip_profile_helper.exe'
}
if (-not (Test-Path -LiteralPath $helper -PathType Leaf)) {
    Fail 'epocanva_tip_profile_helper.exe is missing; run tools/installer/build_helper.ps1' 3
}
$helper = (Get-Item -LiteralPath $helper).FullName

# Read and verify the staged package manifest against the actual files.
$metadata = @{}
$files = [System.Collections.Generic.List[object]]::new()
foreach ($line in (Get-Content -LiteralPath (Join-Path $StageRoot 'package.manifest') -Encoding UTF8)) {
    if (-not $line -or $line.StartsWith('#')) { continue }
    if ($line -cmatch '^epocanva-package-manifest [12]$') { continue }
    if ($line -cmatch '^(version|commit|acceptance-profile|distribution-policy|path-exception)=(.+)$') {
        $metadata[$Matches[1]] = $Matches[2]
    } elseif ($line -cmatch '^file\t([^\t]+)\t([0-9a-f]{64})\t(\d+)$') {
        $relative = $Matches[1]
        $hash = $Matches[2]
        $bytes = [uint64]$Matches[3]
        if ($relative.Contains('..') -or $relative.StartsWith('/') -or $relative.Contains('\')) {
            Fail "Unsafe package path: $relative" 4
        }
        $path = Join-Path $StageRoot ($relative.Replace('/', '\'))
        $file = Get-Item -LiteralPath $path -Force
        if ($file.PSIsContainer -or $file.Length -ne $bytes -or (Get-Sha256 $path) -cne $hash) {
            Fail "Staged file differs from its manifest: $relative" 4
        }
        $files.Add([pscustomobject]@{ relative = $relative; path = $file.FullName;
            sha256 = $hash; bytes = [long]$bytes })
    } else {
        Fail 'Unrecognised package manifest row' 4
    }
}
if ($files.Count -eq 0) { Fail 'The package payload is empty' 4 }
if ($metadata['version'] -cne $versionText) { Fail 'The staged package version differs from product VERSION' 4 }

$outputDir = Join-Path $BuildDir 'installer'
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
if (-not $OutputPath) { $OutputPath = Join-Path $outputDir "EpoCanva-$versionText-x64.msi" }
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$wxsPath = Join-Path $outputDir 'EpoCanva.wxs'
$extractDir = Join-Path $outputDir 'extract'
$sourceDir = Join-Path $outputDir 'source'

# Copy the staged payload and helper into a private source directory so WiX
# never reads the stage while another process could change it.
if (Test-Path -LiteralPath $sourceDir) { Remove-Item -LiteralPath $sourceDir -Recurse -Force }
New-Item -ItemType Directory -Path $sourceDir -Force | Out-Null
foreach ($file in $files) {
    $destination = Join-Path $sourceDir ($file.relative.Replace('/', '\'))
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $file.path -Destination $destination -Force
}
$helperSource = Join-Path $sourceDir 'epocanva_tip_profile_helper.exe'
Copy-Item -LiteralPath $helper -Destination $helperSource -Force

# Build the nested directory tree.
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

$clsid = '{B440F702-63E4-4D47-BCC4-074899ABA836}'
$keyboardCategory = '{34745C63-B2F0-4784-8B67-5E12C8701A31}'
$uiCategory = '{49D2F9CF-1F5E-11D7-A6D3-00065B84435C}'
$componentIds = [System.Collections.Generic.List[string]]::new()
$builder = New-Object System.Text.StringBuilder
$null = $builder.AppendLine('<?xml version="1.0" encoding="utf-8"?>')
$null = $builder.AppendLine('<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">')
$null = $builder.AppendLine(('  <Package Name="EpoCanva" Manufacturer="EpoCanva Project" Version="{0}" UpgradeCode="{{4AE1849E-029B-4213-8457-B756806C4FCD}}" ProductCode="*" Scope="perUser" Compressed="yes" InstallerVersion="500" Language="1033">' -f (XmlEscape $ProductVersion)))
$null = $builder.AppendLine('    <MajorUpgrade DowngradeErrorMessage="A newer EpoCanva version is already installed." />')
$null = $builder.AppendLine('    <MediaTemplate EmbedCab="yes" />')
$icon = Join-Path $RepoRoot 'platforms\windows\product\EpoCanva.ico'
if (Test-Path -LiteralPath $icon -PathType Leaf) {
    $null = $builder.AppendLine(('    <Icon Id="EpoCanvaIcon" SourceFile="{0}" />' -f (XmlEscape $icon)))
    $null = $builder.AppendLine('    <Property Id="ARPPRODUCTICON" Value="EpoCanvaIcon" />')
}
$null = $builder.AppendLine('    <StandardDirectory Id="LocalAppDataFolder">')
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
        if ($file.relative -ceq 'epocanva_tsf.dll') {
            $null = $builder.AppendLine(('{0}  <RegistryKey Root="HKCU" Key="Software\Classes\CLSID\{1}" ForceCreateOnInstall="yes" ForceDeleteOnUninstall="yes">' -f $pad, $clsid))
            $null = $builder.AppendLine(('{0}    <RegistryValue Name="EpoCanvaProduct" Type="string" Value="epocanva_tsf" KeyPath="no" />' -f $pad))
            $null = $builder.AppendLine(('{0}    <RegistryKey Key="InprocServer32" ForceCreateOnInstall="yes" ForceDeleteOnUninstall="yes">' -f $pad))
            $null = $builder.AppendLine(('{0}      <RegistryValue Type="string" Value="[EpoCanvaFolder]epocanva_tsf.dll" KeyPath="no" />' -f $pad))
            $null = $builder.AppendLine(('{0}      <RegistryValue Name="ThreadingModel" Type="string" Value="Apartment" KeyPath="no" />' -f $pad))
            $null = $builder.AppendLine(('{0}    </RegistryKey>' -f $pad))
            $null = $builder.AppendLine(('{0}    <RegistryKey Key="Implemented Categories\{1}" ForceCreateOnInstall="yes" ForceDeleteOnUninstall="yes" />' -f $pad, $keyboardCategory))
            $null = $builder.AppendLine(('{0}    <RegistryKey Key="Implemented Categories\{1}" ForceCreateOnInstall="yes" ForceDeleteOnUninstall="yes" />' -f $pad, $uiCategory))
            $null = $builder.AppendLine(('{0}  </RegistryKey>' -f $pad))
        }
        $null = $builder.AppendLine(('{0}</Component>' -f $pad))
    }
    if ($relative -ne '') { $null = $builder.AppendLine(('{0}</Directory>' -f $pad)) }
}
& $emitDirectory '' 6

# The helper is an installer-only component; it is not part of the 66-file
# runtime package and is removed on uninstall.
$helperComponentId = 'cmp_tip_profile_helper'
$helperFileId = 'fil_tip_profile_helper'
$componentIds.Add($helperComponentId)
$null = $builder.AppendLine(('      <Component Id="{0}" Guid="*">' -f $helperComponentId))
$null = $builder.AppendLine(('        <File Id="{0}" Source="{1}" KeyPath="yes" />' -f $helperFileId, (XmlEscape $helperSource)))
$null = $builder.AppendLine('      </Component>')

$null = $builder.AppendLine('      </Directory>')
$null = $builder.AppendLine('    </StandardDirectory>')
$null = $builder.AppendLine('    <Feature Id="ProductFeature" Title="EpoCanva" Level="1">')
$null = $builder.AppendLine('      <ComponentGroupRef Id="ProductComponents" />')
$null = $builder.AppendLine('    </Feature>')
$null = $builder.AppendLine('    <ComponentGroup Id="ProductComponents">')
foreach ($componentId in $componentIds) {
    $null = $builder.AppendLine(('      <ComponentRef Id="{0}" />' -f $componentId))
}
$null = $builder.AppendLine('    </ComponentGroup>')
$null = $builder.AppendLine(('    <CustomAction Id="RegisterTipProfile" FileRef="{0}" ExeCommand="register" Execute="deferred" Impersonate="yes" Return="check" />' -f $helperFileId))
$null = $builder.AppendLine(('    <CustomAction Id="UnregisterTipProfile" FileRef="{0}" ExeCommand="unregister" Execute="deferred" Impersonate="yes" Return="ignore" />' -f $helperFileId))
$null = $builder.AppendLine('    <InstallExecuteSequence>')
$null = $builder.AppendLine('      <Custom Action="RegisterTipProfile" After="InstallFiles" Condition="NOT REMOVE" />')
$null = $builder.AppendLine('      <Custom Action="UnregisterTipProfile" Before="RemoveFiles" Condition="REMOVE=&quot;ALL&quot;" />')
$null = $builder.AppendLine('    </InstallExecuteSequence>')
$null = $builder.AppendLine('  </Package>')
$null = $builder.AppendLine('</Wix>')
[IO.File]::WriteAllText($wxsPath, $builder.ToString(), (New-Object System.Text.UTF8Encoding($false)))

# Build the MSI. WiX 6 selects the x64 payload architecture explicitly.
if (Test-Path -LiteralPath $OutputPath) { Remove-Item -LiteralPath $OutputPath -Force }
$buildArgs = @('build', '-arch', 'x64', '-o', $OutputPath, $wxsPath)
$previous = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $buildOutput = & $WixPath @buildArgs 2>&1 | Out-String
    $buildExit = $LASTEXITCODE
} finally { $ErrorActionPreference = $previous }
$buildLog = Join-Path $outputDir 'wix-build.log'
[IO.File]::WriteAllText($buildLog, $buildOutput, (New-Object System.Text.UTF8Encoding($false)))
if ($buildExit -ne 0 -or -not (Test-Path -LiteralPath $OutputPath -PathType Leaf)) {
    Write-Host $buildOutput
    Fail "WiX MSI build failed with exit $buildExit" 5
}

$msiHash = Get-Sha256 $OutputPath
$msiBytes = (Get-Item -LiteralPath $OutputPath).Length

# Administrative extraction is a read-only verification of the MSI database and
# payload; it does not install or register anything. It is intentionally skipped
# only when the caller asks for a build-only artifact.
$extractVerified = $false
$extractedFiles = 0
if (-not $SkipExtractionCheck) {
    if (Test-Path -LiteralPath $extractDir) { Remove-Item -LiteralPath $extractDir -Recurse -Force }
    New-Item -ItemType Directory -Path $extractDir -Force | Out-Null
    $extractLog = Join-Path $outputDir 'msiexec-extract.log'
    $extractArguments = "/a `"$OutputPath`" /qn TARGETDIR=`"$extractDir`" /l*v `"$extractLog`""
    $extract = Start-Process -FilePath "$env:SystemRoot\System32\msiexec.exe" -ArgumentList $extractArguments -Wait -PassThru -NoNewWindow
    if ($extract.ExitCode -ne 0) { Fail "Administrative extraction failed with exit $($extract.ExitCode)" 6 }
    $extracted = @(Get-ChildItem -LiteralPath $extractDir -Recurse -File -Force)
    $extractedFiles = $extracted.Count
    $byHash = @{}
    foreach ($item in $extracted) { $byHash[(Get-Sha256 $item.FullName)] = $item.FullName }
    foreach ($file in $files) {
        if (-not $byHash.ContainsKey($file.sha256)) {
            Fail "Extracted MSI payload is missing or differs: $($file.relative)" 6
        }
    }
    $helperHash = Get-Sha256 $helperSource
    if (-not $byHash.ContainsKey($helperHash)) { Fail 'Extracted MSI payload is missing the TSF profile helper' 6 }
    $extractVerified = $true
}

$manifestPath = Join-Path $outputDir 'installer-manifest.json'
$document = [ordered]@{
    format = 1
    product_version = $versionText
    msi_product_version = $ProductVersion
    package_manifest_sha256 = Get-Sha256 (Join-Path $StageRoot 'package.manifest')
    destination_commit = $metadata['commit']
    acceptance_profile = $metadata['acceptance-profile']
    distribution_policy = $metadata['distribution-policy']
    builder = [ordered]@{
        name = 'WiX Toolset'
        version = $wixVersion
        path = $WixPath
        sha256 = Get-Sha256 $WixPath
    }
    installer = [ordered]@{
        path = $OutputPath
        sha256 = $msiHash
        bytes = [long]$msiBytes
        architecture = 'x64'
        scope = 'per-user'
        payload_files = $files.Count
        helper_included = $true
    }
    extraction = [ordered]@{ verified = $extractVerified; files = $extractedFiles }
}
[IO.File]::WriteAllText($manifestPath, (($document | ConvertTo-Json -Depth 8) + "`n"),
    (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Built $OutputPath"
Write-Host "MSI SHA-256: $msiHash ($msiBytes bytes)"
Write-Host "Manifest: $manifestPath"
exit 0
