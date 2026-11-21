# Build the installer-only TSF profile helper from project-owned sources.
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$BuildDir = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Fail([string]$Message, [int]$Code = 1) {
    Write-Host "FAILED: $Message" -ForegroundColor Red
    exit $Code
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

$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
$compiler = Get-Command cl.exe -CommandType Application -ErrorAction Stop
$cmakeVersion = (& $cmake --version | Select-Object -First 1).Trim()
if ($LASTEXITCODE -ne 0) { Fail 'Cannot query the selected CMake version' 3 }
$compiler = Get-Command cl.exe -CommandType Application -ErrorAction Stop
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudioVersion = (& $vswhere -path $vs -property catalog_productDisplayVersion | Select-Object -First 1).Trim()
if ($LASTEXITCODE -ne 0 -or -not $visualStudioVersion) { Fail 'Cannot determine selected Visual Studio toolchain version' 3 }
$compilerVersion = "Visual Studio $visualStudioVersion; compiler identity sha256-bound"
$cmakeHash = (Get-FileHash -LiteralPath $cmake -Algorithm SHA256).Hash.ToLowerInvariant()
$compilerHash = (Get-FileHash -LiteralPath $compiler.Source -Algorithm SHA256).Hash.ToLowerInvariant()
$sourcePaths = @(
    (Join-Path $RepoRoot 'tools\product\isolation\tip_profile_helper.cc'),
    (Join-Path $RepoRoot 'platforms\windows\tsf\identifiers.cc'),
    (Join-Path $RepoRoot 'platforms\windows\tsf\identifiers.h'),
    (Join-Path $RepoRoot 'tools\installer\helper\CMakeLists.txt'))
$sourceRows = @($sourcePaths | ForEach-Object {
    $item = Get-Item -LiteralPath $_ -ErrorAction Stop
    [pscustomobject]@{ path = $item.FullName.Substring($RepoRoot.Length + 1).Replace('\', '/');
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant();
        bytes = [long]$item.Length }
})
$sourceBody = ($sourceRows | ForEach-Object { "$($_.path)`t$($_.sha256)`t$($_.bytes)" }) -join "`n"
$algorithm = [Security.Cryptography.SHA256]::Create()
try {
    $sourceIdentity = ([BitConverter]::ToString($algorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes($sourceBody + "`n")))).Replace('-', '').ToLowerInvariant()
    $toolchainBody = "$cmakeVersion`n$cmakeHash`n$($compiler.Source)`n$compilerHash`n$compilerVersion"
    $toolchainIdentity = ([BitConverter]::ToString($algorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes($toolchainBody)))).Replace('-', '').ToLowerInvariant()
    $configuration = 'Release;x64;CXX17;extensions=OFF;permissive=OFF;logging=OFF'
    $configurationHash = ([BitConverter]::ToString($algorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes($configuration)))).Replace('-', '').ToLowerInvariant()
} finally { $algorithm.Dispose() }
& $cmake --fresh -S (Join-Path $RepoRoot 'tools\installer\helper') -B $BuildDir `
    -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_GENERATOR_INSTANCE=$vs"
if ($LASTEXITCODE -ne 0) { Fail "helper configure failed with exit $LASTEXITCODE" 4 }
& $cmake --build $BuildDir --config Release --target epocanva_tip_profile_helper --parallel 4
if ($LASTEXITCODE -ne 0) { Fail "helper build failed with exit $LASTEXITCODE" 5 }
$helper = Join-Path $BuildDir 'bin\Release\epocanva_tip_profile_helper.exe'
if (-not (Test-Path -LiteralPath $helper -PathType Leaf)) {
    $helper = Join-Path $BuildDir 'bin\epocanva_tip_profile_helper.exe'
}
if (-not (Test-Path -LiteralPath $helper -PathType Leaf)) { Fail 'helper output is missing after a successful build' 5 }
$hash = (Get-FileHash -LiteralPath $helper -Algorithm SHA256).Hash.ToLowerInvariant()
$bytes = (Get-Item -LiteralPath $helper).Length
$receipt = [ordered]@{
    format = 2
    status = 'verified'
    configuration = $configuration
    configuration_sha256 = $configurationHash
    source_sha256 = $sourceIdentity
    sources = $sourceRows
    toolchain_sha256 = $toolchainIdentity
    toolchain = [ordered]@{
        cmake = $cmakeVersion
        cmake_sha256 = $cmakeHash
        compiler_path = $compiler.Source
        compiler_sha256 = $compilerHash
        compiler_version = $compilerVersion
    }
    build_exit = 0
    binary = [ordered]@{ path = $helper; sha256 = $hash; bytes = [long]$bytes }
}
[IO.File]::WriteAllText((Join-Path $BuildDir 'helper-build.json'),
    (($receipt | ConvertTo-Json -Depth 8) + "`n"), (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Built $helper"
Write-Host "SHA-256: $hash ($bytes bytes)"
exit 0
