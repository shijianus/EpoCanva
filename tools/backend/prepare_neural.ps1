<#!
.SYNOPSIS
  Prepare and verify the optional CPU-only neural reranker.
.DESCRIPTION
  This entry point never downloads source, wheels, models, or user content. It
  configures and builds only engine/neural against the already acquired pinned
  llama.cpp source under build/backend-model-acquire, then runs the bounded
  real-model and intentional-failure tests. All generated files stay beneath
  build/backend-neural. The module is optional and is not added to the product
  root build.
#>
[CmdletBinding()]
param(
  [string]$RepoRoot,
  [string]$BuildRoot,
  [string]$ModelPath,
  [string]$Python = 'python.exe',
  [switch]$SkipTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
  $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
} else {
  $RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
}
if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
  $BuildRoot = Join-Path $RepoRoot 'build\backend-neural'
}
$BuildRoot = [System.IO.Path]::GetFullPath($BuildRoot)
$ownedRoot = (Join-Path $RepoRoot 'build\backend-neural').Replace('\','/').ToLowerInvariant().TrimEnd('/')
$requestedRoot = $BuildRoot.Replace('\','/').ToLowerInvariant().TrimEnd('/')
if ($requestedRoot -ne $ownedRoot -and -not $requestedRoot.StartsWith($ownedRoot + '/')) {
  throw "BuildRoot must remain under <repo>/build/backend-neural: $BuildRoot"
}
if ([string]::IsNullOrWhiteSpace($ModelPath)) {
  $ModelPath = Join-Path $BuildRoot 'models\qwen2.5-0.5b-base-q8_0.gguf'
}
$ModelPath = [System.IO.Path]::GetFullPath($ModelPath)
$expectedModelBytes = [uint64]531067776
$expectedModelSha256 = '4b965ca8e4a95be38f3223bbb512cc73112efaef00c0313b7e618f0835c504c0'
$logRoot = Join-Path $BuildRoot 'logs'
$buildDir = Join-Path $BuildRoot 'cpu'
New-Item -ItemType Directory -Force -Path $BuildRoot, $logRoot | Out-Null

function Invoke-Checked {
  param(
    [Parameter(Mandatory)][string]$Name,
    [Parameter(Mandatory)][string]$Executable,
    [Parameter()][string[]]$Arguments = @()
  )
  $log = Join-Path $logRoot ($Name + '.log')
  $lines = @(
    "name=$Name"
    "executable=$Executable"
    'arguments='
  ) + @($Arguments | ForEach-Object { "  |$_" })
  $old = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    $output = & $Executable @Arguments 2>&1 | ForEach-Object { $_.ToString() }
    $code = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $old
  }
  ($lines + "exit_code=$code" + '' + $output) | Set-Content -LiteralPath $log -Encoding UTF8
  if ($code -ne 0) {
    Write-Host "FAILED $Name exit=$code log=$log" -ForegroundColor Red
    $output | Select-Object -Last 40 | ForEach-Object { Write-Host $_ }
    throw "${Name} failed with exit code $code"
  }
  Write-Host "OK $Name exit=0 log=$log"
  return $output
}

if (-not (Test-Path -LiteralPath $ModelPath -PathType Leaf)) {
  throw "Model resource not found; acquisition is not performed by this script: $ModelPath"
}
$modelItem = Get-Item -LiteralPath $ModelPath
if ([uint64]$modelItem.Length -ne $expectedModelBytes) {
  throw "Model byte count mismatch: observed $($modelItem.Length), expected $expectedModelBytes"
}
$modelHash = (Get-FileHash -LiteralPath $ModelPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($modelHash -ne $expectedModelSha256) {
  throw "Model SHA-256 mismatch: observed $modelHash, expected $expectedModelSha256"
}
if ([string]::IsNullOrWhiteSpace($Python)) {
  throw 'Python must name an existing interpreter; no dependency installation is attempted.'
}
[pscustomobject]@{
  model_path = $ModelPath
  model_bytes = [uint64]$modelItem.Length
  model_sha256 = $modelHash
  source_root = (Join-Path $RepoRoot 'build\backend-model-acquire\llama.cpp-b4514\source')
  offline = $true
  network_downloads = $false
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $logRoot 'model-verification.json') -Encoding UTF8

$source = Join-Path $RepoRoot 'build\backend-model-acquire\llama.cpp-b4514\source'
if (-not (Test-Path -LiteralPath (Join-Path $source 'CMakeLists.txt') -PathType Leaf)) {
  throw "Pinned llama.cpp source is missing; acquisition is not performed by this script: $source"
}
$neuralSource = Join-Path $RepoRoot 'engine\neural'
if (-not (Test-Path -LiteralPath (Join-Path $neuralSource 'CMakeLists.txt') -PathType Leaf)) {
  throw "Optional neural source is missing: $neuralSource"
}

# Validate the archive, Git tree metadata, and every extracted file before CMake
# can execute any source-provided configure logic. No Python packages are needed.
Invoke-Checked 'verify-pinned-sources' $Python @('-B', (Join-Path $neuralSource 'verify_resources.py'), '--repo', $RepoRoot) | Out-Null

# Keep process-local temporary paths inside this build; do not modify system PATH.
$tmp = Join-Path $BuildRoot 'tmp'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$env:TMP = $tmp
$env:TEMP = $tmp
$env:PYTHONDONTWRITEBYTECODE = '1'
# Prevent upstream tarball build-info from accidentally reading the parent repo's HEAD.
$env:GIT_CEILING_DIRECTORIES = $source

$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
if ($null -eq $cmake) {
  throw 'cmake.exe is not on PATH; start this script from an existing VS Developer PowerShell. No tool installation is attempted.'
}
$configureArgs = @(
  '-S', $neuralSource,
  '-B', $buildDir,
  '-G', 'Ninja',
  '-DCMAKE_BUILD_TYPE=Release',
  '-DCMAKE_CXX_STANDARD=17',
  '-DCMAKE_CXX_EXTENSIONS=OFF',
  ('-DNEURAL_MODEL_PATH=' + $ModelPath.Replace('\','/')),
  ('-DLLAMA_SOURCE=' + $source.Replace('\','/'))
)
Invoke-Checked 'configure-cpu' $cmake.Source $configureArgs | Out-Null
$cache = Get-Content -LiteralPath (Join-Path $buildDir 'CMakeCache.txt')
foreach ($key in @('BUILD_SHARED_LIBS','LLAMA_BUILD_COMMON','LLAMA_BUILD_EXAMPLES','LLAMA_BUILD_SERVER','LLAMA_BUILD_TESTS','LLAMA_CURL','GGML_RPC','GGML_BACKEND_DL','GGML_CUDA','GGML_HIP','GGML_SYCL','GGML_VULKAN','GGML_OPENCL','GGML_BLAS','GGML_KOMPUTE','GGML_METAL','GGML_MUSA','GGML_CANN','GGML_OPENMP','GGML_LLAMAFILE')) {
  if (-not ($cache -match ('^' + $key + ':BOOL=OFF$'))) { throw "Unexpected CPU/offline cache value: $key" }
}
if (-not ($cache -match '^GGML_CPU:BOOL=ON$')) { throw 'CPU backend is not enabled.' }
Invoke-Checked 'build-cpu' $cmake.Source @('--build', $buildDir, '--target', 'neural_reranker_tests', '--parallel', '6') | Out-Null
$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if ($null -eq $dumpbin) { throw 'dumpbin.exe missing from existing developer environment.' }
$testExe = Join-Path $buildDir 'neural_reranker_tests.exe'
$imports = Invoke-Checked 'imports-cpu' $dumpbin.Source @('/imports', $testExe)
if (($imports -join "`n") -match '(?i)ws2_32|wsock32|winhttp|wininet|libcurl|ggml-rpc|cuda|opencl') {
  throw 'Unexpected networking or GPU dependency in test executable import table.'
}

if (-not $SkipTests) {
  $ctest = Get-Command ctest.exe -ErrorAction SilentlyContinue
  if ($null -eq $ctest) { throw 'ctest.exe is not on PATH; no tool installation is attempted.' }
  Invoke-Checked 'ctest-cpu' $ctest.Source @('--test-dir', $buildDir, '--output-on-failure', '--no-tests=error') | Out-Null
}

$receipt = [ordered]@{
  status = if ($SkipTests) { 'build_only' } else { 'tests_passed' }
  model_path = $ModelPath
  model_bytes = [uint64]$modelItem.Length
  model_sha256 = $modelHash
  build_root = $BuildRoot
  build_directory = $buildDir
  source_directory = $source
  llama_revision = 'b4514 / ec7f3ac9ab33e46b136eb5ab6a76c4d81f57c7f1'
  model_revision = 'Qwen/Qwen2.5-0.5B@060db6499f32faf8b98477b0a26969ef7d8b9987'
  cpu_only = $true
  cuda = $false
  rpc = $false
  curl = $false
  server = $false
  downloads_performed = $false
  default_admission = $false
  latency_gate_met = $false
  tests_skipped = [bool]$SkipTests
  test_executable_sha256 = (Get-FileHash -LiteralPath $testExe -Algorithm SHA256).Hash.ToLowerInvariant()
  cmake_cache_sha256 = (Get-FileHash -LiteralPath (Join-Path $buildDir 'CMakeCache.txt') -Algorithm SHA256).Hash.ToLowerInvariant()
}
$receipt | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $BuildRoot 'neural-build-receipt.json') -Encoding UTF8
Write-Host "Neural preparation complete: $($receipt.status)"
exit 0
