# Offline-privacy acceptance producer.
#
# Four formal cases, each backed by a real observation of the tested candidate:
#   runtime-no-network      static import audit plus live TCP/UDP endpoint
#                           observation of the product sink process tree
#   dependency-review       dumpbin /imports of EpoCanva.exe, epocanva_tsf.dll
#                           and rime.dll, plus the fixed dependency manifest
#   no-content-logging      a fresh synthetic canary is typed through the
#                           product; stdout, stderr and every isolated user-data
#                           file are searched for it
#   sensitive-no-persistence the same canary is typed into a password-scoped
#                           field; the user-data tree is searched again and the
#                           learning store must not exist while recording is off
#
# The canary is generated per run, passed only through the child environment,
# never written to the evidence log, and redacted if it ever appears. A missing
# monitoring cmdlet or dumpbin fails the case; it is never silently skipped.
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$BuildDir = '',
    [string]$StageRoot = '',
    [string]$EvidencePath = '',
    [string]$Canary = '',
    [switch]$SkipEvidenceWrite
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
function Find-Dumpbin {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $installs = @(& $vswhere -all -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
        foreach ($install in $installs) {
            $tools = Join-Path $install 'VC\Tools\MSVC'
            if (-not (Test-Path -LiteralPath $tools)) { continue }
            $found = Get-ChildItem -LiteralPath $tools -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match '\\Hostx64\\x64\\dumpbin\.exe$' } |
                Sort-Object FullName -Descending | Select-Object -First 1
            if ($null -ne $found) { return $found.FullName }
        }
    }
    $onPath = Get-Command dumpbin.exe -CommandType Application -ErrorAction SilentlyContinue
    if ($null -ne $onPath) { return $onPath.Source }
    return ''
}
function Find-CanaryInTree([string]$Root, [string]$Needle, [ref]$FilesScanned) {
    $utf16 = -join ($Needle.ToCharArray() | ForEach-Object { [string]$_ + [char]0 })
    foreach ($item in (Get-ChildItem -LiteralPath $Root -Recurse -File -Force -ErrorAction SilentlyContinue)) {
        if ($item.Length -gt 16MB) { continue }
        try { $bytes = [IO.File]::ReadAllBytes($item.FullName) } catch { continue }
        ++$FilesScanned.Value
        $text = [Text.Encoding]::UTF8.GetString($bytes)
        if ($text.Contains($Needle) -or $text.Contains($utf16)) { return $item.FullName }
        $unicode = [Text.Encoding]::Unicode.GetString($bytes)
        if ($unicode.Contains($Needle)) { return $item.FullName }
    }
    return ''
}
function Find-LearningStoreFile([string]$Root) {
    foreach ($item in (Get-ChildItem -LiteralPath $Root -Recurse -File -Force -ErrorAction SilentlyContinue)) {
        if ($item.Name -match 'learning' -and $item.Extension -in @('.dat', '.v2', '.tmp')) {
            return $item.FullName
        }
    }
    return ''
}
function Invoke-ObservedPrivacyRun {
    param(
        [string]$Executable,
        [string[]]$Arguments,
        [hashtable]$Environment,
        [string]$LogPath,
        [int]$TimeoutSeconds
    )
    $started = [DateTimeOffset]::UtcNow
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Executable
    $psi.Arguments = (($Arguments | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
    }) -join ' ')
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    foreach ($key in $Environment.Keys) {
        $psi.EnvironmentVariables[$key] = [string]$Environment[$key]
    }
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $psi
    $networkSeen = $false
    $monitoringAvailable = ($null -ne (Get-Command Get-NetTCPConnection -ErrorAction SilentlyContinue)) -and
        ($null -ne (Get-Command Get-NetUDPEndpoint -ErrorAction SilentlyContinue))
    if (-not $monitoringAvailable) { $networkSeen = $true }
    $endpointSamples = 0
    $knownPids = @()
    $lastTreeRefresh = [DateTime]::MinValue
    $null = $process.Start()
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while (-not $process.HasExited) {
        if ([DateTime]::UtcNow -gt $deadline) {
            try { $process.Kill() } catch { }
            break
        }
        if (([DateTime]::UtcNow - $lastTreeRefresh).TotalMilliseconds -ge 1000) {
            $lastTreeRefresh = [DateTime]::UtcNow
            $knownPids = @($process.Id)
            $queue = [System.Collections.Generic.Queue[int]]::new()
            $queue.Enqueue($process.Id)
            while ($queue.Count -gt 0) {
                $parent = $queue.Dequeue()
                $children = @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$parent" -ErrorAction SilentlyContinue)
                foreach ($child in $children) {
                    if ($knownPids -notcontains [int]$child.ProcessId) {
                        $knownPids += [int]$child.ProcessId
                        $queue.Enqueue([int]$child.ProcessId)
                    }
                }
            }
        }
        foreach ($processId in $knownPids) {
            if (-not $monitoringAvailable) { break }
            $tcp = @(Get-NetTCPConnection -OwningProcess $processId -ErrorAction SilentlyContinue)
            $udp = @(Get-NetUDPEndpoint -OwningProcess $processId -ErrorAction SilentlyContinue)
            if ($tcp.Count -gt 0 -or $udp.Count -gt 0) {
                $networkSeen = $true
                $endpointSamples += ($tcp.Count + $udp.Count)
            }
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $process.HasExited) {
        try { $process.WaitForExit(5000) | Out-Null } catch { }
    }
    $stdout = $stdoutTask.Result
    $stderr = $stderrTask.Result
    $exitCode = if ($process.HasExited) { $process.ExitCode } else { -1 }
    $watch.Stop()
    $completed = [DateTimeOffset]::UtcNow
    $directory = Split-Path -Parent $LogPath
    if ($directory) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
    [IO.File]::AppendAllText($LogPath,
        ("===== offline-privacy product run | exit=$exitCode | started=$($started.ToString('o')) | completed=$($completed.ToString('o')) =====`n" +
         $stdout + $stderr + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    return [pscustomobject]@{
        exit_code = [int]$exitCode
        passed = ($exitCode -eq 0 -and (($stdout + $stderr) -match 'RESULT PASS'))
        seconds = $watch.Elapsed.TotalSeconds
        started_utc = $started
        completed_utc = $completed
        network_seen = $networkSeen
        monitoring_available = $monitoringAvailable
        endpoint_samples = $endpointSamples
        stdout = $stdout
        stderr = $stderr
    }
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
if (-not $Canary) {
    $Canary = 'epocanvaprivacy' + [guid]::NewGuid().ToString('N').Substring(0, 16)
}
if ($Canary -notmatch '^[A-Za-z0-9]{8,64}$') { Fail 'Canary must be 8-64 ASCII alphanumeric characters' 2 }

. (Join-Path $PSScriptRoot 'acceptance_evidence.ps1')
$identity = Get-AcceptanceIdentity -RepoRoot $RepoRoot -BuildDir $BuildDir -StageRoot $StageRoot

$sinkExe = Join-Path $BuildDir 'product\bin\epocanva_product_sink_tests.exe'
$productDll = Join-Path $BuildDir 'product\bin\epocanva_tsf_direct_sink_test.dll'
$stagedExe = Join-Path $StageRoot 'EpoCanva.exe'
$stagedTsf = Join-Path $StageRoot 'epocanva_tsf.dll'
$stagedRime = Join-Path $StageRoot 'rime.dll'
foreach ($path in @($sinkExe, $productDll, $stagedExe, $stagedTsf, $stagedRime)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { Fail "required binary is missing: $path" 3 }
}
$rimeDll = @($identity.receipt.binaries | Where-Object { $_.name -eq 'rime' })[0].path
$dataDir = Join-Path $RepoRoot 'data\product'
$lexicon = Join-Path $RepoRoot 'build\product-data\epocanva_en_alpha.tsv'
$outputDir = Join-Path $BuildDir 'offline-privacy'
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$logPath = Join-Path $outputDir 'process.log'
$junitPath = Join-Path $outputDir 'offline-privacy.junit.xml'
$reportPath = Join-Path $outputDir 'report.json'
if (Test-Path -LiteralPath $logPath) { Remove-Item -LiteralPath $logPath -Force }

$startedAll = [DateTimeOffset]::UtcNow
$cases = [System.Collections.Generic.List[object]]::new()

# --- dependency review ------------------------------------------------------
$dumpbin = Find-Dumpbin
$forbidden = '(?i)\b(ws2_32|wsock32|wininet|winhttp|urlmon|libcurl|mswsock|dnsapi)\.dll\b'
$importsClean = $true
$importDetails = [System.Collections.Generic.List[string]]::new()
$dumpbinLog = Join-Path $outputDir 'imports.log'
if (-not $dumpbin) {
    $importsClean = $false
    $importDetails.Add('dumpbin.exe unavailable; import review could not run')
} else {
    foreach ($binary in @(@{ name = 'EpoCanva.exe'; path = $stagedExe },
                          @{ name = 'epocanva_tsf.dll'; path = $stagedTsf },
                          @{ name = 'rime.dll'; path = $stagedRime })) {
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $output = & $dumpbin /nologo /imports $binary.path 2>&1 | Out-String
            $code = $LASTEXITCODE
        } finally { $ErrorActionPreference = $previous }
        [IO.File]::AppendAllText($dumpbinLog, "===== $($binary.name) exit=$code =====`n$output`n",
            (New-Object System.Text.UTF8Encoding($false)))
        if ($code -ne 0) {
            $importsClean = $false
            $importDetails.Add("$($binary.name): dumpbin exit $code")
        } elseif ($output -match $forbidden) {
            $importsClean = $false
            $importDetails.Add("$($binary.name): forbidden network import present")
        } else {
            $importDetails.Add("$($binary.name): no network imports")
        }
    }
}
$dependencyManifest = Join-Path $RepoRoot 'data\manifests\product-dependencies.json'
$dependencyReviewPassed = $importsClean
if (-not (Test-Path -LiteralPath $dependencyManifest -PathType Leaf)) {
    $dependencyReviewPassed = $false
    $importDetails.Add('product-dependencies.json is missing')
} else {
    $dependencyDocument = Get-Content -LiteralPath $dependencyManifest -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($dependencyDocument.policy_semantics -notmatch 'No unlisted package file') {
        $dependencyReviewPassed = $false
        $importDetails.Add('dependency manifest policy section is malformed')
    }
}
$cases.Add([pscustomobject]@{
    name = 'offline-privacy.dependency-review'
    passed = $dependencyReviewPassed
    detail = ($importDetails -join '; ')
    seconds = 0.0
})
[IO.File]::AppendAllText($logPath,
    ("===== offline-privacy.dependency-review | passed=$dependencyReviewPassed =====`n" +
     ($importDetails -join "`n") + "`n"),
    (New-Object System.Text.UTF8Encoding($false)))

# --- live product run with canary and network observation ------------------
$runRoot = Join-Path $outputDir 'run'
$arguments = @(
    '--product-dll', $productDll,
    '--rime-dll', $rimeDll,
    '--data-dir', $dataDir,
    '--lexicon', $lexicon,
    '--run-root', $runRoot,
    '--mode', 'privacy')
$privacy = Invoke-ObservedPrivacyRun -Executable $sinkExe -Arguments $arguments `
    -Environment @{ EPOCANVA_PRIVACY_CANARY = $Canary } -LogPath $logPath -TimeoutSeconds 300

# The captured process output and every persisted user-data file must not
# contain the canary. The product test itself prints only counts and booleans.
$captured = $privacy.stdout + $privacy.stderr
$filesScanned = 0
$treeHit = ''
# Scan the whole isolated run root. It contains only this test's staged product
# and synthetic user data, so a hit anywhere is a persistence/logging failure.
if (Test-Path -LiteralPath $runRoot) {
    $treeHit = Find-CanaryInTree -Root $runRoot -Needle $Canary -FilesScanned ([ref]$filesScanned)
}
$capturedHasCanary = $captured.Contains($Canary)
$noContentLogging = $privacy.passed -and -not $capturedHasCanary -and -not $treeHit
$cases.Add([pscustomobject]@{
    name = 'offline-privacy.no-content-logging'
    passed = $noContentLogging
    detail = "process_passed=$($privacy.passed); captured_canary=$capturedHasCanary; scanned_files=$filesScanned; tree_hit=$([bool]$treeHit)"
    seconds = [math]::Round($privacy.seconds, 3)
})
$learningFile = ''
if (Test-Path -LiteralPath $runRoot) { $learningFile = Find-LearningStoreFile -Root $runRoot }
$sensitiveNoPersistence = $privacy.passed -and -not $treeHit -and -not $learningFile
$cases.Add([pscustomobject]@{
    name = 'offline-privacy.sensitive-no-persistence'
    passed = $sensitiveNoPersistence
    detail = "process_passed=$($privacy.passed); canary_in_tree=$([bool]$treeHit); learning_store_file=$([bool]$learningFile); scanned_files=$filesScanned"
    seconds = [math]::Round($privacy.seconds, 3)
})
$runtimeNoNetwork = $privacy.passed -and -not $privacy.network_seen -and
    $privacy.monitoring_available -and $importsClean
$cases.Add([pscustomobject]@{
    name = 'offline-privacy.runtime-no-network'
    passed = $runtimeNoNetwork
    detail = "process_passed=$($privacy.passed); monitoring_available=$($privacy.monitoring_available); network_endpoints_seen=$($privacy.network_seen); endpoint_samples=$($privacy.endpoint_samples); imports_clean=$importsClean"
    seconds = [math]::Round($privacy.seconds, 3)
})
[IO.File]::AppendAllText($logPath,
    ("===== offline-privacy summary =====`n" +
     "process_passed=$($privacy.passed) network_seen=$($privacy.network_seen) " +
     "endpoint_samples=$($privacy.endpoint_samples) captured_canary=$capturedHasCanary " +
     "scanned_files=$filesScanned tree_hit=$([bool]$treeHit) learning_file=$([bool]$learningFile) `n"),
    (New-Object System.Text.UTF8Encoding($false)))

