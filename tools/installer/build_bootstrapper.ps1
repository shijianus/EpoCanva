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
. (Join-Path $PSScriptRoot 'installer_contract.ps1')

if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
$RepoRoot = (Get-Item -LiteralPath $RepoRoot).FullName
$BuildDir = Get-InstallerBuildDirectory $RepoRoot $BuildDir 'windows-product'
$installerDir = Assert-InstallerBuildPath (Join-Path $BuildDir 'installer') $BuildDir
$version = (Get-Content -LiteralPath (Join-Path $RepoRoot 'platforms\windows\product\VERSION') -Raw -Encoding UTF8).Trim()
if ($version -cnotmatch '^\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?$') { throw 'Invalid installer product version.' }
if (-not $MsiPath) { $MsiPath = Join-Path $installerDir "EpoCanva-$version-x64.msi" }
if (-not $InstallerManifest) { $InstallerManifest = Join-Path $installerDir 'installer-manifest.json' }
if (-not $OutputPath) { $OutputPath = Join-Path $installerDir "EpoCanva-$version-x64-setup.exe" }
$MsiPath = Assert-InstallerBuildPath $MsiPath $installerDir
$InstallerManifest = Assert-InstallerBuildPath $InstallerManifest $installerDir
$OutputPath = Assert-InstallerBuildPath $OutputPath $installerDir
$bootstrapperManifest = Assert-InstallerBuildPath (Join-Path $installerDir 'bootstrapper-manifest.json') $installerDir
if ([IO.Path]::GetExtension($MsiPath) -ine '.msi' -or [IO.Path]::GetExtension($OutputPath) -ine '.exe' -or
    [IO.Path]::GetDirectoryName($OutputPath) -ine $installerDir) { throw 'Invalid installer output path.' }

$msi = Assert-InstallerMsiManifest $InstallerManifest $MsiPath
$helperReceipt = (Assert-InstallerHelperReceipt $RepoRoot $msi.manifest.helper.receipt_path $msi.manifest.helper.binary_path).receipt
$expectedCompiler = $helperReceipt.toolchain.compiler_file
if ($msi.manifest.product_version -cne $version -or $msi.manifest.destination_commit -cnotmatch '^[0-9a-f]{40}$' -or
    $msi.manifest.package_manifest_sha256 -cnotmatch '^[0-9a-f]{64}$') { throw 'Invalid MSI candidate identity.' }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw 'vswhere.exe not found.' }
