# Execute the candidate qualification evaluator and record hash-bound evidence.
#
# This runner never synthesizes a PASS. It executes
# tools/backend/evaluate_candidates.py in qualification mode with the frozen
# machine policy, captures the real process log and exit code, maps the
# measured report to the five mandatory candidate-quality cases, and appends
# one evidence entry to release-evidence.json. A failed evaluator or any
# unmet condition produces failing JUnit cases and a nonzero exit; the
# release gate then blocks. Candidate surfaces never appear in this log or
# JUnit: only case ids, counts and reason codes.
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$BuildDir = '',
    [string]$PackageManifest = '',
    [string]$CasesDir = '',
    [string]$Manifest = '',
    [string]$Resources = '',
    [string]$Harness = '',
    [string]$RimeDll = '',
    [string]$RimeShared = '',
    [string]$BaselineReport = '',
    [string]$EvidencePath = '',
    [ValidateSet('internal-unsigned', 'public-signed')]
    [string]$AcceptanceProfile = 'internal-unsigned',
    [int]$TimeoutSeconds = 3600
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Fail([string]$Message, [int]$Code = 2) {
    Write-Host "FAILED: $Message" -ForegroundColor Red
    exit $Code
}
function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Assert-Leaf([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { Fail "$Label is missing" 2 }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -and
            ($item.LinkType -or @($item.Target).Count -gt 0)) { Fail "$Label must not be a reparse point" 2 }
}
function Assert-Plain([string]$Path, [string]$Boundary, [string]$Label) {
    $full = [IO.Path]::GetFullPath($Path)
    $base = [IO.Path]::GetFullPath($Boundary).TrimEnd('\', '/')
    if ($full -ne $base -and -not $full.StartsWith($base + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) { Fail "$Label is outside its permitted directory" 2 }
    $cursor = $full
    while ($cursor -and $cursor.Length -ge $base.Length) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -and
                    ($item.LinkType -or @($item.Target).Count -gt 0)) { Fail "$Label must not traverse a reparse point" 2 }
        }
        if ($cursor -eq $base) { break }
        $cursor = Split-Path -Parent $cursor
    }
}
function New-Reference([string]$Path) {
    $file = Get-Item -LiteralPath $Path -Force
    if ($file.PSIsContainer -or $file.Length -eq 0) { Fail "evidence file is empty: $($file.Name)" 2 }
    $relative = $file.FullName.Substring($script:repoRoot.Length + 1).Replace('\', '/')
    return [ordered]@{ path = $relative; sha256 = (Get-Sha256 $file.FullName); bytes = [long]$file.Length }
}
function Get-Prop($Object, [string]$Name, $Default = $null) {
    if ($null -eq $Object) { return $Default }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $Default }
    return $property.Value
}
function Test-ExactProtocol($Report, $Policy) {
    $determinism = Get-Prop $Report 'determinism'
    $qualification = Get-Prop $Policy 'qualification'
    return ((Get-Prop $determinism 'repeats_per_case') -eq (Get-Prop $qualification 'repeat') -and
            (Get-Prop $determinism 'fresh_runs') -eq (Get-Prop $qualification 'runs'))
}
function Test-Rate($Value, [double]$Minimum) {
    return ($null -ne $Value -and [double]$Value -ge $Minimum)
}

