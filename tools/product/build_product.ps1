# Configure, build and test without installing or registering a text service.
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$BuildDir = '',
    [string]$RimeDll = '',
    [string]$BaselineDll = '',
    [string]$BackendResourcesRoot = '',
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [switch]$SkipTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Fail([string]$Message, [int]$Code = 1) {
    Write-Host "FAILED: $Message" -ForegroundColor Red
    exit $Code
}
function Write-Json([string]$Path, $Value) {
    [System.IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 10),
        (New-Object System.Text.UTF8Encoding($false)))
}
function Invoke-Logged([string]$Stage, [string]$Exe, [string[]]$Arguments) {
    $command = Get-Command $Exe -CommandType Application -ErrorAction Stop
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $global:LASTEXITCODE = $null
        & $command.Source @Arguments 2>&1 | ForEach-Object { $_.ToString() } |
            Tee-Object -FilePath (Join-Path $evidence "$Stage.log") | Write-Host
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previousPreference }
    if ($null -eq $code) { Fail "$Stage did not produce a process exit code" 3 }
    $commands.Add([pscustomobject]@{ stage = $Stage; executable = $Exe; arguments = $Arguments; exit_code = $code })
    Write-Json (Join-Path $evidence 'commands.json') $commands.ToArray()
    if ($code -ne 0) { Fail "$Stage failed (exit $code); later stages were not run" $code }
}

