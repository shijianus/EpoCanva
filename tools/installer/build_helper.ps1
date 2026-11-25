# Build the installer-only TSF profile helper from project-owned sources.
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$BuildDir = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installer_contract.ps1')

function Fail([string]$Message, [int]$Code = 1) {
    Write-Host "FAILED: $Message" -ForegroundColor Red
    exit $Code
}
if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
$RepoRoot = (Get-Item -LiteralPath $RepoRoot).FullName
$BuildDir = Get-InstallerBuildDirectory $RepoRoot $BuildDir 'installer-tools'
$receiptPath = Assert-InstallerBuildPath (Join-Path $BuildDir 'helper-build.json') $BuildDir

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { Fail 'vswhere.exe not found' 3 }
$installs = @(& $vswhere -all -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
$vswhereExit = $LASTEXITCODE
if ($vswhereExit -ne 0) { Fail "Visual Studio discovery failed with exit $vswhereExit" 3 }
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

$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$cmakeVersion = (& $cmake --version | Select-Object -First 1).Trim()
if ($LASTEXITCODE -ne 0) { Fail 'Cannot query the selected CMake version' 3 }
$compiler = Get-Command cl.exe -CommandType Application -ErrorAction Stop
$compilerVersion = (Get-Item -LiteralPath $compiler.Source).VersionInfo.FileVersion
if (-not $compilerVersion) { Fail 'Cannot determine selected compiler version' 3 }
$source = Get-InstallerSources $RepoRoot 'helper'
$toolchain = [ordered]@{
    cmake_version = $cmakeVersion
    cmake_file = Get-InstallerFileIdentity $cmake
    compiler_version = $compilerVersion
    compiler_file = Get-InstallerFileIdentity $compiler.Source
    linker_file = Get-InstallerFileIdentity (Get-Command link.exe -CommandType Application).Source
}
$configuration = 'Release;x64;CXX17;extensions=OFF;permissive=OFF;logging=OFF;runtime=MT'
$runDir = New-InstallerRunDirectory $BuildDir 'build'
$savedTemp = $env:TEMP
$savedTmp = $env:TMP
Push-Location -LiteralPath $runDir
try {
    $env:TEMP = $runDir
    $env:TMP = $runDir
    $PSNativeCommandUseErrorActionPreference = $false
    $configureLog = Join-Path $runDir 'configure.log'
    $buildLog = Join-Path $runDir 'build.log'
    & $cmake --fresh -S (Join-Path $RepoRoot 'tools\installer\helper') -B $runDir `
        -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_GENERATOR_INSTANCE=$vs" *> $configureLog
    $configureExit = $LASTEXITCODE
    if ($configureExit -ne 0) { Fail "helper configure failed with exit $configureExit" 4 }
    & $cmake --build $runDir --config Release --parallel 4 *> $buildLog
    $buildExit = $LASTEXITCODE
    if ($buildExit -ne 0) { Fail "helper build failed with exit $buildExit" 5 }
    $ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
    $testLog = Join-Path $runDir 'ctest.log'
    $testJUnit = Join-Path $runDir 'ctest.xml'
    & $ctest --test-dir $runDir -C Release --no-tests=error --output-on-failure --output-junit $testJUnit *> $testLog
    $testExit = $LASTEXITCODE
    if ($testExit -ne 0) { Fail "helper policy tests failed with exit $testExit" 6 }
    $testCases = Read-ProductPassingJUnit $testJUnit
    if ($testCases.Count -ne 1 -or $testCases[0].GetAttribute('name') -cne 'installer.helper.ownership_policy') {
        Fail 'helper policy execution is incomplete' 6
    }
} finally {
    $env:TEMP = $savedTemp
    $env:TMP = $savedTmp
    Pop-Location
}
Assert-InstallerSources $source $RepoRoot 'helper'
foreach ($reference in @($toolchain.cmake_file, $toolchain.compiler_file, $toolchain.linker_file)) {
    $null = Assert-InstallerFileReference $reference
}
$builtHelper = Join-Path $runDir 'bin\Release\epocanva_tip_profile_helper.exe'
$reader = [IO.BinaryReader]::new([IO.File]::OpenRead($builtHelper))
try { $null = Get-InstallerPeLayout $reader } finally { $reader.Dispose() }
$helper = Join-Path $BuildDir 'bin\Release\epocanva_tip_profile_helper.exe'
$null = Assert-InstallerBuildPath $helper $BuildDir
New-Item -ItemType Directory -Path (Split-Path -Parent $helper) -Force | Out-Null
Publish-InstallerFile $builtHelper $helper
$helperIdentity = Get-InstallerFileIdentity $helper
$receipt = [ordered]@{
    format = 4
    status = 'verified'
    configuration = $configuration
    configuration_sha256 = Get-InstallerTextHash $configuration
    source_sha256 = $source.sha256
    source = $source
    toolchain_sha256 = Get-InstallerTextHash ($toolchain | ConvertTo-Json -Depth 8 -Compress)
    toolchain = $toolchain
    configure_exit = $configureExit
    build_exit = $buildExit
    configure_log = Get-InstallerFileIdentity $configureLog
    build_log = Get-InstallerFileIdentity $buildLog
    test_exit = $testExit
    test_log = Get-InstallerFileIdentity $testLog
    test_junit = Get-InstallerFileIdentity $testJUnit
    binary = $helperIdentity
}
Write-InstallerJson $receiptPath $receipt
$null = Assert-InstallerHelperReceipt $RepoRoot $receiptPath $helper
Write-Host "Built $helper"
Write-Host "SHA-256: $($helperIdentity.sha256) ($($helperIdentity.bytes) bytes)"
exit 0