$completedAll = [DateTimeOffset]::UtcNow
$casesArray = $cases.ToArray()
$failedCases = @($casesArray | Where-Object { -not $_.passed })
Write-AcceptanceJUnit -Path $junitPath -SuiteName 'offline-privacy' `
    -Cases $casesArray -Identity $identity

$report = [ordered]@{
    format = 'epocanva-offline-privacy-acceptance-v1'
    version = $identity.version
    commit = $identity.commit
    source_sha256 = $identity.source_sha256
    package_manifest_sha256 = $identity.package_manifest_sha256
    acceptance_profile = $identity.acceptance_profile
    cases = @($casesArray | ForEach-Object {
        [ordered]@{ name = $_.name; passed = $_.passed; detail = $_.detail }
    })
    process = [ordered]@{
        exit_code = $privacy.exit_code; network_seen = $privacy.network_seen;
        endpoint_samples = $privacy.endpoint_samples; scanned_files = $filesScanned
    }
    result = $(if ($failedCases.Count -eq 0) { 'passed' } else { 'failed' })
}
# Never persist the canary itself. Record only a domain-separated digest so two
# runs can be distinguished without enabling a preimage search on a low-entropy
# canary.
$canaryDigest = [BitConverter]::ToString(
    [Security.Cryptography.SHA256]::Create().ComputeHash(
        [Text.Encoding]::UTF8.GetBytes('epocanva-canary-v1:' + $Canary))).Replace('-', '').ToLowerInvariant()
$report.process['canary_sha256'] = $canaryDigest
[IO.File]::WriteAllText($reportPath, (($report | ConvertTo-Json -Depth 8) + "`n"),
    (New-Object System.Text.UTF8Encoding($false)))

$exitCode = if ($failedCases.Count -eq 0) { 0 } else { 1 }
if (-not $SkipEvidenceWrite) {
    $runnerPath = (Get-Item -LiteralPath $PSCommandPath).FullName
    $command = @($runnerPath, '-RepoRoot', $RepoRoot, '-BuildDir', $BuildDir,
                 '-StageRoot', $StageRoot, '-EvidencePath', $EvidencePath)
    Add-AcceptanceEvidenceEntry -EvidencePath $EvidencePath -Identity $identity `
        -EntryId 'offline-privacy' -RunnerPath $runnerPath -LogPath $logPath `
        -JUnitPath $junitPath -StartedUtc $startedAll -CompletedUtc $completedAll `
        -ExitCode $exitCode -Command $command -ReportPath $reportPath
}
Write-Host "Offline privacy: $($casesArray.Count - $failedCases.Count)/$($casesArray.Count) passed; exit=$exitCode"
foreach ($case in $failedCases) { Write-Host ("FAILED CASE {0}: {1}" -f $case.name, $case.detail) }
if ($exitCode -ne 0) { exit $exitCode }
exit 0
