# English capability acceptance producer.
#
# Executes seven separately named cases against the tested candidate:
#   frequency, context-learning, morphology, spelling, segmentation,
#   abbreviation (the real English core algorithms), and
#   cross-app-consistency (two independent TSF product sessions in one process).
#
# The cross-app case is a TSF product-layer consistency test, not a real
# Notepad/Chromium/Word run; the separately gated real-host evidence is the only
# source that can claim real applications, and this script never writes it.
#
# Every case is a process this script actually starts. Failures are written into
# the JUnit document and the runner exits nonzero; nothing is waived.
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$BuildDir = '',
    [string]$StageRoot = '',
    [string]$EvidencePath = '',
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [switch]$SkipEvidenceWrite
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
if (-not $BuildDir) { $BuildDir = Join-Path $buildBase 'windows-product' }
$BuildDir = [IO.Path]::GetFullPath($BuildDir).TrimEnd('\', '/')
if ((Split-Path -Parent $BuildDir) -ne $buildBase) { Fail 'BuildDir must be directly below build/' 2 }
$version = (Get-Content -LiteralPath (Join-Path $RepoRoot 'platforms\windows\product\VERSION') -Raw -Encoding UTF8).Trim()
if (-not $StageRoot) { $StageRoot = Join-Path $BuildDir "stage\EpoCanva-$version" }
$StageRoot = [IO.Path]::GetFullPath($StageRoot).TrimEnd('\', '/')
if (-not $EvidencePath) { $EvidencePath = Join-Path $BuildDir 'release-evidence.json' }
$EvidencePath = [IO.Path]::GetFullPath($EvidencePath)

. (Join-Path $PSScriptRoot 'acceptance_evidence.ps1')
$identity = Get-AcceptanceIdentity -RepoRoot $RepoRoot -BuildDir $BuildDir -StageRoot $StageRoot

$englishExe = Join-Path $BuildDir "english_core_tests\$Configuration\epocanva_english_language_support_tests.exe"
$sinkExe = Join-Path $BuildDir 'product\bin\epocanva_product_sink_tests.exe'
$productDll = Join-Path $BuildDir 'product\bin\epocanva_tsf_direct_sink_test.dll'
foreach ($path in @($englishExe, $sinkExe, $productDll)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { Fail "required test binary is missing: $path" 3 }
}
$rimeDll = @($identity.receipt.binaries | Where-Object { $_.name -eq 'rime' })[0].path
if (-not (Test-Path -LiteralPath $rimeDll -PathType Leaf)) { Fail 'the tested rime.dll is missing' 3 }
$dataDir = Join-Path $RepoRoot 'data\product'
$lexicon = Join-Path $RepoRoot 'build\product-data\epocanva_en_alpha.tsv'
foreach ($path in @($dataDir, $lexicon)) {
    if (-not (Test-Path -LiteralPath $path)) { Fail "required product input is missing: $path" 3 }
}

$outputDir = Join-Path $BuildDir 'english-acceptance'
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$logPath = Join-Path $outputDir 'process.log'
$junitPath = Join-Path $outputDir 'english-acceptance.junit.xml'
$reportPath = Join-Path $outputDir 'report.json'
if (Test-Path -LiteralPath $logPath) { Remove-Item -LiteralPath $logPath -Force }

$coreCapabilities = @('frequency', 'context-learning', 'morphology', 'spelling',
                      'segmentation', 'abbreviation')
$cases = [System.Collections.Generic.List[object]]::new()
$startedAll = [DateTimeOffset]::UtcNow
foreach ($capability in $coreCapabilities) {
    $result = Invoke-AcceptanceCase -Name ("english-capabilities." + $capability) `
        -Executable $englishExe -Arguments @('--capability', $capability) `
        -LogPath $logPath -TimeoutSeconds 180
    $cases.Add($result)
}

# Two independent controlled TSF sessions race through the same per-user start
# lock. Each must retain its own text with no suppressed keys. This is the
# cross-session consistency case; it is not a real-application claim.
$runRoot = Join-Path $outputDir 'cross-app'
$crossArguments = @(
    '--product-dll', $productDll,
    '--rime-dll', $rimeDll,
    '--data-dir', $dataDir,
    '--lexicon', $lexicon,
    '--run-root', $runRoot,
    '--mode', 'multi')
$cross = Invoke-AcceptanceCase -Name 'english-capabilities.cross-app-consistency' `
    -Executable $sinkExe -Arguments $crossArguments -LogPath $logPath `
    -TimeoutSeconds 600
$cases.Add($cross)
$completedAll = [DateTimeOffset]::UtcNow
$failedCases = @($cases | Where-Object { -not $_.passed })

Write-AcceptanceJUnit -Path $junitPath -SuiteName 'english-capabilities' `
    -Cases $cases.ToArray() -Identity $identity

$report = [ordered]@{
    format = 'epocanva-english-acceptance-v1'
    version = $identity.version
    commit = $identity.commit
    source_sha256 = $identity.source_sha256
    package_manifest_sha256 = $identity.package_manifest_sha256
    acceptance_profile = $identity.acceptance_profile
    cases = @($cases | ForEach-Object {
        [ordered]@{ name = $_.name; passed = $_.passed; exit_code = $_.exit_code;
                    seconds = [math]::Round($_.seconds, 3); detail = $_.detail }
    })
    result = $(if ($failedCases.Count -eq 0) { 'passed' } else { 'failed' })
}
[IO.File]::WriteAllText($reportPath, (($report | ConvertTo-Json -Depth 8) + "`n"),
    (New-Object System.Text.UTF8Encoding($false)))

$exitCode = if ($failedCases.Count -eq 0) { 0 } else { 1 }
if (-not $SkipEvidenceWrite) {
    $runnerPath = (Get-Item -LiteralPath $PSCommandPath).FullName
    $command = @($runnerPath, '-RepoRoot', $RepoRoot, '-BuildDir', $BuildDir,
                 '-StageRoot', $StageRoot, '-EvidencePath', $EvidencePath,
                 '-Configuration', $Configuration)
    Add-AcceptanceEvidenceEntry -EvidencePath $EvidencePath -Identity $identity `
        -EntryId 'english-capabilities' -RunnerPath $runnerPath -LogPath $logPath `
        -JUnitPath $junitPath -StartedUtc $startedAll -CompletedUtc $completedAll `
        -ExitCode $exitCode -Command $command -ReportPath $reportPath
}
Write-Host "English acceptance: $($cases.Count - $failedCases.Count)/$($cases.Count) passed; exit=$exitCode"
foreach ($case in $failedCases) { Write-Host ("FAILED CASE {0}: {1}" -f $case.name, $case.detail) }
if ($exitCode -ne 0) { exit $exitCode }
exit 0
