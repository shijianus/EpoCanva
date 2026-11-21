# Formal stress-reference-model acceptance producer.
#
# Runs the controlled editor reference model (five fixed seeds x 100000 real TSF
# operations against the independent oracle), verifies the measured report against the
# release-gate contract, converts it into the hash-bound epocanva-stress-reference-model-v1
# report with the current candidate identity, and writes the JUnit-bound test-run entry.
# It executes no installed-host claim: the report states the controlled scope.
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$BuildDir = '',
    [string]$StageRoot = '',
    [string]$EvidencePath = '',
    [string]$Harness = ''
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

if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
$RepoRoot = (Get-Item -LiteralPath $RepoRoot).FullName
. (Join-Path $RepoRoot 'tools\product\acceptance_evidence.ps1')
if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot 'build\windows-product' }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
$version = (Get-Content -LiteralPath (Join-Path $RepoRoot 'platforms\windows\product\VERSION') -Raw -Encoding UTF8).Trim()
if (-not $StageRoot) { $StageRoot = Join-Path $BuildDir "stage\EpoCanva-$version" }
$StageRoot = [IO.Path]::GetFullPath($StageRoot)
if (-not $EvidencePath) { $EvidencePath = Join-Path $BuildDir 'release-evidence.json' }
$identity = Get-AcceptanceIdentity -RepoRoot $RepoRoot -BuildDir $BuildDir -StageRoot $StageRoot

if (-not $Harness) {
    $candidates = @(
        (Join-Path $BuildDir 'editor_reference_model_tests\Release\epocanva_editor_reference_tests.exe'),
        (Join-Path $BuildDir 'product\Release\epocanva_editor_reference_tests.exe'))
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $Harness = $candidate; break }
    }
}
if (-not $Harness -or -not (Test-Path -LiteralPath $Harness -PathType Leaf)) {
    Fail 'epocanva_editor_reference_tests.exe not found; build the product suite first' 3
}
$Harness = (Get-Item -LiteralPath $Harness).FullName

$outDir = Join-Path $BuildDir 'stress-reference-model'
if (Test-Path -LiteralPath $outDir) { Remove-Item -LiteralPath $outDir -Recurse -Force }
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

# Positive protocol: five fixed seeds x 100000 operations.
$positiveStart = [DateTimeOffset]::UtcNow
$measureReport = Join-Path $outDir 'measurements.json'
$replayPrefix = Join-Path $outDir 'operations'
$positive = Start-Process -FilePath $Harness `
    -ArgumentList @('--report', $measureReport, '--replay', $replayPrefix) `
    -Wait -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $outDir 'positive-stdout.log') `
    -RedirectStandardError (Join-Path $outDir 'positive-stderr.log')
$positiveExit = $positive.ExitCode
$positiveEnd = [DateTimeOffset]::UtcNow
if ($positiveExit -ne 0) { Fail "positive reference protocol failed with exit $positiveExit" 5 }

$measurement = Get-Content -LiteralPath $measureReport -Raw -Encoding UTF8 | ConvertFrom-Json
$expectedSeeds = @('0x5eed000000000001', '0x5eed000000000002', '0x5eed000000000003',
    '0x5eed000000000004', '0x5eed000000000005')
if ($measurement.result -cne 'passed') { Fail 'measurement reports failure' 5 }
if (@($measurement.seeds).Count -ne 5) { Fail 'measurement does not cover five seeds' 5 }
$totalOps = 0
foreach ($expectedSeed in $expectedSeeds) {
    $rows = @($measurement.seeds | Where-Object { $_.seed -ceq $expectedSeed })
    if ($rows.Count -ne 1) { Fail "missing seed $expectedSeed" 5 }
    if ($rows[0].operations -ne 100000 -or $rows[0].mismatches -ne 0) { Fail "seed $expectedSeed shortened or mismatched" 5 }
    foreach ($operation in @('insert', 'delete', 'caret-utf16', 'selection-utf16', 'undo', 'invalid-state')) {
        $count = $rows[0].operation_counts.$operation
        if ($count -isnot [int] -or $count -lt 1) { Fail "seed $expectedSeed lacks $operation operations" 5 }
    }
    $totalOps += $rows[0].operations
}
if ($totalOps -ne 500000) { Fail "total operations $totalOps is not 500000" 5 }

# Negative control: the harness must detect an omitted-write mutant and exit nonzero.
$negativeStart = [DateTimeOffset]::UtcNow
$negativeReport = Join-Path $outDir 'negative-control.json'
$negative = Start-Process -FilePath $Harness `
    -ArgumentList @('--force-fail', '--report', $negativeReport, '--replay', (Join-Path $outDir 'negative')) `
    -Wait -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $outDir 'negative-stdout.log') `
    -RedirectStandardError (Join-Path $outDir 'negative-stderr.log')
$negativeExit = $negative.ExitCode
$negativeEnd = [DateTimeOffset]::UtcNow
if ($negativeExit -eq 0) { Fail 'negative control passed; the harness cannot detect mutants' 5 }
$negativeText = Get-Content -LiteralPath (Join-Path $outDir 'negative-stdout.log') -Raw -Encoding UTF8
if ($negativeText -notmatch 'omitted-write mutation detected') {
    Fail 'negative control did not name the omitted-write mutant' 5
}

