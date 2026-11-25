[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$BuildDir = '',
    [string]$StageRoot = '',
    [string]$EvidencePath = '',
    [string]$Harness = '',
    [switch]$Development
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# Native nonzero results are checked explicitly, including expected negative
# controls; do not inherit pwsh 7's preference from an invoking session.
$PSNativeCommandUseErrorActionPreference = $false

function Write-StressJson([string]$Path, $Document) {
    [IO.File]::WriteAllText($Path, (($Document | ConvertTo-Json -Depth 14) + "`n"), $utf8)
}

function Write-StressNativeJUnit {
    $settings = [Xml.XmlWriterSettings]::new()
    $settings.Indent = $true
    $settings.Encoding = $utf8
    $writer = [Xml.XmlWriter]::Create($nativeJUnit, $settings)
    try {
        $writer.WriteStartDocument()
        $writer.WriteStartElement('testsuite')
        $writer.WriteAttributeString('name', 'editor-native-execution')
        $writer.WriteAttributeString('tests', [string]$executions.Count)
        $writer.WriteAttributeString('failures', [string]@($executions | Where-Object {
            $_.timed_out -or $_.exit_code -ne $_.expected_exit_code
        }).Count)
        $writer.WriteAttributeString('errors', '0')
        $writer.WriteAttributeString('skipped', '0')
        foreach ($item in $executions) {
            $writer.WriteStartElement('testcase')
            $writer.WriteAttributeString('name', $item.mode)
            $writer.WriteAttributeString('status', 'run')
            $seconds = ([DateTimeOffset]::Parse($item.completed_utc) - [DateTimeOffset]::Parse($item.started_utc)).TotalSeconds
            $writer.WriteAttributeString('time', $seconds.ToString('F7', [Globalization.CultureInfo]::InvariantCulture))
            $writer.WriteStartElement('properties')
            $properties = [ordered]@{
                exit_code = [string]$item.exit_code; started_utc = $item.started_utc
                completed_utc = $item.completed_utc; executable_sha256 = $harnessReference.sha256
            }
            foreach ($key in $properties.Keys) {
                $writer.WriteStartElement('property')
                $writer.WriteAttributeString('name', $key)
                $writer.WriteAttributeString('value', $properties[$key])
                $writer.WriteEndElement()
            }
            $writer.WriteEndElement()
            if ($item.timed_out -or $item.exit_code -ne $item.expected_exit_code) {
                $writer.WriteStartElement('failure')
                $writer.WriteAttributeString('message', 'Native exit or timeout differs from protocol')
                $writer.WriteEndElement()
            }
            $writer.WriteEndElement()
        }
        $writer.WriteEndElement()
        $writer.WriteEndDocument()
    } finally { $writer.Dispose() }
}

function Invoke-StressProcess([string]$Mode, [int]$ExpectedExit, [string[]]$Arguments) {
    $process = [Diagnostics.Process]::new()
    $process.StartInfo.FileName = $Harness
    $process.StartInfo.WorkingDirectory = $outDir
    $process.StartInfo.Arguments = $Arguments -join ' '
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    $started = [DateTimeOffset]::UtcNow.ToString('o')
    try {
        if (-not $process.Start()) { throw "Could not launch editor protocol: $Mode" }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $timedOut = -not $process.WaitForExit(1800000)
        if ($timedOut) {
            $process.Kill()
            $process.WaitForExit()
        }
        $code = $process.ExitCode
        $completed = [DateTimeOffset]::UtcNow.ToString('o')
        $logPath = Join-Path $outDir ($Mode + '.log')
        $nativeOutput = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
        [IO.File]::WriteAllText($logPath, $nativeOutput, $utf8)
    } finally {
        $process.Dispose()
    }
    $executions.Add([ordered]@{
        mode = $Mode; command = @($Harness) + $Arguments; exit_code = $code; timed_out = $timedOut
        expected_exit_code = $ExpectedExit
        started_utc = $started; completed_utc = $completed
        log = if ($nativeOutput.Length -gt 0) { New-AcceptanceReference $logPath $RepoRoot }
    })
    $execution.executions = $executions.ToArray()
    $execution.started_utc = $executions[0].started_utc
    $execution.completed_utc = $completed
    Write-StressNativeJUnit
    $execution.native_junit = New-AcceptanceReference $nativeJUnit $RepoRoot
    Write-StressJson $log $execution
    if ($timedOut -or $code -ne $ExpectedExit) {
        throw "Editor protocol $Mode failed: exit $code, timeout=$timedOut (expected exit $ExpectedExit). See $log"
    }
}

if (-not $RepoRoot) { $RepoRoot = Join-Path $PSScriptRoot '..\..' }
$RepoRoot = (Get-Item -LiteralPath $RepoRoot).FullName
. (Join-Path $PSScriptRoot 'acceptance_evidence.ps1')
$utf8 = [Text.UTF8Encoding]::new($false)
$python = (Get-Command python -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$verifier = Join-Path $PSScriptRoot 'verify_stress_reference_model.py'
$buildBase = Join-Path $RepoRoot 'build'
if (-not $BuildDir) { $BuildDir = Join-Path $buildBase 'windows-product' }
$BuildDir = Assert-ProductPlainPath $BuildDir $RepoRoot
if ((Split-Path -Parent $BuildDir) -ine $buildBase) { throw 'BuildDir must be <repo>/build/<preset>.' }

if (-not $Development) {
    $version = (Get-Content -LiteralPath (Join-Path $RepoRoot 'platforms/windows/product/VERSION') -Raw -Encoding UTF8).Trim()
    if (-not $StageRoot) { $StageRoot = Join-Path $BuildDir "stage/EpoCanva-$version" }
    if (-not $EvidencePath) { $EvidencePath = Join-Path $BuildDir 'release-evidence.json' }
    $StageRoot = Assert-ProductPlainPath $StageRoot $BuildDir
    $EvidencePath = Assert-ProductPlainPath $EvidencePath $BuildDir
    $identity = Get-AcceptanceIdentity -RepoRoot $RepoRoot -BuildDir $BuildDir -StageRoot $StageRoot
    if ($identity.commit -cnotmatch '^[0-9a-f]{40}$' -or $identity.receipt.source.dirty -ne $false) {
        throw 'Formal stress requires a clean candidate SHA. Use -Development for local measurements.'
    }
}

if (-not $Harness) {
    $candidates = @('editor_reference_model_tests/Release/epocanva_editor_reference_tests.exe',
        'product/Release/epocanva_editor_reference_tests.exe')
    if ($Development) { $candidates += 'tests/editor_reference_model/Release/epocanva_editor_reference_tests.exe' }
    foreach ($candidate in $candidates) {
        $probe = Join-Path $BuildDir $candidate
        if (Test-Path -LiteralPath $probe -PathType Leaf) { $Harness = $probe; break }
    }
}
if (-not $Harness) { throw 'Editor reference harness missing. Build the Release target first.' }
$Harness = (Get-Item -LiteralPath (Assert-ProductPlainPath $Harness $BuildDir)).FullName
$harnessReference = New-AcceptanceReference $Harness $RepoRoot
$source = Get-ProductSourceIdentity -RepoRoot $RepoRoot
if (-not $Development) {
    $tested = @($identity.receipt.test_binaries | Where-Object { $_.path -ieq $Harness })
    if ($tested.Count -ne 1 -or $tested[0].sha256 -cne $harnessReference.sha256 -or
            $tested[0].bytes -ne $harnessReference.bytes) { throw 'Editor harness differs from the tested binary.' }
}

$runName = 'run-' + [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' + [guid]::NewGuid().ToString('N')
$outDir = Assert-ProductPlainPath (Join-Path $BuildDir "stress-reference-model/$runName") $RepoRoot
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
Write-Host "Editor stress evidence: $outDir"
$hasher = [Security.Cryptography.SHA256]::Create()
try {
    $machineHash = [BitConverter]::ToString($hasher.ComputeHash($utf8.GetBytes([Environment]::MachineName))).Replace('-', '').ToLowerInvariant()
} finally { $hasher.Dispose() }
$scope = 'controlled-real-tsf-edit-layer'
$runner = [ordered]@{
    machine_sha256 = $machineHash; os_version = [Environment]::OSVersion.Version.ToString()
    kind = 'local'
}
if ($env:GITHUB_ACTIONS -ceq 'true') {
    $runner.kind = 'github-actions'
    $runner.run_id = $env:GITHUB_RUN_ID
    $runner.run_attempt = $env:GITHUB_RUN_ATTEMPT
    $runner.job = $env:GITHUB_JOB
}
$execution = [ordered]@{
    format = 'epocanva-editor-stress-execution-v2'; scope = $scope; installed_host_acceptance = $false
    runner_identity = $runner; harness = $harnessReference; working_directory = $outDir
    source = $source; native_junit = $null
    started_utc = $null; completed_utc = $null; executions = @()
}
$executions = [Collections.Generic.List[object]]::new()
$log = Join-Path $outDir 'commands.json'
$nativeJUnit = Join-Path $outDir 'native.junit.xml'
Invoke-StressProcess 'stress' 0 @('--report', 'measurements.json')
Invoke-StressProcess 'self-test' 0 @('--self-test', '--report', 'negative-control.json')
Invoke-StressProcess 'replay-mutant' 1 @('--replay', 'negative-control.json.replay', '--report', 'mutant-replay.json')
Invoke-StressProcess 'force-fail' 1 @('--force-fail')

$measureReport = Join-Path $outDir 'measurements.json'
$measurement = Read-ProductJson $measureReport
$replayBundle = Join-Path $outDir 'replay-log.json'
Write-StressJson $replayBundle ([ordered]@{
    format = 'epocanva-editor-stress-replay-v1'
    seeds = @(1..5 | ForEach-Object { New-AcceptanceReference "$measureReport.seed-$_.replay" $RepoRoot })
    original = New-AcceptanceReference (Join-Path $outDir 'negative-control.json.original.replay') $RepoRoot
    minimal = New-AcceptanceReference (Join-Path $outDir 'negative-control.json.replay') $RepoRoot
})
$samplesPath = Join-Path $outDir 'resource-samples.json'
Write-StressJson $samplesPath ([ordered]@{
    format = 'epocanva-editor-stress-resources-v1'
    seeds = @($measurement.seeds | ForEach-Object {
        [ordered]@{ seed = $_.seed; samples = $_.resource_samples }
    })
})
foreach ($file in @(@('measurements', 'measurements.json'), @('replay_log', 'replay-log.json'),
        @('resource_samples', 'resource-samples.json'), @('negative_control', 'negative-control.json'),
        @('mutant_replay_report', 'mutant-replay.json'))) {
    $execution[$file[0]] = New-AcceptanceReference (Join-Path $outDir $file[1]) $RepoRoot
}
Write-StressJson $log $execution

$sourceAfter = Get-ProductSourceIdentity -RepoRoot $RepoRoot
foreach ($key in @('head', 'commit', 'source_sha256', 'repository_sha256', 'dirty', 'source_files')) {
    if (-not [object]::Equals($source.$key, $sourceAfter.$key)) { throw "Source changed during editor stress: $key" }
}

$verifyArgs = @('-B', $verifier, '--repo', $RepoRoot, '--build-dir', $BuildDir, '--runner', $Harness, '--log', $log)
$resourcePath = Join-Path $outDir 'resource-assessment.json'
$global:LASTEXITCODE = $null
& $python @verifyArgs --development --resource-assessment $resourcePath
$rawExit = $LASTEXITCODE
if ($rawExit -isnot [int]) { throw 'Editor stress verifier did not return an exit code.' }
if ($rawExit -ne 0) { exit $rawExit }
$resourceAssessment = Read-ProductJson $resourcePath
if ($Development) {
    Write-Host "Controlled editor stress verified in development mode. Formal resources: $($resourceAssessment.status)."
    exit 0
}

$junitPath = Join-Path $outDir 'stress-reference-model.junit.xml'
$cases = @(for ($ordinal = 0; $ordinal -lt 5; ++$ordinal) {
    [pscustomobject]@{ name = "stress-reference-model.seed-$($ordinal + 1)"; passed = $true
        detail = 'Independent editor oracle'; seconds = $measurement.seeds[$ordinal].duration_ms / 1000 }
})
foreach ($name in @('negative-control', 'replay-minimization', 'resource-growth')) {
    $passed = $name -cne 'resource-growth' -or $resourceAssessment.status -ceq 'passed'
    $cases += [pscustomobject]@{ name = "stress-reference-model.$name"; passed = $passed
        detail = if ($passed) { '' } else { $resourceAssessment.blockers -join ',' }; seconds = 0.0 }
}
Write-AcceptanceJUnit -Path $junitPath -SuiteName 'stress-reference-model' -Cases $cases -Identity $identity
$report = [ordered]@{
    format = 'epocanva-stress-reference-model-v1'; result = $resourceAssessment.status
    oracle = $measurement.oracle; failure_reduction = $measurement.failure_reduction
    installed_host_acceptance = $false; total_ops = $measurement.total_ops; seeds = $measurement.seeds
    started_utc = $execution.started_utc; completed_utc = $execution.completed_utc; runner_identity = $runner
    executable = $harnessReference; execution_log = New-AcceptanceReference $log $RepoRoot
    junit = New-AcceptanceReference $junitPath $RepoRoot
    build_receipt = New-AcceptanceReference (Join-Path $BuildDir 'product-build-receipt.json') $RepoRoot
    package_manifest = New-AcceptanceReference (Join-Path $StageRoot 'package.manifest') $RepoRoot
    native_junit = $execution.native_junit; source = $execution.source
    resource_assessment = New-AcceptanceReference $resourcePath $RepoRoot
}
foreach ($key in @('version', 'commit', 'source_sha256', 'build_receipt_sha256', 'package_manifest_sha256',
        'language_policy_sha256', 'acceptance_profile')) { $report[$key] = $identity.$key }
foreach ($key in @('measurements', 'replay_log', 'resource_samples', 'negative_control', 'mutant_replay_report')) {
    $report[$key] = $execution[$key]
}
$reportPath = Join-Path $outDir 'stress-reference-model.json'
Write-StressJson $reportPath $report
if ($resourceAssessment.status -cne 'passed') {
    Write-Host "EDITOR_STRESS_BLOCKED $($resourceAssessment.blockers -join ',')"
    exit 1
}
$global:LASTEXITCODE = $null
& $python @verifyArgs --stage-root $StageRoot --report $reportPath --junit $junitPath
$reportExit = $LASTEXITCODE
if ($null -eq $reportExit) { throw 'Editor stress verifier did not return an exit code.' }
if ($reportExit -ne 0) { exit $reportExit }

$current = Get-AcceptanceIdentity -RepoRoot $RepoRoot -BuildDir $BuildDir -StageRoot $StageRoot
foreach ($key in @('commit', 'source_sha256', 'build_receipt_sha256', 'package_manifest_sha256', 'language_policy_sha256')) {
    if ($identity.$key -cne $current.$key) { throw "Candidate changed during stress: $key" }
}
Add-AcceptanceEvidenceEntry -EvidencePath $EvidencePath -Identity $identity -EntryId 'stress-reference-model' `
    -RunnerPath $Harness -LogPath $log -JUnitPath $junitPath -ReportPath $reportPath `
    -StartedUtc ([DateTimeOffset]::Parse($execution.started_utc)) -CompletedUtc ([DateTimeOffset]::Parse($execution.completed_utc)) `
    -ExitCode $executions[0].exit_code -Command @($Harness, '--report', 'measurements.json')
Write-Host "Bound controlled editor stress to $EvidencePath"
exit 0