if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
$script:repoRoot = (Get-Item -LiteralPath $RepoRoot).FullName
$buildBase = [IO.Path]::GetFullPath((Join-Path $script:repoRoot 'build'))
if (-not $BuildDir) { $BuildDir = Join-Path $buildBase 'windows-product' }
$BuildDir = [IO.Path]::GetFullPath($BuildDir).TrimEnd('\', '/')
if ((Split-Path -Parent $BuildDir) -ne $buildBase -or
        (Split-Path -Leaf $BuildDir) -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$') {
    Fail 'BuildDir must be exactly <repo>/build/<preset>' 2
}
$version = (Get-Content -LiteralPath (Join-Path $script:repoRoot 'platforms\windows\product\VERSION') -Raw -Encoding UTF8).Trim()
if (-not $PackageManifest) { $PackageManifest = Join-Path $BuildDir "stage\EpoCanva-$version\package.manifest" }
if (-not $CasesDir) { $CasesDir = Join-Path $script:repoRoot 'data\eval\candidate-v1\cases' }
if (-not $Manifest) { $Manifest = Join-Path $script:repoRoot 'data\eval\candidate-v1\manifest.json' }
if (-not $EvidencePath) { $EvidencePath = Join-Path $BuildDir 'release-evidence.json' }
$EvidencePath = [IO.Path]::GetFullPath($EvidencePath)
foreach ($pair in @(
        @($PackageManifest, 'package manifest'),
        @((Join-Path $CasesDir 'en.jsonl'), 'English corpus'),
        @($Manifest, 'corpus manifest'),
        @((Join-Path $script:repoRoot 'data\eval\candidate-v1\quality-policy-v2.json'), 'quality policy'),
        @((Join-Path $script:repoRoot 'data\locale\backend-bindings.json'), 'backend bindings'),
        @((Join-Path $script:repoRoot 'tools\backend\evaluate_candidates.py'), 'evaluator'),
        @((Join-Path $script:repoRoot 'tools\backend\candidate_quality_policy.py'), 'evaluator policy module'),
        @((Join-Path $BuildDir 'corresponding-source.zip'), 'corresponding source archive'),
        @((Join-Path $BuildDir 'product-build-receipt.json'), 'product build receipt'))) {
    Assert-Leaf $pair[0] $pair[1]
}
Assert-Plain $BuildDir $buildBase 'BuildDir'
Assert-Plain $EvidencePath $BuildDir 'EvidencePath'
if (-not $Resources) { Fail 'Resources is required' 2 }
if (-not $Harness) { Fail 'Harness is required' 2 }
if (-not $RimeDll) { Fail 'RimeDll is required for zh cases' 2 }
if (-not $RimeShared) { Fail 'RimeShared is required for zh cases' 2 }
Assert-Leaf $Harness 'evaluation harness'
Assert-Leaf $RimeDll 'rime.dll'
Assert-Plain $Harness $buildBase 'Harness'
$Resources = [IO.Path]::GetFullPath($Resources)
$RimeShared = [IO.Path]::GetFullPath($RimeShared)
if (-not (Test-Path -LiteralPath $Resources -PathType Container)) { Fail 'Resources directory is missing' 2 }
if (-not (Test-Path -LiteralPath $RimeShared -PathType Container)) { Fail 'RimeShared directory is missing' 2 }
if ($BaselineReport) { Assert-Leaf $BaselineReport 'baseline report' }

# Resolve exactly one real Python interpreter. Get-Command can return several PATH
# entries (and the WindowsApps app-execution alias); passing that array to the call
# operator runs no evaluator while a stale $LASTEXITCODE can still look like success.
# Prefer the first real file in PATH order and never accept the WindowsApps alias.
$pythonCommands = @(Get-Command python -CommandType Application -All -ErrorAction SilentlyContinue)
$pythonPaths = @($pythonCommands | ForEach-Object { $_.Source } | Where-Object {
        $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) -and
        $_ -notmatch '[\\/]WindowsApps[\\/]'
    } | Select-Object -Unique)
if ($pythonPaths.Count -eq 0) {
    Fail 'Missing tool: a real python.exe on PATH (WindowsApps aliases are not accepted)' 2
}
$pythonExe = [string]$pythonPaths[0]
$pythonVersion = (& $pythonExe -c 'import platform; print(platform.python_version())' 2>$null)
if ($LASTEXITCODE -ne 0 -or -not $pythonVersion) {
    Fail "Python interpreter is not runnable: $pythonExe" 2
}

. (Join-Path $PSScriptRoot 'build_identity.ps1')
$source = Get-ProductSourceIdentity -RepoRoot $script:repoRoot
if ($source.dirty) { Fail 'qualification requires a clean source tree' 1 }
$head = $source.head
$commitTimeText = (& git -C $script:repoRoot show -s --format=%cI $head).Trim()
if ($LASTEXITCODE -ne 0 -or -not $commitTimeText) { Fail 'cannot read candidate commit timestamp' 2 }
$status = @(& git -C $script:repoRoot status --porcelain=v1 -uall)
if ($LASTEXITCODE -ne 0 -or $status.Count -ne 0) { Fail 'qualification requires a clean source tree' 1 }