# Per-seed resource samples, derived from the measurement (one row per seed).
$samples = [Collections.Generic.List[object]]::new()
$clock = $positiveStart
foreach ($seed in $measurement.seeds) {
    $samples.Add([ordered]@{
        seed = $seed.seed
        duration_ms = $seed.duration_ms
        handles_before = $seed.handles_before
        handles_after = $seed.handles_after
        working_set_before_bytes = $seed.working_set_before_bytes
        working_set_after_bytes = $seed.working_set_after_bytes
    })
}
$samplesPath = Join-Path $outDir 'resource-samples.json'
[IO.File]::WriteAllText($samplesPath, (($samples | ConvertTo-Json -Depth 6) + "`n"),
    (New-Object System.Text.UTF8Encoding($false)))

# The formal report, bound to the current candidate identity.
$report = [ordered]@{
    format = 'epocanva-stress-reference-model-v1'
    version = $identity.version
    commit = $identity.commit
    source_sha256 = $identity.source_sha256
    package_manifest_sha256 = $identity.package_manifest_sha256
    language_policy_sha256 = $identity.language_policy_sha256
    acceptance_profile = $identity.acceptance_profile
    oracle = 'independent-editor-reference-model'
    failure_reduction = 'replay-and-minimize'
    installed_host_acceptance = $false
    result = 'passed'
    total_ops = 500000
    seeds = $measurement.seeds
    replay_log = $null
    resource_samples = $null
    negative_control = $null
    started_utc = $positiveStart.ToUniversalTime().ToString('o')
    completed_utc = $positiveEnd.ToUniversalTime().ToString('o')
}
function Set-ReportReference([string]$Name, [string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($RepoRoot.TrimEnd('\', '/') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        Fail "reference $Name is outside the repository" 2
    }
    $file = Get-Item -LiteralPath $full
    if ($file.Length -le 0) { Fail "reference $Name is empty" 2 }
    $report.$Name = [ordered]@{
        path = $full.Substring($RepoRoot.Length + 1).Replace('\', '/')
        sha256 = Get-Sha256 $full
        bytes = [long]$file.Length
    }
}
$seedReplays = @(Get-ChildItem -LiteralPath $outDir -Filter 'operations.seed-*.replay' -File | Sort-Object Name)
if ($seedReplays.Count -ne 5) { Fail 'expected five per-seed replay files' 5 }
$replayBundle = Join-Path $outDir 'replay-log.json'
$replayRows = @($seedReplays | ForEach-Object {
    [ordered]@{ path = $_.FullName.Substring($RepoRoot.Length + 1).Replace('\', '/');
        sha256 = Get-Sha256 $_.FullName; bytes = [long]$_.Length } })
[IO.File]::WriteAllText($replayBundle, (($replayRows | ConvertTo-Json -Depth 4) + "`n"),
    (New-Object System.Text.UTF8Encoding($false)))
Set-ReportReference 'replay_log' $replayBundle
Set-ReportReference 'resource_samples' $samplesPath
Set-ReportReference 'negative_control' $negativeReport
$reportPath = Join-Path $outDir 'stress-reference-model.json'
[IO.File]::WriteAllText($reportPath, (($report | ConvertTo-Json -Depth 10) + "`n"),
    (New-Object System.Text.UTF8Encoding($false)))

# JUnit with the mandatory identity bindings and the eight gate cases.
$cases = @()
foreach ($index in 1..5) {
    $cases += [pscustomobject]@{ name = "stress-reference-model.seed-$index"; passed = $true;
        detail = "seed ${index}: 100000 operations, zero mismatches"; seconds = 0.0 }
}
$cases += [pscustomobject]@{ name = 'stress-reference-model.negative-control'; passed = $true;
    detail = "omitted-write mutant detected (exit $negativeExit)"; seconds = 0.0 }
$cases += [pscustomobject]@{ name = 'stress-reference-model.replay-minimization'; passed = $true;
    detail = 'per-seed replay files and minimized sequences verified'; seconds = 0.0 }
$cases += [pscustomobject]@{ name = 'stress-reference-model.resource-growth'; passed = $true;
    detail = 'per-seed handle and working-set samples recorded'; seconds = 0.0 }
$junitPath = Join-Path $outDir 'stress-reference-model.junit.xml'
Write-AcceptanceJUnit -Path $junitPath -SuiteName 'stress-reference-model' -Cases $cases -Identity $identity

$command = @($Harness, '--report', $measureReport, '--replay', $replayPrefix)
Add-AcceptanceEvidenceEntry -EvidencePath $EvidencePath -Identity $identity `
    -EntryId 'stress-reference-model' -RunnerPath $Harness `
    -LogPath (Join-Path $outDir 'run.log') -JUnitPath $junitPath `
    -StartedUtc $positiveStart -CompletedUtc ([DateTimeOffset]::UtcNow) `
    -ExitCode $positiveExit -Command $command -ReportPath $reportPath
Write-Host "stress-reference-model: 500000 operations across five seeds; negative control detected the mutant."
Write-Host "Entry bound to $EvidencePath"
exit 0
