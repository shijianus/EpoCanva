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
& $cmake --version | Select-Object -First 1
& $cmake -S (Join-Path $RepoRoot 'tools\installer\helper') -B $BuildDir `
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
[IO.File]::WriteAllText((Join-Path $BuildDir 'helper-build.json'),
    ((@{ format = 1; path = $helper; sha256 = $hash; bytes = $bytes } | ConvertTo-Json) + "`n"),
    (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Built $helper"
Write-Host "SHA-256: $hash ($bytes bytes)"
exit 0