$qualityPolicyPath = Join-Path $script:repoRoot 'data\eval\candidate-v1\quality-policy-v2.json'
$corpusManifestPath = Join-Path $script:repoRoot 'data\eval\candidate-v1\manifest.json'
$backendManifestPath = Join-Path $script:repoRoot 'data\locale\backend-bindings.json'
$languagePolicyPath = Join-Path $script:repoRoot 'data\manifests\language-readiness.json'
$packagePolicyPath = Join-Path $script:repoRoot 'data\manifests\product-package-files.inc'
$licenseDecisionPath = Join-Path $script:repoRoot 'data\manifests\license-decision.json'
$evaluatorPath = Join-Path $script:repoRoot 'tools\backend\evaluate_candidates.py'
$supportPath = Join-Path $script:repoRoot 'tools\backend\candidate_quality_policy.py'
$runnerPath = (Get-Item -LiteralPath $PSCommandPath -Force).FullName
$policy = Get-Content -LiteralPath $qualityPolicyPath -Raw -Encoding UTF8 | ConvertFrom-Json
$tierBLockPath = Join-Path (Split-Path -Parent $qualityPolicyPath) ([string]$policy.tier_b_lock)
Assert-Leaf $tierBLockPath 'Tier B lock'

$outputDir = Join-Path $BuildDir 'candidate-quality'
if (Test-Path -LiteralPath $outputDir) { Remove-Item -LiteralPath $outputDir -Recurse -Force }
$null = New-Item -ItemType Directory -Path $outputDir -Force
$reportPath = Join-Path $outputDir 'report.json'
$junitPath = Join-Path $outputDir 'candidate-quality.junit.xml'
$logPath = Join-Path $outputDir 'process.log'
$stdoutPath = Join-Path $outputDir 'evaluator.stdout.log'
$stderrPath = Join-Path $outputDir 'evaluator.stderr.log'

$runs = [int]$policy.qualification.runs
$repeat = [int]$policy.qualification.repeat
$warmup = [int]$policy.qualification.warmup_queries
$seed = [int]$policy.qualification.seed
$bootstraps = [int]$policy.qualification.bootstrap_replicates
$evaluatorArguments = @(
    '--cases', $CasesDir,
    '--manifest', $Manifest,
    '--bindings', $backendManifestPath,
    '--resources', $Resources,
    '--harness', ([IO.Path]::GetFullPath($Harness)),
    '--output', $reportPath,
    '--mode', 'qualification',
    '--expected-repo-commit', $head,
    '--runs', [string]$runs,
    '--repeat', [string]$repeat,
    '--warmup-queries', [string]$warmup,
    '--seed', [string]$seed,
    '--bootstrap-replicates', [string]$bootstraps,
    '--rime-dll', ([IO.Path]::GetFullPath($RimeDll)),
    '--rime-dll-sha256', (Get-Sha256 $RimeDll),
    '--rime-shared', $RimeShared,
    '--package-manifest', ([IO.Path]::GetFullPath($PackageManifest)),
    '--source-archive', ([IO.Path]::GetFullPath((Join-Path $BuildDir 'corresponding-source.zip'))),
    '--language-policy', $languagePolicyPath,
    '--package-policy', $packagePolicyPath,
    '--acceptance-profile', $AcceptanceProfile,
    '--timeout', [string]$TimeoutSeconds
)
if ($BaselineReport) { $evaluatorArguments += @('--baseline-report', ([IO.Path]::GetFullPath($BaselineReport))) }

