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
function Invoke-Logged([string]$Stage, [string]$Exe, [string[]]$Arguments) {
    $command = Get-Command $Exe -CommandType Application -ErrorAction Stop | Select-Object -First 1
    $started = [DateTimeOffset]::UtcNow.ToString('o')
    $PSNativeCommandUseErrorActionPreference = $false
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $global:LASTEXITCODE = $null
        if ($Stage -ceq 'inventory') {
            $testList = @(& $command.Source @Arguments 2> (Join-Path $evidence 'inventory.stderr.log'))
            $code = $LASTEXITCODE
            [IO.File]::WriteAllText((Join-Path $evidence 'ctest-list.json'), ($testList -join "`n"))
        } else {
            & $command.Source @Arguments 2>&1 | ForEach-Object { $_.ToString() } |
                Tee-Object -FilePath (Join-Path $evidence "$Stage.log") -ErrorAction Stop | Write-Host
            $code = $LASTEXITCODE
        }
    } finally { $ErrorActionPreference = $previousPreference }
    if ($code -isnot [int]) { Fail "$Stage did not produce a process exit code" 3 }
    $commands.Add([pscustomobject]@{ stage = $Stage; executable = $command.Source; arguments = $Arguments;
        exit_code = $code; started_utc = $started; completed_utc = [DateTimeOffset]::UtcNow.ToString('o') })
    Write-ProductJson (Join-Path $evidence 'commands.json') $commands.ToArray()
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
$BuildDir = Assert-ProductPlainPath $BuildDir $buildBase
$receiptPath = Assert-ProductPlainPath (Join-Path $BuildDir 'product-build-receipt.json') $buildBase
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$lockPath = Assert-ProductPlainPath (Join-Path $BuildDir 'product-build.lock') $buildBase
$runLock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
try {
Write-ProductJson $receiptPath @{ format = 2; status = 'in_progress' }
if (-not $RimeDll) { $RimeDll = Join-Path $RepoRoot 'build\librime-english\rime\install\lib\rime.dll' }
if (-not $BaselineDll) { $BaselineDll = Join-Path $RepoRoot 'build\librime-baseline\rime\install\lib\rime.dll' }
foreach ($file in @($RimeDll, $BaselineDll)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { Fail "Required engine DLL not found: $file" 3 }
}
$RimeDll = (Get-Item -LiteralPath $RimeDll).FullName
$BaselineDll = (Get-Item -LiteralPath $BaselineDll).FullName
$initialRimeHash = Get-ProductIdentityFileSha256 -Path $RimeDll
$initialBaselineHash = Get-ProductIdentityFileSha256 -Path $BaselineDll
$engineReceiptPath = Assert-ProductPlainPath (Join-Path $RepoRoot 'build/librime-english/engine-build-receipt.json') $RepoRoot
$engineReceiptHash = Get-ProductIdentityFileSha256 $engineReceiptPath
$engineReceipt = Assert-ProductEngineReceipt -RepoRoot $RepoRoot -RimeDll $RimeDll
if (-not $BackendResourcesRoot) { $BackendResourcesRoot = Join-Path $RepoRoot 'build\product-resources\product' }
$BackendResourcesRoot = [System.IO.Path]::GetFullPath($BackendResourcesRoot)
if (-not (Test-Path -LiteralPath (Join-Path $BackendResourcesRoot 'data\backend\resources.tsv') -PathType Leaf)) {
    Fail 'Verified multilingual resources are missing. Run python -B tools/backend/stage_resources.py --product-root build/product-resources/product --oov error first.' 3
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { Fail "vswhere.exe not found: $vswhere" 3 }
$global:LASTEXITCODE = $null
$installs = @(& $vswhere -all -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
$discoveryExit = $LASTEXITCODE
if ($discoveryExit -isnot [int] -or $discoveryExit -ne 0) { Fail 'Visual Studio discovery failed' 3 }
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
Write-ProductJson (Join-Path $evidence 'source.json') $source
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
    Write-ProductJson $receiptPath @{ format = 2; status = 'build_only'; source = $source }
    Write-Host 'Build completed; tests skipped and no staging receipt issued.'
    exit 0
}
$junit = Join-Path $evidence 'ctest.xml'
$inventoryPath = Join-Path $evidence 'ctest-list.json'
Invoke-Logged 'inventory' 'ctest' @('--test-dir', $BuildDir, '-C', $Configuration, '--show-only=json-v1')
$inventory = Read-ProductTestInventory $inventoryPath
Assert-ProductRegisteredTestContract -RepoRoot $RepoRoot -BuildDir $BuildDir -Inventory $inventory
$inventoryHash = Get-ProductIdentityFileSha256 $inventoryPath
$cacheHash = Get-ProductIdentityFileSha256 $cachePath
$binDirectory = 'product\bin'
if ($Configuration -eq 'Debug') { $binDirectory = 'product\bin-debug' }
$bins = @(
    @{ name = 'exe'; path = (Join-Path $BuildDir "$binDirectory\EpoCanva.exe") },
    @{ name = 'tsf'; path = (Join-Path $BuildDir "$binDirectory\epocanva_tsf.dll") },
    @{ name = 'rime'; path = $RimeDll }
)
$binaryEvidence = @($bins | ForEach-Object {
    $path = if ($_.name -eq 'rime') { $_.path } else { Assert-ProductPlainPath $_.path $BuildDir }
    New-ProductBinaryRecord $_.name $path
})
$testBinaryEvidence = @(Get-ProductTestExecutablePaths $inventory $BuildDir | ForEach-Object {
    $path = Assert-ProductPlainPath $_ $BuildDir
    New-ProductBinaryRecord ([IO.Path]::GetFileName($path)) $path
})
Invoke-Logged 'ctest' 'ctest' @('--test-dir', $BuildDir, '-C', $Configuration,
    '--output-on-failure', '--no-tests=error', '--output-junit', $junit)
$junitHash = Get-ProductIdentityFileSha256 $junit
try {
    $cases = Read-ProductPassingJUnit $junit
    Assert-ProductTestInventory $inventory $cases
    Assert-ProductBuildCommands $commands.ToArray() $BuildDir $evidence $Configuration
} catch { Fail $_.Exception.Message 8 }
$current = Get-ProductSourceIdentity -RepoRoot $RepoRoot
if ($source.commit -cne $current.commit -or $source.source_sha256 -cne $current.source_sha256 -or
    $source.repository_sha256 -cne $current.repository_sha256 -or
    $initialRimeHash -cne (Get-ProductIdentityFileSha256 $RimeDll) -or
    $initialBaselineHash -cne (Get-ProductIdentityFileSha256 $BaselineDll) -or
    $engineReceiptHash -cne (Get-ProductIdentityFileSha256 $engineReceiptPath) -or
    $cacheHash -cne (Get-ProductIdentityFileSha256 $cachePath) -or
    $inventoryHash -cne (Get-ProductIdentityFileSha256 $inventoryPath) -or
    $junitHash -cne (Get-ProductIdentityFileSha256 $junit)) {
    Fail 'Source, build input or test evidence changed during verification' 9
}
foreach ($binary in ($binaryEvidence + $testBinaryEvidence)) {
    Assert-ProductBinaryRecord $binary $binary.name $binary.path
}
$receipt = [pscustomobject]@{
    format = 2; status = 'verified'; source = $source; configuration = $Configuration
    trace = $false; build_exit = $commands[1].exit_code; ctest_exit = $commands[3].exit_code; evidence = $evidence
    engine_source_sha256 = $engineReceipt.source_sha256
    engine_receipt_sha256 = $engineReceiptHash
    cache_sha256 = $cacheHash
    ctest_list_sha256 = $inventoryHash
    ctest_xml_sha256 = $junitHash
    commands_sha256 = Get-ProductIdentityFileSha256 -Path (Join-Path $evidence 'commands.json')
    tests = @{ total = $cases.Count; passed = $cases.Count; skipped = 0; failed = 0; skipped_names = @() }
    binaries = $binaryEvidence
    test_binaries = $testBinaryEvidence
}
Write-ProductJson $receiptPath $receipt
Write-Host "Verified $($cases.Count)/$($cases.Count), skipped 0."
Write-Host "Build receipt: $receiptPath"
exit 0
} finally { $runLock.Dispose() }