if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
$RepoRoot = (Get-Item -LiteralPath $RepoRoot).FullName
. (Join-Path $PSScriptRoot 'build_identity.ps1')
if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot 'build\windows-product' }
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir).TrimEnd('\', '/')
$buildBase = [System.IO.Path]::GetFullPath((Join-Path $RepoRoot 'build'))
if ((Split-Path -Parent $BuildDir) -ine $buildBase -or
        (Split-Path -Leaf $BuildDir) -cnotmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$' -or
        $BuildDir.EndsWith('.')) {
    Fail 'BuildDir must be exactly <repo>/build/<preset>' 2
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$receiptPath = Join-Path $BuildDir 'product-build-receipt.json'
Write-Json $receiptPath @{ format = 1; status = 'in_progress' }
if (-not $RimeDll) { $RimeDll = Join-Path $RepoRoot 'build\librime-english\rime\install\lib\rime.dll' }
if (-not $BaselineDll) { $BaselineDll = Join-Path $RepoRoot 'build\librime-baseline\rime\install\lib\rime.dll' }
foreach ($file in @($RimeDll, $BaselineDll)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { Fail "Required engine DLL not found: $file" 3 }
}
$RimeDll = (Get-Item -LiteralPath $RimeDll).FullName
$BaselineDll = (Get-Item -LiteralPath $BaselineDll).FullName
$initialRimeHash = (Get-FileHash -LiteralPath $RimeDll -Algorithm SHA256).Hash
$engineReceipt = Assert-ProductEngineReceipt -RepoRoot $RepoRoot -RimeDll $RimeDll
if (-not $BackendResourcesRoot) { $BackendResourcesRoot = Join-Path $RepoRoot 'build\product-resources\product' }
$BackendResourcesRoot = [System.IO.Path]::GetFullPath($BackendResourcesRoot)
if (-not (Test-Path -LiteralPath (Join-Path $BackendResourcesRoot 'data\backend\resources.tsv') -PathType Leaf)) {
    Fail 'Verified multilingual resources are missing. Run python -B tools/backend/stage_resources.py --product-root build/product-resources/product --oov error first.' 3
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { Fail "vswhere.exe not found: $vswhere" 3 }
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
if (-not $vs) { Fail 'No Visual Studio installation with DevShell and bundled CMake was found' 3 }
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=amd64 -host_arch=amd64' | Out-Null
$env:VSLANG = '1033'
foreach ($tool in @('cmake', 'ctest')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Fail "$tool is unavailable after entering the VS dev shell" 3 }
}
$source = Get-ProductSourceIdentity -RepoRoot $RepoRoot
$run = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssfffZ') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$evidence = Join-Path $BuildDir "verification-$run"
New-Item -ItemType Directory -Path $evidence | Out-Null
$commands = [System.Collections.Generic.List[object]]::new()
Write-Json (Join-Path $evidence 'source.json') $source
foreach ($flag in @('CL', '_CL_', 'CFLAGS', 'CXXFLAGS', 'LDFLAGS')) {
    if ([Environment]::GetEnvironmentVariable($flag)) {
        Fail "Compiler/linker environment override is not permitted for a verified build: $flag" 2
    }
}
$configure = @(
    '--fresh', '-S', (Join-Path $RepoRoot 'platforms\windows\engine_bridge'), '-B', $BuildDir,
    '-G', 'Visual Studio 17 2022', '-A', 'x64',
    "-DCMAKE_GENERATOR_INSTANCE=$vs",
    '-DEPOCANVA_BUILD_PRODUCT=ON', '-DEPOCANVA_BUILD_TSF_LAB=OFF',
    '-DEPOCANVA_BACKEND_WITH_FRENCH_DICTIONARY=ON',
    "-DEPOCANVA_PRODUCT_BACKEND_ROOT=$BackendResourcesRoot",
    '-DEPOCANVA_PRODUCT_TRACE=OFF',
    "-DEPOCANVA_PRODUCT_BUILD_ID=$($source.commit)",
    "-DEPOCANVA_RIME_DLL=$RimeDll", "-DEPOCANVA_BASELINE_RIME_DLL=$BaselineDll",
    ('-DEPOCANVA_ENGLISH_FIXTURE_DIR=' + (Join-Path $RepoRoot 'tests\fixtures\english_completion')),
    ('-DEPOCANVA_SPELLING_FIXTURE_DIR=' + (Join-Path $RepoRoot 'tests\fixtures\english_spelling')),
    ('-DEPOCANVA_PRODUCT_LEXICON=' + (Join-Path $RepoRoot 'build\product-data\epocanva_en_alpha.tsv'))
)
Invoke-Logged 'configure' 'cmake' $configure
$cachePath = Join-Path $BuildDir 'CMakeCache.txt'
$cacheLines = @(Get-Content -LiteralPath $cachePath -Encoding UTF8)
if (@($cacheLines | Where-Object { $_ -eq 'EPOCANVA_PRODUCT_TRACE:BOOL=OFF' }).Count -ne 1 -or
    @($cacheLines | Where-Object { $_ -eq "EPOCANVA_PRODUCT_BUILD_ID:STRING=$($source.commit)" }).Count -ne 1) {
    Fail 'Configured trace or source identity does not match the requested formal build' 4
}
Invoke-Logged 'build' 'cmake' @('--build', $BuildDir, '--config', $Configuration, '--parallel', '8')
if ($SkipTests) {
    Write-Json $receiptPath @{ format = 1; status = 'build_only'; source = $source }
    Write-Host 'Build completed; tests skipped and no staging receipt issued.'
    exit 0
}
$junit = Join-Path $evidence 'ctest.xml'
$previousPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $testList = @(& ctest --test-dir $BuildDir -C $Configuration --show-only=json-v1)
    $listExit = $LASTEXITCODE
} finally { $ErrorActionPreference = $previousPreference }
if ($listExit -ne 0) { Fail "CTest list export failed (exit $listExit)" $listExit }
[System.IO.File]::WriteAllText((Join-Path $evidence 'ctest-list.json'), ($testList -join "`n"))
$registered = @((($testList -join "`n") | ConvertFrom-Json).tests | ForEach-Object { $_.name })
if ($registered.Count -eq 0) { Fail 'CTest registered no tests' 8 }
foreach ($prefix in @('windows.', 'product.', 'engine_abi.', 'english.core.', 'learning.core.',
        'learning.repository.', 'chinese.core.', 'french.core.', 'composition.core.', 'language_packages.')) {
    if (@($registered | Where-Object { $_.StartsWith($prefix, [StringComparison]::Ordinal) }).Count -eq 0) {
        Fail "Mandatory test group was not registered: $prefix" 8
    }
}
Invoke-Logged 'ctest' 'ctest' @('--test-dir', $BuildDir, '-C', $Configuration,
    '--output-on-failure', '--no-tests=error', '--output-junit', $junit)
[xml]$testReport = Get-Content -LiteralPath $junit -Raw -Encoding UTF8
$cases = @($testReport.SelectNodes('//testcase'))
$skipped = @($cases | Where-Object { $null -ne $_.SelectSingleNode('skipped') })
$failed = @($cases | Where-Object { $null -ne $_.SelectSingleNode('failure') -or $null -ne $_.SelectSingleNode('error') })
if ($cases.Count -eq 0 -or $failed.Count -ne 0) { Fail 'CTest report is empty or contains failures' 8 }
$executed = @($cases | ForEach-Object { $_.GetAttribute('name') })
if ($executed.Count -ne $registered.Count -or
    @($executed | Sort-Object -Unique).Count -ne $executed.Count -or
    @(Compare-Object ($registered | Sort-Object) ($executed | Sort-Object)).Count -ne 0) {
    Fail 'CTest execution does not match the complete registered test set' 8
}
$current = Get-ProductSourceIdentity -RepoRoot $RepoRoot
if ($source.commit -ne $current.commit -or $source.source_sha256 -ne $current.source_sha256 -or
    $initialRimeHash -ne (Get-FileHash -LiteralPath $RimeDll -Algorithm SHA256).Hash) {
    Fail 'Source or runtime changed during verification; no receipt issued' 9
}
$binDirectory = 'product\bin'
if ($Configuration -eq 'Debug') { $binDirectory = 'product\bin-debug' }
$bins = @(
    @{ name = 'exe'; path = (Join-Path $BuildDir "$binDirectory\EpoCanva.exe") },
    @{ name = 'tsf'; path = (Join-Path $BuildDir "$binDirectory\epocanva_tsf.dll") },
    @{ name = 'rime'; path = $RimeDll }
)
$binaryEvidence = @($bins | ForEach-Object {
    $file = Get-Item -LiteralPath $_.path
    [pscustomobject]@{ name = $_.name; path = $file.FullName; bytes = $file.Length;
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
$receipt = [pscustomobject]@{
    format = 1; status = 'verified'; source = $source; configuration = $Configuration
    trace = $false; build_exit = 0; ctest_exit = 0; evidence = $evidence
    engine_source_sha256 = $engineReceipt.source_sha256
    cache_sha256 = (Get-FileHash -LiteralPath $cachePath -Algorithm SHA256).Hash.ToLowerInvariant()
    ctest_list_sha256 = (Get-FileHash -LiteralPath (Join-Path $evidence 'ctest-list.json') -Algorithm SHA256).Hash.ToLowerInvariant()
    ctest_xml_sha256 = (Get-FileHash -LiteralPath $junit -Algorithm SHA256).Hash.ToLowerInvariant()
    tests = @{ total = $cases.Count; passed = $cases.Count - $skipped.Count; skipped = $skipped.Count;
        failed = 0; skipped_names = @($skipped | ForEach-Object { $_.name }) }
    binaries = $binaryEvidence
}
Write-Json $receiptPath $receipt
Write-Host "Verified $($cases.Count - $skipped.Count)/$($cases.Count), skipped $($skipped.Count)."
Write-Host "Build receipt: $receiptPath"
exit 0
