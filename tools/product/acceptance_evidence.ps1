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

function Get-AcceptanceSha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-AcceptanceFullPath([string]$Path) {
    return (Get-Item -LiteralPath $Path -Force).FullName
}

function New-AcceptanceReference([string]$Path, [string]$RepoRoot) {
    $full = Get-AcceptanceFullPath $Path
    $repo = (Get-AcceptanceFullPath $RepoRoot).TrimEnd('\', '/')
    if (-not $full.StartsWith($repo + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Evidence reference is outside the repository: $full"
    }
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
    . (Join-Path $repo 'tools\product\build_identity.ps1')
    $receiptPath = Join-Path $BuildDir 'product-build-receipt.json'
    if (-not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) {
        throw 'product-build-receipt.json is missing; run build_product.ps1 with tests'
    }
    $receipt = Get-Content -LiteralPath $receiptPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($receipt.format -ne 1 -or $receipt.status -ne 'verified') {
        throw 'The product build receipt is not a verified test-executed build'
    }
    $current = Get-ProductSourceIdentity -RepoRoot $repo
    if ($receipt.source.commit -cne $current.commit -or
            $receipt.source.source_sha256 -cne $current.source_sha256 -or
            $receipt.source.head -cne $current.head) {
        throw 'The current source differs from the tested build receipt; rebuild and retest'
    }
    $manifestPath = Join-Path $StageRoot 'package.manifest'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw 'The staged package.manifest is missing; run stage_product.ps1'
    }
    $metadata = Read-PackageMetadata $manifestPath
    if ($metadata['commit'] -cne $current.commit) {
        throw 'The staged package commit differs from the current tested source'
    }
    $version = (Get-Content -LiteralPath (Join-Path $repo 'platforms\windows\product\VERSION') -Raw -Encoding UTF8).Trim()
    if ($metadata['version'] -cne $version) { throw 'The staged package version differs from product VERSION' }
    $policyPath = Join-Path $repo 'data\manifests\product-package-files.inc'
    $languagePath = Join-Path $repo 'data\manifests\language-readiness.json'
    $licensePath = Join-Path $repo 'data\manifests\license-decision.json'
    return [pscustomobject]@{
        repo = $repo
        build_dir = (Get-AcceptanceFullPath $BuildDir)
        stage_root = (Get-AcceptanceFullPath $StageRoot)
        version = $version
        commit = $current.commit
        source_sha256 = $current.source_sha256
        package_manifest_sha256 = Get-AcceptanceSha256 $manifestPath
        build_receipt_sha256 = Get-AcceptanceSha256 $receiptPath
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
    $document = Get-Content -LiteralPath $EvidencePath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($document.format -ne 2 -or $null -eq $document.entries) {
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
        if ([string]$document.$key -cne [string]$bindings[$key]) {
            throw "release-evidence.json identity mismatch: $key"
        }
    }
    $runnerFull = Get-AcceptanceFullPath $RunnerPath
    if ($Command.Count -lt 1 -or $Command[0] -cne $runnerFull) {
        throw 'The recorded command must begin with the exact runner path'
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
    $json = $document | ConvertTo-Json -Depth 12
    [IO.File]::WriteAllText($EvidencePath, $json + "`n", (New-Object System.Text.UTF8Encoding($false)))
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