$started = (Get-Date).ToUniversalTime().ToString('o')
# Clear the previous native exit code first: if the invocation itself fails before
# starting Python, $LASTEXITCODE must stay $null so the evidence cannot record 0.
$global:LASTEXITCODE = $null
$ErrorActionPreference = 'Continue'
try {
    & $pythonExe -B $evaluatorPath @evaluatorArguments 1> $stdoutPath 2> $stderrPath
} catch {
    $global:LASTEXITCODE = -1
}
$evaluatorExit = if ($null -eq $LASTEXITCODE) { -1 } else { [int]$LASTEXITCODE }
$ErrorActionPreference = 'Stop'
$completed = (Get-Date).ToUniversalTime().ToString('o')
$commandLine = @($pythonExe, '-B', $evaluatorPath) + $evaluatorArguments
$logText = (@('evaluator_command=' + ($commandLine -join ' ')) +
    @('python_exe=' + $pythonExe) + @('python_version=' + [string]$pythonVersion) +
    @('evaluator_exit=' + [string]$evaluatorExit) +
    @('started_utc=' + $started) + @('completed_utc=' + $completed) +
    @('--- stdout ---') + @(Get-Content -LiteralPath $stdoutPath -Encoding UTF8 -ErrorAction SilentlyContinue) +
    @('--- stderr ---') + @(Get-Content -LiteralPath $stderrPath -Encoding UTF8 -ErrorAction SilentlyContinue)) -join "`n"
[IO.File]::WriteAllText($logPath, $logText + "`n", (New-Object Text.UTF8Encoding($false)))

$report = $null
if (Test-Path -LiteralPath $reportPath -PathType Leaf) {
    try { $report = Get-Content -LiteralPath $reportPath -Raw -Encoding UTF8 | ConvertFrom-Json }
    catch { $report = $null }
}
$bindings = [ordered]@{
    version = $version
    commit = $source.commit
    source_sha256 = $source.source_sha256
    package_manifest_sha256 = (Get-Sha256 $PackageManifest)
    package_policy_sha256 = (Get-Sha256 $packagePolicyPath)
    language_policy_sha256 = (Get-Sha256 $languagePolicyPath)
    acceptance_profile = $AcceptanceProfile
    quality_policy_sha256 = (Get-Sha256 $qualityPolicyPath)
    corpus_manifest_sha256 = (Get-Sha256 $corpusManifestPath)
    source_archive_sha256 = (Get-Sha256 (Join-Path $BuildDir 'corresponding-source.zip'))
    evaluator_sha256 = (Get-Sha256 $evaluatorPath)
    evaluator_support_sha256 = (Get-Sha256 $supportPath)
    runner_sha256 = (Get-Sha256 $runnerPath)
    harness_sha256 = (Get-Sha256 $Harness)
    backend_manifest_sha256 = (Get-Sha256 $backendManifestPath)
    tier_b_lock_sha256 = (Get-Sha256 $tierBLockPath)
}

$identity = Get-Prop $report 'identity'
$gate = Get-Prop $report 'quality_gate'
$perLanguage = Get-Prop $report 'per_language'
$failures = @(Get-Prop $report 'failures' @())
$unverified = Get-Prop $report 'unverified'
$predicates = Get-Prop $gate 'predicates'
$minimums = Get-Prop $policy 'minimum_cases'
$thresholds = Get-Prop $policy 'thresholds'
$corpusEvidence = Get-Prop (Get-Prop $report 'corpus') 'evidence'
$languages = @('en', 'zh', 'fr', 'ja', 'ko')

$identityOk = ($null -ne $report) -and
    ((Get-Prop $report 'format') -eq 'epocanva-candidate-eval-v3') -and
    ((Get-Prop $identity 'mode') -eq 'qualification') -and
    ((Get-Prop $identity 'repo_commit') -eq $bindings['commit']) -and
    ((Get-Prop $identity 'source_tree_clean') -eq $true) -and
    ((Get-Prop $identity 'quality_policy_sha256') -eq $bindings['quality_policy_sha256']) -and
    ((Get-Prop $identity 'manifest_sha256') -eq $bindings['corpus_manifest_sha256']) -and
    ((Get-Prop $identity 'backend_manifest_sha256') -eq $bindings['backend_manifest_sha256']) -and
    ((Get-Prop $identity 'harness_sha256') -eq $bindings['harness_sha256']) -and
    ((Get-Prop $corpusEvidence 'status') -eq 'passed') -and
    ((Get-Prop $corpusEvidence 'tier') -eq 'B') -and
    ((Get-Prop $corpusEvidence 'lock_sha256') -eq $bindings['tier_b_lock_sha256'])

