# Shared producer helpers for acceptance test-run evidence.
#
# This file is dot-sourced by the English, offline-privacy and language
# runners. It does not execute a product or a test by itself and it never
# invents an owner, installer, host or signing record. Every entry it writes is
# a hash-bound record of a process that the caller actually ran.
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'build_identity.ps1')

function Get-AcceptanceSha256([string]$Path) {
    return Get-ProductIdentityFileSha256 $Path
}

function Get-AcceptanceFullPath([string]$Path) {
    return (Get-Item -LiteralPath $Path -Force).FullName
}

function New-AcceptanceReference([string]$Path, [string]$RepoRoot) {
    $full = Get-AcceptanceFullPath $Path
    $repo = (Get-AcceptanceFullPath $RepoRoot).TrimEnd('\', '/')
    $full = Assert-ProductPlainPath $full $repo
    $file = Get-Item -LiteralPath $full -Force
    if ($file.PSIsContainer -or $file.Length -eq 0) { throw "Evidence reference is empty: $full" }
    return [pscustomobject]@{
        path = $full.Substring($repo.Length + 1).Replace('\', '/')
        sha256 = Get-AcceptanceSha256 $full
        bytes = [long]$file.Length
    }
}

function Read-PackageMetadata([string]$ManifestPath) {
    $metadata = @{}
    foreach ($line in (Get-Content -LiteralPath $ManifestPath -Encoding UTF8)) {
        if ($line -cmatch '^(version|commit|acceptance-profile|distribution-policy|path-exception)=(.+)$') {
            if ($metadata.ContainsKey($Matches[1])) { throw "Duplicate package metadata: $($Matches[1])" }
            $metadata[$Matches[1]] = $Matches[2]
        }
    }
    return $metadata
}

function Get-AcceptanceIdentity {
    param(
        [Parameter(Mandatory=$true)][string]$RepoRoot,
        [Parameter(Mandatory=$true)][string]$BuildDir,
        [Parameter(Mandatory=$true)][string]$StageRoot
    )
    $repo = Get-AcceptanceFullPath $RepoRoot
    $buildBase = Join-Path $repo 'build'
    $BuildDir = Assert-ProductPlainPath $BuildDir $buildBase
    $StageRoot = Assert-ProductPlainPath $StageRoot $buildBase
    $receiptPath = Assert-ProductPlainPath (Join-Path $BuildDir 'product-build-receipt.json') $buildBase
    if (-not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) {
        throw 'product-build-receipt.json is missing; run build_product.ps1 with tests'
    }
    $receiptHash = Get-AcceptanceSha256 $receiptPath
    $receipt = Read-ProductJson $receiptPath
    $engines = @($receipt.binaries | Where-Object { [object]::Equals($_.name, 'rime') })
    if ($engines.Count -ne 1 -or $engines[0].path -isnot [string] -or -not [IO.Path]::IsPathRooted($engines[0].path)) {
        throw 'Build receipt lacks a unique tested English engine path.'
    }
    $receipt = Assert-ProductBuildReceipt -RepoRoot $repo -BuildDir $BuildDir -RimeDll $engines[0].path
    $null = Assert-ProductBuildEngineBinding $repo $engines[0].path $receipt
    if ((Get-AcceptanceSha256 $receiptPath) -cne $receiptHash) { throw 'Product build receipt changed during verification.' }
    $current = $receipt.source
    $manifestPath = Assert-ProductPlainPath (Join-Path $StageRoot 'package.manifest') $StageRoot
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw 'The staged package.manifest is missing; run stage_product.ps1'
    }
    $manifestHash = Get-AcceptanceSha256 $manifestPath
    $metadata = Read-PackageMetadata $manifestPath
    if ($metadata['commit'] -cne $current.commit) {
        throw 'The staged package commit differs from the current tested source'
    }
    $version = (Get-Content -LiteralPath (Join-Path $repo 'platforms\windows\product\VERSION') -Raw -Encoding UTF8).Trim()
    if ($metadata['version'] -cne $version) { throw 'The staged package version differs from product VERSION' }
    $policyPath = Join-Path $repo 'data\manifests\product-package-files.inc'
    $languagePath = Join-Path $repo 'data\manifests\language-readiness.json'
    $licensePath = Join-Path $repo 'data\manifests\license-decision.json'
    foreach ($file in @(@('exe', 'EpoCanva.exe'), @('tsf', 'epocanva_tsf.dll'), @('rime', 'rime.dll'))) {
        $binding = @($receipt.binaries | Where-Object { $_.name -ceq $file[0] })[0]
        $path = Assert-ProductPlainPath (Join-Path $StageRoot $file[1]) $StageRoot
        if ((Get-AcceptanceSha256 $path) -cne $binding.sha256) { throw "Staged $($file[1]) differs from the tested binary." }
    }
    if ((Get-AcceptanceSha256 $manifestPath) -cne $manifestHash) { throw 'Package manifest changed during verification.' }
    return [pscustomobject]@{
        repo = $repo
        build_dir = (Get-AcceptanceFullPath $BuildDir)
        stage_root = (Get-AcceptanceFullPath $StageRoot)
        version = $version
        commit = $current.commit
        source_sha256 = $current.source_sha256
        package_manifest_sha256 = $manifestHash
        build_receipt_sha256 = $receiptHash
        package_policy_sha256 = Get-AcceptanceSha256 $policyPath
        language_policy_sha256 = Get-AcceptanceSha256 $languagePath
        license_decision_sha256 = Get-AcceptanceSha256 $licensePath
        acceptance_profile = [string]$metadata['acceptance-profile']
        distribution_policy = [string]$metadata['distribution-policy']
        path_exception = [string]$metadata['path-exception']
        receipt = $receipt
    }
}

function Write-AcceptanceJUnit {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][string]$SuiteName,
        [Parameter(Mandatory=$true)][object[]]$Cases,
        [Parameter(Mandatory=$true)]$Identity
    )
    $directory = Split-Path -Parent $Path
    if ($directory) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
    $settings = New-Object System.Xml.XmlWriterSettings
    $settings.Indent = $true
    $settings.Encoding = New-Object System.Text.UTF8Encoding($false)
    $writer = [System.Xml.XmlWriter]::Create($Path, $settings)
    try {
        $writer.WriteStartDocument()
        $writer.WriteStartElement('testsuite')
        $writer.WriteAttributeString('name', $SuiteName)
        $writer.WriteAttributeString('tests', [string]$Cases.Count)
        $writer.WriteAttributeString('failures', [string]@($Cases | Where-Object { -not $_.passed }).Count)
        $writer.WriteAttributeString('errors', '0')
        $writer.WriteAttributeString('skipped', '0')
        $writer.WriteAttributeString('timestamp', [DateTimeOffset]::UtcNow.ToString('o'))
        $writer.WriteStartElement('properties')
        $properties = [ordered]@{
            version = $Identity.version
            commit = $Identity.commit
            source_sha256 = $Identity.source_sha256
            package_manifest_sha256 = $Identity.package_manifest_sha256
            language_policy_sha256 = $Identity.language_policy_sha256
            acceptance_profile = $Identity.acceptance_profile
        }
        if ($Identity.PSObject.Properties['installer_sha256']) {
            $properties['installer_sha256'] = $Identity.installer_sha256
        }
        foreach ($key in $properties.Keys) {
            $writer.WriteStartElement('property')
            $writer.WriteAttributeString('name', 'epocanva.' + $key)
            $writer.WriteAttributeString('value', [string]$properties[$key])
            $writer.WriteEndElement()
        }
        $writer.WriteEndElement()  # properties
        foreach ($case in $Cases) {
            $writer.WriteStartElement('testcase')
            $writer.WriteAttributeString('name', [string]$case.name)
            $writer.WriteAttributeString('classname', $SuiteName)
            $writer.WriteAttributeString('time', ('{0:F3}' -f [double]$case.seconds))
            if (-not $case.passed) {
                $writer.WriteStartElement('failure')
                $writer.WriteAttributeString('message', [string]$case.detail)
                $writer.WriteString([string]$case.detail)
                $writer.WriteEndElement()
            }
            $writer.WriteEndElement()
        }
        $writer.WriteEndElement()  # testsuite
        $writer.WriteEndDocument()
    } finally {
        $writer.Dispose()
    }
}

