# EpoCanva W1 -- configure, build and test the Windows product tree.
#
# This is the local entry point for the product work. It does not install anything, does
# not register a text service, does not write outside the repository build directory and
# never deletes an existing build tree. The VS 2022 Professional dev shell is entered
# explicitly because CMake ships with that instance and is not on PATH by default.
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$BuildDir = '',
    [string]$RimeDll = '',
    [string]$BaselineDll = '',
    [string]$Configuration = 'Release',
    [switch]$SkipTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Fail([string]$Message, [int]$Code = 1) {
    Write-Host "FAILED: $Message" -ForegroundColor Red
    exit $Code
}

if (-not $RepoRoot) {
    $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
$RepoRoot = (Get-Item -LiteralPath $RepoRoot).FullName
if (-not $BuildDir) {
    $BuildDir = Join-Path $RepoRoot 'build\windows-product'
}
if (-not $RimeDll) {
    $RimeDll = Join-Path $RepoRoot 'build\librime-english\rime\install\lib\rime.dll'
}
if (-not $BaselineDll) {
    $BaselineDll = Join-Path $RepoRoot 'build\librime-baseline\rime\install\lib\rime.dll'
}
if (-not (Test-Path -LiteralPath $RimeDll -PathType Leaf)) {
    Fail "English rime.dll not found: $RimeDll" 3
}
if (-not (Test-Path -LiteralPath $BaselineDll -PathType Leaf)) {
    $BaselineDll = ''
}

# Enter a Visual Studio developer shell. The installation is discovered, not
# hardcoded: this project needs an installation that provides both the DevShell module
# and its bundled CMake. Machine-specific facts stay in docs/reports/environment.local.md.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    Fail "vswhere.exe not found: $vswhere" 3
}
$installs = @(& $vswhere -all -products '*' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath)
$vs = $null
foreach ($candidate in $installs) {
    $devShell = Join-Path $candidate 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
    $bundledCmake = Join-Path $candidate 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if ((Test-Path -LiteralPath $devShell -PathType Leaf) -and
        (Test-Path -LiteralPath $bundledCmake -PathType Leaf)) {
        $vs = $candidate
        break
    }
}
if (-not $vs) {
    Fail 'no Visual Studio installation with DevShell and bundled CMake was found' 3
}
$devShell = Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
Import-Module $devShell
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation `
    -DevCmdArguments '-arch=amd64 -host_arch=amd64' | Out-Null
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Fail 'cmake is still not on PATH after entering the VS dev shell' 3
}

$sourceDir = Join-Path $RepoRoot 'platforms\windows\engine_bridge'
$fixtureDir = Join-Path $RepoRoot 'tests\fixtures\english_completion'
$spellingDir = Join-Path $RepoRoot 'tests\fixtures\english_spelling'
$lexicon = Join-Path $RepoRoot 'build\product-data\epocanva_en_alpha.tsv'

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$configure = @(
    '-S', $sourceDir,
    '-B', $BuildDir,
    '-G', 'Visual Studio 17 2022',
    '-A', 'x64',
    '-DEPOCANVA_BUILD_PRODUCT=ON',
    '-DEPOCANVA_BUILD_TSF_LAB=OFF',
    "-DEPOCANVA_RIME_DLL=$RimeDll",
    "-DEPOCANVA_ENGLISH_FIXTURE_DIR=$fixtureDir",
    "-DEPOCANVA_SPELLING_FIXTURE_DIR=$spellingDir",
    "-DEPOCANVA_PRODUCT_LEXICON=$lexicon"
)
if ($BaselineDll) {
    $configure += "-DEPOCANVA_BASELINE_RIME_DLL=$BaselineDll"
}

Write-Host "configure: $BuildDir"
& cmake @configure
if ($LASTEXITCODE -ne 0) { Fail "configure failed (exit $LASTEXITCODE)" 4 }

Write-Host "build: $Configuration"
& cmake --build $BuildDir --config $Configuration --parallel 8
if ($LASTEXITCODE -ne 0) { Fail "build failed (exit $LASTEXITCODE)" 5 }

if ($SkipTests) {
    Write-Host 'build only: tests skipped by request'
    exit 0
}

Write-Host 'ctest'
& ctest --test-dir $BuildDir -C $Configuration --output-on-failure --no-tests=error
$testCode = $LASTEXITCODE
if ($testCode -ne 0) {
    Write-Host "ctest failed (exit $testCode)" -ForegroundColor Red
    exit $testCode
}
Write-Host 'all product tests passed'
exit 0