$coverageOk = $identityOk
if ($coverageOk) {
    foreach ($language in $languages) {
        $values = Get-Prop $perLanguage $language
        if ($null -eq $values -or
                ([int](Get-Prop $values 'cases' -1)) -lt [int]$minimums.$language -or
                (Get-Prop $values 'minimum_cases_met') -ne $true) { $coverageOk = $false; break }
    }
}
$determinismOk = $identityOk -and (Test-ExactProtocol $report $policy)
if ($determinismOk) {
    foreach ($language in $languages) {
        $values = Get-Prop $perLanguage $language
        if ($null -eq $values) { $determinismOk = $false; break }
        foreach ($field in @('deterministic_case_rate', 'in_process_repeat_deterministic_rate',
                'cross_process_deterministic_rate')) {
            if (-not (Test-Rate (Get-Prop $values $field) ([double]$thresholds.deterministic_case_rate_min))) {
                $determinismOk = $false; break
            }
        }
        if (-not $determinismOk) { break }
    }
}
$contractsOk = $identityOk -and (@($failures).Count -eq 0) -and
    ($null -ne $unverified) -and (@($unverified.PSObject.Properties).Count -eq 0) -and
    ((Get-Prop $gate 'status') -eq 'passed') -and ((Get-Prop $gate 'met') -eq $true) -and
    (@(Get-Prop $gate 'unmet_reasons' @()).Count -eq 0) -and
    (@(Get-Prop $gate 'not_evaluated_predicates' @()).Count -eq 0)
if ($contractsOk) {
    foreach ($language in $languages) {
        $values = Get-Prop $perLanguage $language
        if ($null -eq $values -or
                -not (Test-Rate (Get-Prop $values 'raw_present_rate') ([double]$thresholds.raw_present_rate_min)) -or
                -not (Test-Rate (Get-Prop $values 'raw_first_rate') ([double]$thresholds.raw_first_rate_min)) -or
                ([double](Get-Prop $values 'empty_menu_rate' 1)) -gt [double]$thresholds.empty_menu_rate_max -or
                ([double](Get-Prop $values 'wrong_language_rate' 1)) -gt [double]$thresholds.wrong_language_rate_max -or
                ((Get-Prop (Get-Prop $values 'wrong_language_observability') 'status') -ne 'observable')) {
            $contractsOk = $false; break
        }
    }
}
$qualificationOk = $identityOk -and ($evaluatorExit -eq 0) -and
    ((Get-Prop $gate 'status') -eq 'passed') -and ((Get-Prop $gate 'met') -eq $true)

$caseResults = [ordered]@{
    'candidate-quality.qualification' = @($qualificationOk, 'evaluator-not-passing')
    'candidate-quality.coverage' = @($coverageOk, 'sample-or-coverage-failure')
    'candidate-quality.determinism' = @($determinismOk, 'determinism-failure')
    'candidate-quality.contracts' = @($contractsOk, 'contract-or-predicate-failure')
    'candidate-quality.identity-binding' = @($identityOk, 'identity-or-input-binding-failure')
}

$settings = New-Object System.Xml.XmlWriterSettings
$settings.Indent = $true
$settings.Encoding = New-Object System.Text.UTF8Encoding($false)
$settings.OmitXmlDeclaration = $false
$writer = [System.Xml.XmlWriter]::Create($junitPath, $settings)
try {
    $writer.WriteStartDocument()
    $writer.WriteStartElement('testsuite')
    $writer.WriteAttributeString('name', 'candidate-quality')
    $writer.WriteAttributeString('tests', '5')
    $writer.WriteAttributeString('failures', [string](@($caseResults.Values | Where-Object { -not $_[0] }).Count))
    $writer.WriteAttributeString('errors', '0')
    $writer.WriteAttributeString('skipped', '0')
    $writer.WriteAttributeString('disabled', '0')
    $writer.WriteStartElement('properties')
    foreach ($key in $bindings.Keys) {
        $writer.WriteStartElement('property')
        $writer.WriteAttributeString('name', 'epocanva.' + $key)
        $writer.WriteAttributeString('value', [string]$bindings[$key])
        $writer.WriteEndElement()
    }
    $writer.WriteEndElement()
    foreach ($name in $caseResults.Keys) {
        $writer.WriteStartElement('testcase')
        $writer.WriteAttributeString('name', $name)
        if (-not $caseResults[$name][0]) {
            $writer.WriteStartElement('failure')
            $writer.WriteAttributeString('message', $caseResults[$name][1])
            $writer.WriteString($caseResults[$name][1])
            $writer.WriteEndElement()
        }
        $writer.WriteEndElement()
    }
    $writer.WriteEndElement()
    $writer.WriteEndDocument()
} finally { $writer.Dispose() }