function Add-AcceptanceEvidenceEntry {
    param(
        [Parameter(Mandatory=$true)][string]$EvidencePath,
        [Parameter(Mandatory=$true)]$Identity,
        [Parameter(Mandatory=$true)][string]$EntryId,
        [Parameter(Mandatory=$true)][string]$RunnerPath,
        [Parameter(Mandatory=$true)][string]$LogPath,
        [Parameter(Mandatory=$true)][string]$JUnitPath,
        [Parameter(Mandatory=$true)][datetimeoffset]$StartedUtc,
        [Parameter(Mandatory=$true)][datetimeoffset]$CompletedUtc,
        [Parameter(Mandatory=$true)][int]$ExitCode,
        [Parameter(Mandatory=$true)][string[]]$Command,
        [string]$ReportPath = ''
    )
    if (-not (Test-Path -LiteralPath $EvidencePath -PathType Leaf)) {
        throw 'release-evidence.json is missing; run record_package_diagnose.ps1 first'
    }
    $EvidencePath = Assert-ProductPlainPath $EvidencePath (Join-Path $Identity.repo 'build')
    $evidenceHash = Get-AcceptanceSha256 $EvidencePath
    $document = Read-ProductJson $EvidencePath
    if (-not (Test-ProductJsonInteger $document.format) -or $document.format -ne 2 -or $document.entries -isnot [array]) {
        throw 'release-evidence.json is not format 2 with an entries array'
    }
    $bindings = @{
        version = $Identity.version
        commit = $Identity.commit
        source_sha256 = $Identity.source_sha256
        package_manifest_sha256 = $Identity.package_manifest_sha256
        build_receipt_sha256 = $Identity.build_receipt_sha256
        package_policy_sha256 = $Identity.package_policy_sha256
        acceptance_profile = $Identity.acceptance_profile
        distribution_policy = $Identity.distribution_policy
        license_decision_sha256 = $Identity.license_decision_sha256
        language_policy_sha256 = $Identity.language_policy_sha256
    }
    foreach ($key in $bindings.Keys) {
        if (-not [object]::Equals($document.$key, $bindings[$key])) {
            throw "release-evidence.json identity mismatch: $key"
        }
    }
    $runnerFull = Get-AcceptanceFullPath $RunnerPath
    if ($Command.Count -lt 1 -or $Command[0] -cne $runnerFull) {
        throw 'The recorded command must begin with the exact runner path'
    }
    if ($ExitCode -ne 0 -or $CompletedUtc -lt $StartedUtc -or $CompletedUtc -gt [DateTimeOffset]::UtcNow.AddMinutes(5)) {
        throw 'Evidence entry requires a successful process and ordered execution timestamps.'
    }
    $null = Read-ProductPassingJUnit (Assert-ProductPlainPath $JUnitPath $Identity.repo)
    if ((Get-AcceptanceSha256 (Join-Path $Identity.build_dir 'product-build-receipt.json')) -cne $Identity.build_receipt_sha256 -or
            (Get-AcceptanceSha256 (Join-Path $Identity.stage_root 'package.manifest')) -cne $Identity.package_manifest_sha256) {
        throw 'Build or package changed during the acceptance run.'
    }
    $entry = [pscustomobject][ordered]@{
        id = $EntryId
        kind = 'test-run'
        runner = New-AcceptanceReference $runnerFull $Identity.repo
        log = New-AcceptanceReference $LogPath $Identity.repo
        junit = New-AcceptanceReference $JUnitPath $Identity.repo
        exit_code = [int]$ExitCode
        started_utc = $StartedUtc.ToUniversalTime().ToString('o')
        completed_utc = $CompletedUtc.ToUniversalTime().ToString('o')
        command = @($Command)
    }
    if ($ReportPath) {
        $entry | Add-Member -NotePropertyName report -NotePropertyValue (New-AcceptanceReference $ReportPath $Identity.repo)
    }
    $entries = @($document.entries | Where-Object { $_.id -cne $EntryId })
    $entries += $entry
    $document.entries = $entries
    if ((Get-AcceptanceSha256 $EvidencePath) -cne $evidenceHash) { throw 'Release evidence changed during entry generation.' }
    Write-ProductJson $EvidencePath $document
}