$installs = @(& $vswhere -all -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
$discoverExit = $LASTEXITCODE
if ($discoverExit -ne 0) { throw "Visual Studio discovery failed with exit $discoverExit." }
$vs = @($installs | Where-Object {
    $expectedCompiler.path.StartsWith($_.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -and
    (Test-Path -LiteralPath (Join-Path $_ 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'))
})
if ($vs.Count -ne 1) { throw 'No Visual Studio installation with DevShell.' }
Import-Module (Join-Path $vs[0] 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs[0] -SkipAutomaticLocation -DevCmdArguments '-arch=amd64 -host_arch=amd64' | Out-Null
$compiler = (Get-Command cl.exe -CommandType Application -ErrorAction Stop).Source
if ($compiler -ine $expectedCompiler.path -or (Get-InstallerFileIdentity $compiler).sha256 -cne $expectedCompiler.sha256) {
    throw 'Bootstrapper must use the same compiler as the verified helper.'
}
$resourceCompiler = (Get-Command rc.exe -CommandType Application -ErrorAction Stop).Source
$toolchain = [ordered]@{
    compiler = Get-InstallerFileIdentity $compiler
    linker = Get-InstallerFileIdentity (Get-Command link.exe -CommandType Application).Source
    resource_compiler = Get-InstallerFileIdentity $resourceCompiler
}
$sources = Get-InstallerSources $RepoRoot 'bootstrapper'

$runDir = New-InstallerRunDirectory $installerDir 'bootstrapper'
$image = Join-Path $runDir 'epocanva_setup_image.exe'
$source = Join-Path $PSScriptRoot 'bootstrapper.cc'
$pending = Join-Path $runDir ([IO.Path]::GetFileName($OutputPath))
$identityHeader = Join-Path $runDir 'epocanva_embedded_identity.h'
[IO.File]::WriteAllText($identityHeader, ('#define EPOCANVA_EMBEDDED_MSI_SHA256 "' + $msi.installer.sha256 + '"' + "`n"),
    [Text.UTF8Encoding]::new($false))
$versionResource = Join-Path $runDir 'version.rc'
$compiledResource = Join-Path $runDir 'version.res'
$numericVersion = $msi.manifest.msi_product_version + '.0'
$resourceVersion = $numericVersion.Replace('.', ',')
$resourceText = @"
#include <windows.h>
1 VERSIONINFO
FILEVERSION $resourceVersion
PRODUCTVERSION $resourceVersion
FILEFLAGSMASK 0x3fL
FILEFLAGS 0
FILEOS VOS_NT_WINDOWS32
FILETYPE VFT_APP
BEGIN
 BLOCK "StringFileInfo"
 BEGIN
  BLOCK "040904b0"
  BEGIN
   VALUE "CompanyName", "EpoCanva Project\0"
   VALUE "FileDescription", "EpoCanva Setup\0"
   VALUE "FileVersion", "$numericVersion\0"
   VALUE "ProductName", "EpoCanva\0"
   VALUE "ProductVersion", "$version\0"
  END
 END
 BLOCK "VarFileInfo"
 BEGIN
  VALUE "Translation", 0x409, 1200
 END
END
"@
[IO.File]::WriteAllText($versionResource, $resourceText, [Text.UTF8Encoding]::new($false))

$savedTemp = $env:TEMP
$savedTmp = $env:TMP
$savedError = $ErrorActionPreference
Push-Location -LiteralPath $runDir
try {
    $env:TEMP = $runDir
    $env:TMP = $runDir
    $ErrorActionPreference = 'Continue'
    $PSNativeCommandUseErrorActionPreference = $false
    $resourceLog = Join-Path $runDir 'resource.log'
    $resourceOutput = & $resourceCompiler /nologo ('/fo' + $compiledResource) $versionResource 2>&1 | Out-String
    $resourceExit = $LASTEXITCODE
    [IO.File]::WriteAllText($resourceLog, ("method=rc-version-resource`nexit=$resourceExit`n" + $resourceOutput), [Text.UTF8Encoding]::new($false))
    if ($resourceExit -ne 0) { throw "Bootstrapper resource compiler exit $resourceExit." }
    $compileOutput = & $compiler /nologo /std:c++17 /permissive- /utf-8 /W4 /EHsc /O2 /MT /Brepro `
        /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ('/I' + $runDir) ('/Fe' + $image) $source `
        $compiledResource /link /SUBSYSTEM:WINDOWS /MACHINE:X64 /INCREMENTAL:NO /Brepro 2>&1 | Out-String
    $compileExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $savedError
    $env:TEMP = $savedTemp
    $env:TMP = $savedTmp
    Pop-Location
}
$compileLog = Join-Path $runDir 'compile.log'
[IO.File]::WriteAllText($compileLog, $compileOutput, [Text.UTF8Encoding]::new($false))
if ($compileExit -ne 0) { throw "Bootstrapper compiler exit $compileExit. See $compileLog" }
Assert-InstallerSources $sources $RepoRoot 'bootstrapper'
foreach ($reference in @($toolchain.compiler, $toolchain.linker, $toolchain.resource_compiler)) {
    $null = Assert-InstallerFileReference $reference
}

$imageIdentity = Get-InstallerFileIdentity $image
$imageStream = [IO.File]::OpenRead($image)
try {
    $payload = [IO.File]::OpenRead($MsiPath)
    try {
        $output = [IO.File]::Open($pending, [IO.FileMode]::CreateNew)
        try {
            $imageStream.CopyTo($output)
            $header = [IO.BinaryWriter]::new($output, [Text.Encoding]::ASCII, $true)
            try {
                $header.Write([Text.Encoding]::ASCII.GetBytes('EPOINST1'))
                $header.Write([uint64]$msi.installer.bytes)
                $digest = $msi.installer.sha256
                for ($offset = 0; $offset -lt $digest.Length; $offset += 2) {
                    $header.Write([Convert]::ToByte($digest.Substring($offset, 2), 16))
                }
            } finally { $header.Dispose() }
            $payload.CopyTo($output)
        } finally { $output.Dispose() }
    } finally { $payload.Dispose() }
} finally { $imageStream.Dispose() }

$facts = Get-InstallerExeFacts $pending $MsiPath
if ($facts.image_sha256 -cne $imageIdentity.sha256 -or $facts.overlay_byte_offset -ne $imageIdentity.bytes -or
    $facts.product_version -cne $version -or $facts.file_version -cne $numericVersion -or
    $facts.embedded_msi_sha256 -cne $msi.installer.sha256 -or $facts.certificate_table_bytes -ne 0 -or
    (Get-InstallerFileIdentity $InstallerManifest).sha256 -cne $msi.reference.sha256) {
    throw 'Assembled installer differs from the built PE image or the validated MSI.'
}
$probe = Start-Process -FilePath $pending -ArgumentList '--verify' -WorkingDirectory $runDir -WindowStyle Hidden -Wait -PassThru
$probeExit = $probe.ExitCode
$verificationReport = Join-Path $runDir 'verification.json'
Write-InstallerJson $verificationReport ([ordered]@{ argument = '--verify'; exit_code = $probeExit; sha256 = $facts.sha256 })
if ($probeExit -ne 0) { throw "Bootstrapper verification-only exit $probeExit." }

Publish-InstallerFile $pending $OutputPath
$document = [ordered]@{
    format = 3
    source = $sources
    toolchain = $toolchain
    product_version = $version
    destination_commit = $msi.manifest.destination_commit
    package_manifest_sha256 = $msi.manifest.package_manifest_sha256
    msi_path = $MsiPath
    embedded_msi_sha256 = $facts.embedded_msi_sha256
    embedded_msi_bytes = $facts.embedded_msi_bytes
    image_sha256 = $facts.image_sha256
    overlay_byte_offset = $facts.overlay_byte_offset
    compiler = [ordered]@{ identity = $toolchain.compiler; exit_code = $compileExit; log = $compileLog;
        log_sha256 = (Get-InstallerFileIdentity $compileLog).sha256 }
    resource = [ordered]@{ exit_code = $resourceExit; log = Get-InstallerFileIdentity $resourceLog;
        source = Get-InstallerFileIdentity $versionResource; file_version = $numericVersion }
    verification = [ordered]@{ argument = '--verify'; exit_code = $probeExit; report = $verificationReport;
        report_sha256 = (Get-InstallerFileIdentity $verificationReport).sha256 }
    bootstrapper = [ordered]@{
        path = $OutputPath
        sha256 = $facts.sha256
        bytes = $facts.bytes
        architecture = 'x64'
        payload_magic = 'EPOINST1'
        source = 'tools/installer/bootstrapper.cc'
        installer_manifest_sha256 = $msi.reference.sha256
    }
}
Write-InstallerJson $bootstrapperManifest $document
Write-Host "Built and statically verified $OutputPath (SHA-256: $($facts.sha256))"
Write-Host "Manifest: $bootstrapperManifest"
exit 0