$allPassed = (@($caseResults.Values | Where-Object { -not $_[0] }).Count -eq 0)
$entryExit = if ($allPassed -and $evaluatorExit -eq 0) { 0 } else { 1 }
$runnerCommand = @($runnerPath, '-RepoRoot', $script:repoRoot, '-BuildDir', $BuildDir,
    '-PackageManifest', $PackageManifest, '-Resources', $Resources, '-Harness', ([IO.Path]::GetFullPath($Harness)),
    '-RimeDll', ([IO.Path]::GetFullPath($RimeDll)), '-RimeShared', $RimeShared,
    '-EvidencePath', $EvidencePath, '-AcceptanceProfile', $AcceptanceProfile)
$entry = [ordered]@{
    id = 'candidate-quality'
    kind = 'test-run'
    exit_code = [int]$entryExit
    started_utc = $started
    completed_utc = $completed
    command = $runnerCommand
    runner = New-Reference $runnerPath
    evaluator = New-Reference $evaluatorPath
    harness = New-Reference ([IO.Path]::GetFullPath($Harness))
    log = New-Reference $logPath
    junit = New-Reference $junitPath
}
if ($null -ne $report) { $entry['report'] = New-Reference $reportPath }

$document = $null
if (Test-Path -LiteralPath $EvidencePath -PathType Leaf) {
    $document = Get-Content -LiteralPath $EvidencePath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ((Get-Prop $document 'format') -ne 2 -or
            (Get-Prop $document 'commit') -ne $source.commit -or
            (Get-Prop $document 'source_sha256') -ne $source.source_sha256 -or
            (Get-Prop $document 'package_manifest_sha256') -ne $bindings['package_manifest_sha256']) {
        Fail 'existing release-evidence.json belongs to a different candidate' 2
    }
}
$existingEntries = @()
if ($null -ne $document) {
    $existingEntries = @(Get-Prop $document 'entries' @() | Where-Object { $_.id -ne 'candidate-quality' })
}
$newDocument = [ordered]@{
    format = 2
    version = $version
    commit = $source.commit
    source_sha256 = $source.source_sha256
    package_manifest_sha256 = $bindings['package_manifest_sha256']
    build_receipt_sha256 = (Get-Sha256 (Join-Path $BuildDir 'product-build-receipt.json'))
    package_policy_sha256 = $bindings['package_policy_sha256']
    acceptance_profile = $AcceptanceProfile
    distribution_policy = $(if ($null -ne $document) { [string](Get-Prop $document 'distribution_policy' 'internal-only') } else { 'internal-only' })
    license_decision_sha256 = (Get-Sha256 $licenseDecisionPath)
    language_policy_sha256 = $bindings['language_policy_sha256']
    entries = @($existingEntries) + @($entry)
}
if ($null -ne $document -and $null -ne (Get-Prop $document 'source_manifest')) {
    $newDocument['source_manifest'] = Get-Prop $document 'source_manifest'
}
[IO.File]::WriteAllText($EvidencePath, ($newDocument | ConvertTo-Json -Depth 10) + "`n",
    (New-Object Text.UTF8Encoding($false)))
Write-Host "candidate-quality evidence written to $EvidencePath"
Write-Host "evaluator exit=$evaluatorExit; cases passed=$(@($caseResults.Values | Where-Object { $_[0] }).Count)/5"
exit $entryExit