function Invoke-AcceptanceCase {
    param(
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)][string]$Executable,
        [Parameter(Mandatory=$true)][string[]]$Arguments,
        [Parameter(Mandatory=$true)][string]$LogPath,
        [string]$WorkingDirectory = '',
        [hashtable]$Environment = @{},
        [int]$TimeoutSeconds = 900,
        [string]$SuccessMarker = 'RESULT PASS'
    )
    $started = [DateTimeOffset]::UtcNow
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $oldEnvironment = @{}
    foreach ($key in $Environment.Keys) {
        $oldEnvironment[$key] = [Environment]::GetEnvironmentVariable($key)
        [Environment]::SetEnvironmentVariable($key, [string]$Environment[$key])
    }
    try {
        $startInfo = New-Object System.Diagnostics.ProcessStartInfo
        $startInfo.FileName = $Executable
        $startInfo.Arguments = (($Arguments | ForEach-Object {
            if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
        }) -join ' ')
        $startInfo.UseShellExecute = $false
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $startInfo.CreateNoWindow = $true
        if ($WorkingDirectory) { $startInfo.WorkingDirectory = $WorkingDirectory }
        $process = New-Object System.Diagnostics.Process
        $process.StartInfo = $startInfo
        $null = $process.Start()
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            try { $process.Kill() } catch { }
            throw "Case $Name exceeded $TimeoutSeconds seconds"
        }
        $stdout = $stdoutTask.Result
        $stderr = $stderrTask.Result
        $exitCode = $process.ExitCode
    } finally {
        foreach ($key in $Environment.Keys) {
            [Environment]::SetEnvironmentVariable($key, $oldEnvironment[$key])
        }
        $ErrorActionPreference = $previous
    }
    $watch.Stop()
    $completed = [DateTimeOffset]::UtcNow
    $detail = "exit_code=$exitCode"
    $passed = $exitCode -eq 0
    if ($passed -and $SuccessMarker -and (($stdout + $stderr) -notmatch [regex]::Escape($SuccessMarker))) {
        $passed = $false
        $detail = "exit_code=0 but the success marker '$SuccessMarker' is absent"
    }
    $logDirectory = Split-Path -Parent $LogPath
    if ($logDirectory) { New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null }
    $header = "===== $Name | exit=$exitCode | started=$($started.ToString('o')) | completed=$($completed.ToString('o')) ====="
    $body = $header + "`n" + $stdout + $stderr + "`n"
    [IO.File]::AppendAllText($LogPath, $body, (New-Object System.Text.UTF8Encoding($false)))
    return [pscustomobject]@{
        name = $Name
        passed = $passed
        exit_code = [int]$exitCode
        detail = $detail
        seconds = $watch.Elapsed.TotalSeconds
        started_utc = $started
        completed_utc = $completed
        stdout = $stdout
        stderr = $stderr
    }
}
