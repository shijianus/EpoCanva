# Read-only acceptance/release gate. Never builds, stages, installs or starts a GUI.
# Missing technical evidence blocks either profile; only internal signing is not required.
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$BuildDir = '',
    [string]$StageRoot = '',
    [string]$RimeDll = '',
    [string]$EvidencePath = '',
    [ValidateSet('internal-unsigned', 'public-signed')]
    [string]$AcceptanceProfile = 'internal-unsigned'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'build_identity.ps1')
$sourceContractPath = Join-Path $PSScriptRoot 'source_contract.ps1'
$sourceContractLoaded = $false
if (Test-Path -LiteralPath $sourceContractPath -PathType Leaf) {
    . $sourceContractPath
    $sourceContractLoaded = $true
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Assert-RelativePath([string]$Path) {
    if (-not $Path -or $Path -notmatch '^[A-Za-z0-9_.\-/]+$') { throw 'Invalid relative payload/evidence path.' }
    foreach ($part in ($Path -split '/')) {
        if (-not $part -or $part -in @('.', '..') -or $part.EndsWith('.')) {
            throw 'Invalid relative path component.'
        }
    }
}
function Assert-PlainPath([string]$Path, [string]$Boundary) {
    $itemPath = [IO.Path]::GetFullPath($Path)
    $base = [IO.Path]::GetFullPath($Boundary).TrimEnd('\', '/')
    if ($itemPath -ne $base -and -not $itemPath.StartsWith($base + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) { throw 'Path is outside its permitted directory.' }
    while ($itemPath -and $itemPath.Length -ge $base.Length) {
        if (Test-Path -LiteralPath $itemPath) {
            $item = Get-Item -LiteralPath $itemPath -Force
            # OneDrive hydration placeholders are reparse points but do not redirect paths.
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -and
                    ($item.LinkType -or @($item.Target).Count -gt 0)) {
                throw 'Symbolic links/junctions are not accepted for release inputs or output.'
            }
        }
        if ($itemPath -eq $base) { break }
        $itemPath = Split-Path -Parent $itemPath
    }
}
function Add-Gate([string]$Id, [scriptblock]$Check, [string]$Category = 'technical') {
    try {
        $detail = @(& $Check) -join ' '
        $script:checks.Add([pscustomobject]@{ id = $Id; category = $Category; status = 'passed'; detail = $detail })
    } catch {
        $script:checks.Add([pscustomobject]@{ id = $Id; category = $Category; status = 'blocked'; detail = $_.Exception.Message })
    }
}
function Test-GatePassed([string]$Id) {
    return @($script:checks | Where-Object { $_.id -ceq $Id -and $_.status -ceq 'passed' }).Count -eq 1
}
function Read-PassingJUnit([string]$Path) {
    $settings = New-Object System.Xml.XmlReaderSettings
    $settings.DtdProcessing = [System.Xml.DtdProcessing]::Prohibit
    $settings.XmlResolver = $null
    $reader = [System.Xml.XmlReader]::Create($Path, $settings)
    try {
        $xml = New-Object System.Xml.XmlDocument
        $xml.XmlResolver = $null
        $xml.Load($reader)
    } finally { $reader.Dispose() }
    if ($xml.DocumentElement.Name -notin @('testsuite', 'testsuites')) { throw 'Unsupported JUnit document.' }
    $cases = @($xml.SelectNodes('//testcase'))
    if ($cases.Count -eq 0 -or $xml.SelectNodes('//failure | //error | //skipped').Count -ne 0) {
        throw 'Tests are absent, failed, errored or skipped; none can be waived for release.'
    }
    foreach ($suite in $xml.SelectNodes('//testsuite | /testsuites')) {
        foreach ($attribute in @('failures', 'errors', 'skipped', 'disabled')) {
            if ($suite.HasAttribute($attribute) -and $suite.GetAttribute($attribute) -notmatch '^0+$') {
                throw 'JUnit summary reports failed, skipped or disabled tests.'
            }
        }
        if ($suite.HasAttribute('tests') -and
                ($suite.GetAttribute('tests') -notmatch '^\d+$' -or
                 [uint64]$suite.GetAttribute('tests') -ne $suite.SelectNodes('.//testcase').Count)) {
            throw 'JUnit summary test count disagrees with actual test cases.'
        }
    }
    foreach ($case in $cases) {
        if (-not $case.GetAttribute('name') -or
            ($case.HasAttribute('status') -and $case.GetAttribute('status') -notin @('run', 'passed')) -or
            ($case.HasAttribute('result') -and $case.GetAttribute('result') -notin @('completed', 'passed'))) {
            throw 'JUnit contains an unexecuted or nonpassing test.'
        }
    }
    return ,$cases
}
function Read-Reference($Reference, [switch]$CorrespondingSource) {
    if ($CorrespondingSource) {
        Assert-ProductSourceRelativePath $Reference.path
    } else {
        Assert-RelativePath $Reference.path
    }
    if ($Reference.sha256 -cnotmatch '^[0-9a-f]{64}$' -or [string]$Reference.bytes -notmatch '^\d+$' -or
            [uint64]$Reference.bytes -eq 0) {
        throw 'An evidence reference requires SHA-256 and an exact nonzero byte count.'
    }
    $path = Join-Path $RepoRoot $Reference.path
    Assert-PlainPath $path $RepoRoot
    $file = Get-Item -LiteralPath $path -Force
    if ($file.PSIsContainer -or $file.Length -ne [uint64]$Reference.bytes -or
            (Get-Sha256 $path) -cne $Reference.sha256) { throw 'Evidence file hash/byte count mismatch.' }
    $script:references.Add([pscustomobject]@{ path = $file.FullName; sha256 = $Reference.sha256; bytes = $Reference.bytes })
    return $file.FullName
}
function Assert-Execution($Entry) {
    if ($Entry.exit_code -isnot [int] -or $Entry.exit_code -ne 0) { throw 'A real successful process exit code is required.' }
    $start = [DateTimeOffset]::Parse($Entry.started_utc)
    $end = [DateTimeOffset]::Parse($Entry.completed_utc)
    if ($end -lt $start -or $end -gt [DateTimeOffset]::UtcNow.AddMinutes(5)) { throw 'Invalid execution timestamps.' }
    if ($Entry.command -isnot [array] -or @($Entry.command).Count -lt 1 -or
            @($Entry.command | Where-Object { $_ -isnot [string] -or [string]::IsNullOrWhiteSpace($_) }).Count -ne 0) {
        throw 'The exact executed command must be recorded as a nonempty string array.'
    }
}

# Validate the output boundary before creating anything. Invalid output paths exit 2.
try {
    if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
    $RepoRoot = (Get-Item -LiteralPath $RepoRoot).FullName
    $buildBase = [IO.Path]::GetFullPath((Join-Path $RepoRoot 'build'))
    if (-not $BuildDir) { $BuildDir = Join-Path $buildBase 'windows-product' }
    $BuildDir = [IO.Path]::GetFullPath($BuildDir).TrimEnd('\', '/')
    if ((Split-Path -Parent $BuildDir) -ne $buildBase -or
            (Split-Path -Leaf $BuildDir) -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$') {
        throw 'BuildDir must be exactly <repo>/build/<preset>.'
    }
    $outputPath = Join-Path $BuildDir 'release-readiness.json'
    Assert-PlainPath $outputPath $buildBase
    if (-not (Test-Path -LiteralPath $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null }
} catch {
    Write-Error -ErrorAction Continue ('Release gate output unavailable: ' + $_.Exception.Message)
    exit 2
}

$checks = [System.Collections.Generic.List[object]]::new()
$source = $null
$receipt = $null
$receiptHash = ''
$version = ''
$policy = @{}
$policyHash = ''
$manifest = $null
$evidence = $null
$evidenceHash = ''
$licenseDecisionHash = ''
$languagePolicyHash = ''
$languagePolicyPath = Join-Path $RepoRoot 'data\manifests\language-readiness.json'
$sourceManifestHash = ''
$installer = $null
$references = [System.Collections.Generic.List[object]]::new()
$tools = [ordered]@{}
$receiptPath = Join-Path $BuildDir 'product-build-receipt.json'
$policyPath = Join-Path $RepoRoot 'data\manifests\product-package-files.inc'
if (-not $RimeDll) { $RimeDll = Join-Path $RepoRoot 'build\librime-english\rime\install\lib\rime.dll' }
if (-not $EvidencePath) { $EvidencePath = Join-Path $BuildDir 'release-evidence.json' }

Add-Gate 'source-identity' {
    $script:source = Get-ProductSourceIdentity -RepoRoot $RepoRoot
    $script:version = (Get-Content -LiteralPath (Join-Path $RepoRoot 'platforms\windows\product\VERSION') -Raw -Encoding UTF8).Trim()
    if (-not $version) { throw 'Product VERSION is empty.' }
    'Current source fingerprint and full commit identity recorded.'
}
if (-not $StageRoot) { $StageRoot = Join-Path $BuildDir "stage\EpoCanva-$version" }
$StageRoot = [IO.Path]::GetFullPath($StageRoot)
Add-Gate 'verified-build' {
    $verified = Assert-ProductBuildReceipt -RepoRoot $RepoRoot -BuildDir $BuildDir -RimeDll $RimeDll
    $script:receiptHash = Get-Sha256 $receiptPath
    $script:receipt = $verified
    'Release/non-trace receipt matches current source and all three binary hashes/byte counts.'
}
Add-Gate 'engine-identity' {
    $engine = Assert-ProductEngineReceipt -RepoRoot $RepoRoot -RimeDll $RimeDll
    if ($null -eq $receipt -or $receipt.engine_source_sha256 -ne $engine.source_sha256) {
        throw 'Product receipt does not match the verified English engine source.'
    }
    'English source, pinned dependency checkouts, disabled logging/plugins and runtime bytes agree.'
}
Add-Gate 'build-tests' {
    if ($null -eq $receipt) { throw 'No valid build receipt; build/test evidence is unavailable.' }
    if ($receipt.tests.skipped -ne 0) { throw 'Build tests were skipped; release requires all necessary tests executed.' }
    $xmlPath = Join-Path $receipt.evidence 'ctest.xml'
    $listPath = Join-Path $receipt.evidence 'ctest-list.json'
    if ((Get-Sha256 $xmlPath) -cne $receipt.ctest_xml_sha256 -or
        (Get-Sha256 $listPath) -cne $receipt.ctest_list_sha256) {
        throw 'Recorded CTest list or result changed after build verification.'
    }
    $cases = Read-PassingJUnit $xmlPath
    $listed = @((Get-Content -LiteralPath $listPath -Raw -Encoding UTF8 | ConvertFrom-Json).tests | ForEach-Object { $_.name })
    $executed = @($cases | ForEach-Object { $_.GetAttribute('name') })
    if ($listed.Count -ne $executed.Count -or
        @(Compare-Object ($listed | Sort-Object) ($executed | Sort-Object)).Count -ne 0) {
        throw 'CTest execution did not match the registered test set.'
    }
    if ($cases.Count -ne $receipt.tests.total -or $cases.Count -ne $receipt.tests.passed) {
        throw 'CTest XML does not agree with the build receipt.'
    }
    "$($cases.Count) actual passing CTest cases; no skips."
}
Add-Gate 'source-clean' {
    if ($null -eq $receipt) { throw 'No verified build receipt exists; a fixture package cannot establish the clean source revision.' }
    if ($null -eq $source -or $source.dirty) { throw 'Aggregate acceptance requires a clean source revision; dirty identity is preserved, never stripped.' }
    'Source worktree is clean.'
}
Add-Gate 'package-policy' {
    $parsed = @{}
    foreach ($line in (Get-Content -LiteralPath $policyPath -Encoding UTF8)) {
        if (-not $line.Trim() -or $line.TrimStart().StartsWith('//')) { continue }
        if ($line -cnotmatch '^EPOCANVA_PACKAGE_FILE\("([^"]+)",\s*"([^"]*)",\s*"([^"]*)",\s*(\d+)\)$') {
            throw 'Unrecognised shared payload policy row.'
        }
        $relative, $origin, $sha, $bytes = $Matches[1], $Matches[2], $Matches[3], $Matches[4]
        Assert-RelativePath $relative
        if ($relative -eq 'package.manifest' -or $parsed.ContainsKey($relative)) { throw 'Duplicate or self-referential payload policy row.' }
        if ($origin) {
            Assert-RelativePath $origin
            if ($sha -cnotmatch '^[0-9a-f]{64}$' -or [uint64]$bytes -eq 0) { throw 'Fixed payload policy lacks a hash/byte count.' }
        } elseif ($relative -notin @('EpoCanva.exe', 'epocanva_tsf.dll', 'rime.dll', 'VERSION.txt') -or $sha -or [uint64]$bytes -ne 0) {
            throw 'Only the three receipt-bound binaries and generated VERSION.txt may have an empty source.'
        }
        $parsed[$relative] = [pscustomobject]@{ path = $relative; source = $origin; sha256 = $sha; bytes = [uint64]$bytes }
    }
    if ($parsed.Count -eq 0) { throw 'Shared payload policy is empty.' }
    $script:policy = $parsed
    $script:policyHash = Get-Sha256 $policyPath
    "$($policy.Count) payload files required by the shared policy; no hardcoded file count."
}
Add-Gate 'package-integrity' {
    if ($policy.Count -eq 0) { throw 'Shared payload policy is unavailable.' }
    Assert-PlainPath $StageRoot $StageRoot
    $manifestPath = Join-Path $StageRoot 'package.manifest'
    Assert-PlainPath $manifestPath $StageRoot
    $lines = @(Get-Content -LiteralPath $manifestPath -Encoding UTF8 | Where-Object { $_ -and -not $_.StartsWith('#') })
    if ($lines.Count -lt 5 -or $lines[0] -cnotmatch '^epocanva-package-manifest [12]$') {
        throw 'Missing supported package manifest.'
    }
    $format = [int]$lines[0].Substring($lines[0].Length - 1)
    $metadata = @{}
    $files = @{}
    foreach ($line in ($lines | Select-Object -Skip 1)) {
        if ($line -cmatch '^(version|commit|internal-only|acceptance-profile|distribution-policy|path-exception)=(.+)$') {
            if ($metadata.ContainsKey($Matches[1])) { throw 'Duplicate manifest metadata.' }
            $metadata[$Matches[1]] = $Matches[2]
        } elseif ($line -cmatch '^file\t([^\t]+)\t([0-9a-f]{64})\t(\d+)$') {
            $relative, $hash, $bytes = $Matches[1], $Matches[2], [uint64]$Matches[3]
            Assert-RelativePath $relative
            if ($files.ContainsKey($relative) -or -not $policy.ContainsKey($relative) -or $relative -cne $policy[$relative].path) {
                throw 'Duplicate or non-whitelisted manifest payload.'
            }
            $filePath = Join-Path $StageRoot $relative
            Assert-PlainPath $filePath $StageRoot
            $file = Get-Item -LiteralPath $filePath -Force
            if ($file.PSIsContainer -or $file.Length -ne $bytes -or (Get-Sha256 $filePath) -cne $hash) {
                throw "Staged hash/byte count mismatch: $relative"
            }
            $locked = $policy[$relative]
            if ($locked.source -and ($hash -cne $locked.sha256 -or $bytes -ne $locked.bytes)) {
                throw "Staged file differs from the fixed source policy: $relative"
            }
            $files[$relative] = [pscustomobject]@{ sha256 = $hash; bytes = $bytes }
        } else { throw 'Unrecognised package manifest row.' }
    }
    if (-not $metadata['version'] -or -not $metadata['commit'] -or $files.Count -ne $policy.Count) {
        throw 'Manifest requires unique version/commit and the complete shared payload set.'
    }
    if ($format -eq 1) {
        if ($metadata.Count -ne 3 -or $metadata['internal-only'] -cnotmatch '^[01]$') {
            throw 'Legacy manifest requires only version/commit/internal-only metadata.'
        }
    } elseif ($metadata.Count -ne 5 -or
            $metadata['acceptance-profile'] -cnotin @('internal-unsigned', 'public-signed') -or
            $metadata['distribution-policy'] -cnotin @('internal-only', 'public-candidate') -or
            $metadata['path-exception'] -cnotin @('none', 'embedded-build-paths')) {
        throw 'Format 2 requires separate acceptance-profile/distribution-policy/path-exception values.'
    }
    $directories = [System.Collections.Generic.Queue[string]]::new()
    $directories.Enqueue($StageRoot)
    $actualCount = 0
    while ($directories.Count -gt 0) {
        foreach ($item in (Get-ChildItem -LiteralPath $directories.Dequeue() -Force)) {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -and
                    ($item.LinkType -or @($item.Target).Count -gt 0)) { throw 'Package contains a symbolic link/junction.' }
            if ($item.PSIsContainer) { $directories.Enqueue($item.FullName); continue }
            $relative = $item.FullName.Substring($StageRoot.TrimEnd('\', '/').Length + 1).Replace('\', '/')
            if ($relative -ceq 'package.manifest') { continue }
            if (-not $files.ContainsKey($relative) -or $relative -cne $policy[$relative].path) { throw "Extra staged file: $relative" }
            ++$actualCount
        }
    }
    if ($actualCount -ne $files.Count) { throw 'Actual payload set differs from the manifest.' }
    $script:manifest = [pscustomobject]@{ format = $format; version = $metadata['version']; commit = $metadata['commit'];
        acceptance_profile = $metadata['acceptance-profile']; distribution_policy = $metadata['distribution-policy'];
        path_exception = $metadata['path-exception']; sha256 = (Get-Sha256 $manifestPath); files = $files }
    "$actualCount payload files verified; no extras."
}
Add-Gate 'package-identity' {
    if ($null -eq $manifest -or $null -eq $source -or $null -eq $receipt) { throw 'Verified manifest/source/build identity is unavailable.' }
    if ($manifest.version -cne $version -or $manifest.commit -cne $source.commit -or $manifest.commit -cne $receipt.source.commit) {
        throw 'Staged version/commit does not match current source and the tested build.'
    }
    $versionLines = @(Get-Content -LiteralPath (Join-Path $StageRoot 'VERSION.txt') -Encoding UTF8)
    foreach ($pair in @(@('Version', $version), @('Commit', $source.commit),
            @('Acceptance-profile', $manifest.acceptance_profile), @('Distribution-policy', $manifest.distribution_policy),
            @('Path-exception', $manifest.path_exception))) {
        $rows = @($versionLines | Where-Object { $_.StartsWith($pair[0] + ':') })
        if ($rows.Count -ne 1 -or $rows[0] -cne ($pair[0] + ': ' + $pair[1])) { throw 'VERSION.txt identity mismatch or duplicate field.' }
    }
    $names = @{ exe = 'EpoCanva.exe'; tsf = 'epocanva_tsf.dll'; rime = 'rime.dll' }
    foreach ($name in $names.Keys) {
        $binary = @($receipt.binaries | Where-Object { $_.name -eq $name })
        $staged = $manifest.files[$names[$name]]
        if ($binary.Count -ne 1 -or $staged.sha256 -cne $binary[0].sha256 -or $staged.bytes -ne $binary[0].bytes) {
            throw "Staged binary differs from tested bytes: $name"
        }
    }
    'VERSION.txt, manifest, current source and three tested binaries agree.'
}
Add-Gate 'acceptance-profile' {
    if ($null -eq $manifest -or $manifest.format -ne 2) { throw 'Legacy format 1 is diagnostic-only; restage with explicit format 2 policies.' }
    if ($manifest.acceptance_profile -cne $AcceptanceProfile) { throw 'Requested acceptance profile differs from the staged artifact.' }
    if ($manifest.path_exception -cne 'none') { throw 'Embedded build-path exception prevents technical acceptance for either profile.' }
    if ($AcceptanceProfile -ceq 'public-signed' -and $manifest.distribution_policy -cne 'public-candidate') {
        throw 'Public acceptance requires an explicit public-candidate package.'
    }
    "Format 2 acceptance profile: $AcceptanceProfile; path-exception=none. Distribution authorization is separate."
}
Add-Gate 'public-package' {
    if ($null -eq $manifest -or $manifest.format -ne 2 -or $manifest.distribution_policy -cne 'public-candidate') {
        throw 'No format-2 public-distribution candidate; internal-only scope does not authorize public release.'
    }
    'Explicit public candidate metadata verified; this is not owner/legal approval.'
} 'distribution'
Add-Gate 'project-license-selection' {
    $path = Join-Path $RepoRoot 'data\manifests\license-decision.json'
    $script:licenseDecisionHash = Get-Sha256 $path
    $decision = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($decision.format -isnot [int] -or $decision.format -ne 1 -or
            $decision.record_kind -cne 'user-instruction-scope' -or $decision.project_license -cne 'AGPL-3.0-only' -or
            $decision.acceptance_profile -cne 'internal-unsigned' -or $decision.distribution_policy -cne 'internal-only' -or
            $decision.public_distribution -cne 'not-authorized-by-this-record' -or
            $decision.lexicon_jurisdictions -cne 'not-decided-by-this-record') {
        throw 'License decision must record only the actual AGPL-only/internal unsigned instruction, not invented public or data approvals.'
    }
    $rootLicense = Join-Path $RepoRoot 'LICENSE'
    if ((Get-Item -LiteralPath $rootLicense).Length -ne 34523 -or
            (Get-Sha256 $rootLicense) -cne '0d96a4ff68ad6d4b6f1f30f713b18d5184912ba8dd389f86aa7710db079abcb0' -or
            $decision.license_text.path -cne 'LICENSE' -or $decision.license_text.bytes -isnot [int] -or
            $decision.license_text.bytes -ne 34523 -or $decision.license_text.sha256 -cne (Get-Sha256 $rootLicense)) {
        throw 'Root LICENSE or the recorded actual GNU license-text identity differs.'
    }
    if ($null -eq $manifest -or -not $manifest.files.ContainsKey('LICENSE') -or
            -not $manifest.files.ContainsKey('license-decision.json') -or
            $manifest.files['LICENSE'].sha256 -cne (Get-Sha256 $rootLicense) -or
            $manifest.files['license-decision.json'].sha256 -cne $licenseDecisionHash) { throw 'Package does not carry the exact project license and decision scope.' }
    'AGPL-3.0-only and internal unsigned scope recorded; no public/data approval inferred.'
}
Add-Gate 'package-path-hygiene' {
    if ($null -eq $manifest) { throw 'No verified payload to inspect.' }
    foreach ($relative in $manifest.files.Keys) {
        $bytes = [IO.File]::ReadAllBytes((Join-Path $StageRoot $relative))
        foreach ($encoding in @([Text.Encoding]::UTF8, [Text.Encoding]::Unicode)) {
            if ($encoding.GetString($bytes) -match '(?i)[A-Z]:[\\/](?:Users|Documents and Settings)[\\/]|OneDrive[\\/]') {
                throw "Embedded personal/build path in payload: $relative (matched text is not logged)."
            }
        }
    }
    'No recognised personal build-path pattern in payload bytes; this is not a general privacy proof.'
}

Add-Gate 'language-readiness' {
    $script:languagePolicyHash = Get-Sha256 $languagePolicyPath
    $languages = Get-Content -LiteralPath $languagePolicyPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $required = @('en', 'zh', 'fr', 'ja', 'ko')
    if ($languages.format -isnot [int] -or $languages.format -ne 1 -or
            $languages.required_languages -isnot [array] -or $languages.languages -isnot [array] -or
            ($languages.required_languages -join ',') -cne ($required -join ',') -or
            @($languages.languages).Count -ne 5) { throw 'Language policy must require exactly en,zh,fr,ja,ko in priority order.' }
    foreach ($id in $required) {
        $rows = @($languages.languages | Where-Object { $_.id -ceq $id })
        if ($rows.Count -ne 1) { throw "Missing or duplicate language policy: $id" }
        $row = $rows[0]
        if ($row.status -cne 'implemented' -or $row.data_status -cne 'packaged') {
            throw "Language $id is not implemented with actual packaged data; core-only or absent data is blocked."
        }
        if ($null -eq $manifest -or $row.payload_paths -isnot [array] -or @($row.payload_paths).Count -eq 0 -or
                $row.source_paths -isnot [array] -or @($row.source_paths).Count -eq 0) {
            throw "Language $id lacks verified payload paths and source references."
        }
        foreach ($relative in $row.payload_paths) {
            Assert-RelativePath $relative
            if (-not $manifest.files.ContainsKey($relative) -or -not $policy.ContainsKey($relative) -or
                    -not $policy[$relative].source) { throw "Language $id data is not in the fixed product payload policy." }
        }
        foreach ($reference in $row.source_paths) { $null = Read-Reference $reference -CorrespondingSource }
    }
    'Five language implementations, exact source references and fixed data payloads present; executed acceptance is checked separately.'
}

# Tool discovery is read-only. No installer, signing utility, registry or host is invoked.
foreach ($name in @('wix.exe', 'candle.exe', 'light.exe', 'ISCC.exe', 'makensis.exe', 'msixmgr.exe', 'signtool.exe')) {
    $tools[$name] = [bool](Get-Command $name -CommandType Application -ErrorAction SilentlyContinue)
}
Add-Gate 'release-evidence-binding' {
    if (-not (Test-Path -LiteralPath $EvidencePath -PathType Leaf)) { throw 'release-evidence.json is missing; approvals and test runs are not inferred.' }
    if ($null -eq $source -or $null -eq $manifest -or $null -eq $receipt) { throw 'Evidence cannot be accepted without verified source/package/build identities.' }
    $documentHash = Get-Sha256 $EvidencePath
    $document = Get-Content -LiteralPath $EvidencePath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($document.format -isnot [int] -or $document.format -ne 2 -or $document.entries -isnot [array]) {
        throw 'Unsupported release evidence format or entries type.'
    }
    $bindings = @{ version = $version; commit = $source.commit; source_sha256 = $source.source_sha256;
        package_manifest_sha256 = $manifest.sha256; build_receipt_sha256 = $receiptHash; package_policy_sha256 = $policyHash;
        acceptance_profile = $AcceptanceProfile; distribution_policy = $manifest.distribution_policy;
        license_decision_sha256 = $licenseDecisionHash; language_policy_sha256 = $languagePolicyHash }
    if (-not $licenseDecisionHash -or -not $languagePolicyHash) { throw 'License/language policy identity unavailable.' }
    foreach ($key in $bindings.Keys) {
        if ($document.$key -isnot [string] -or $document.$key -cne $bindings[$key]) { throw "Release evidence identity mismatch: $key" }
    }
    $ids = @{}
    foreach ($entry in @($document.entries)) {
        if ($entry.id -isnot [string] -or -not $entry.id -or $ids.ContainsKey($entry.id) -or
                $entry.kind -isnot [string]) { throw 'Missing/duplicate evidence id or invalid evidence kind.' }
        $ids[$entry.id] = $true
    }
    if ((Get-Sha256 $EvidencePath) -cne $documentHash) { throw 'Release evidence changed while being read.' }
    $script:evidence = $document
    $script:evidenceHash = $documentHash
    'Evidence is hash/version-bound to this source, tested build, policy and exact package manifest.'
}
Add-Gate 'corresponding-source' {
    if (-not $sourceContractLoaded -or -not (Get-Command Get-ProductCorrespondingSourceFiles -ErrorAction SilentlyContinue)) {
        throw 'tools/product/source_contract.ps1 is missing; Corresponding Source cannot be accepted.'
    }
    if ($null -eq $evidence -or $null -eq $source -or $null -eq $manifest -or $null -eq $receipt) {
        throw 'Corresponding Source requires current evidence/source/package/build identities.'
    }
    $sourceManifestPath = Read-Reference $evidence.source_manifest
    if ($sourceManifestPath -cne (Join-Path $BuildDir 'source-manifest.json')) {
        throw 'Source manifest must be the producer output at BuildDir/source-manifest.json.'
    }
    $script:sourceManifestHash = Get-Sha256 $sourceManifestPath
    $document = Get-Content -LiteralPath $sourceManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($document.format -isnot [int] -or $document.format -ne 1 -or $document.files -isnot [array]) {
        throw 'Unsupported corresponding-source manifest.'
    }
    $bindings = @{ version = $version; commit = $source.commit; source_sha256 = $source.source_sha256;
        package_manifest_sha256 = $manifest.sha256; build_receipt_sha256 = $receiptHash; package_policy_sha256 = $policyHash }
    foreach ($key in $bindings.Keys) {
        if ($document.$key -isnot [string] -or $document.$key -cne $bindings[$key]) { throw "Corresponding-source identity mismatch: $key" }
    }
    $archivePath = Read-Reference $document.archive -CorrespondingSource
    if ($archivePath -cne (Join-Path $BuildDir 'corresponding-source.zip')) { throw 'Archive must be the producer output at BuildDir/corresponding-source.zip.' }
    $required = @(Get-ProductCorrespondingSourceFiles -RepoRoot $RepoRoot)
    $expected = @{}
    foreach ($relative in $required) {
        Assert-ProductSourceRelativePath $relative
        if ($expected.ContainsKey($relative)) { throw 'Source contract contains a duplicate path.' }
        $expected[$relative] = $relative
    }
    if ($expected.Count -eq 0 -or $document.files.Count -ne $expected.Count) { throw 'Corresponding-source file set is incomplete.' }
    $listed = @{}
    $sourceDirectories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $document.files) {
        Assert-ProductSourceRelativePath $entry.path
        if ($listed.ContainsKey($entry.path) -or -not $expected.ContainsKey($entry.path) -or
                $entry.path -cne $expected[$entry.path] -or $entry.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
                [string]$entry.bytes -notmatch '^\d+$') { throw 'Invalid, unrequired or duplicate corresponding-source entry.' }
        $path = Join-Path $RepoRoot $entry.path
        $actual = Get-ProductSourceFileRecord -RepoRoot $RepoRoot -RelativePath $entry.path -VerifiedDirectories $sourceDirectories
        if ($actual.bytes -ne [uint64]$entry.bytes -or $actual.sha256 -cne $entry.sha256) {
            throw 'Corresponding-source manifest differs from current source bytes.'
        }
        $listed[$entry.path] = $entry
        $script:references.Add([pscustomobject]@{ path = $path; sha256 = $entry.sha256; bytes = $entry.bytes })
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        if ($zip.Entries.Count -ne $listed.Count) { throw 'Source archive entry count differs from source manifest.' }
        $seen = @{}
        foreach ($entry in $zip.Entries) {
            $relative = $entry.FullName
            Assert-ProductSourceRelativePath $relative
            if ($seen.ContainsKey($relative) -or -not $listed.ContainsKey($relative) -or
                    $relative -cne $listed[$relative].path -or $entry.Length -ne [uint64]$listed[$relative].bytes -or
                    (($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000 -or
                    ($entry.ExternalAttributes -band 0x410) -ne 0) { throw 'Invalid, linked, extra or duplicate source archive entry.' }
            $stream = $entry.Open()
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $hash = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
            finally { $stream.Dispose(); $sha.Dispose() }
            if ($hash -cne $listed[$relative].sha256) { throw 'Source archive entry digest mismatch.' }
            $seen[$relative] = $true
        }
    } finally { $zip.Dispose() }
    "$($listed.Count) actual source archive entries match the complete contract, current source and exact tested package."
}
Add-Gate 'installer-artifact' {
    if ($null -eq $evidence -or $null -eq $evidence.PSObject.Properties['installer']) {
        throw 'No hash-bound distributable installer artifact or lifecycle evidence.'
    }
    $script:installer = Read-Reference $evidence.installer
    if ([IO.Path]::GetExtension($installer) -notin @('.exe', '.msi', '.msix', '.msixbundle')) { throw 'Unsupported installer artifact type.' }
    'Installer artifact hash and byte count verified; not installed by this gate.'
}
Add-Gate 'installer-toolchain' {
    $builder = $tools['wix.exe'] -or ($tools['candle.exe'] -and $tools['light.exe']) -or $tools['ISCC.exe'] -or $tools['makensis.exe']
    if (-not $builder) { throw 'No WiX/Inno Setup/NSIS builder found on PATH. Probe: Get-Command wix.exe,candle.exe,light.exe,ISCC.exe,makensis.exe,msixmgr.exe -CommandType Application. msixmgr alone is a deployment tool, not a builder.' }
    'An installer builder is available; lifecycle testing remains a separate mandatory gate.'
}

$requiredTests = [ordered]@{
    'installer-lifecycle' = @('install', 'upgrade', 'repair', 'uninstall', 'rollback', 'cleanup')
    'real-host' = @('registration', 'test-start', 'notepad', 'chromium', 'word', 'sensitive-fields', 'cleanup')
    'english-capabilities' = @('frequency', 'context-learning', 'morphology', 'spelling', 'segmentation', 'abbreviation', 'cross-app-consistency')
    'offline-privacy' = @('runtime-no-network', 'dependency-review', 'no-content-logging', 'sensitive-no-persistence')
    'language-acceptance' = @('en', 'zh', 'fr', 'ja', 'ko' | ForEach-Object {
        $language = $_
        'core', 'engine', 'tsf', 'installed-host' | ForEach-Object { "$language.$_" }
    })
    'signing' = @('EpoCanva.exe', 'epocanva_tsf.dll', 'rime.dll', 'installer', 'publisher', 'timestamp', 'trust')
}
$requiredIds = @('project-license', 'lexicon-redistribution', 'package-diagnose') + @($requiredTests.Keys)
foreach ($id in $requiredIds) {
    $category = if ($id -in @('project-license', 'lexicon-redistribution')) { 'distribution' }
        elseif ($id -eq 'signing') { 'signature' } else { 'technical' }
    if ($id -eq 'signing' -and $AcceptanceProfile -ceq 'internal-unsigned' -and
            ($null -eq $evidence -or @($evidence.entries | Where-Object { $_.id -ceq 'signing' }).Count -eq 0)) {
        $checks.Add([pscustomobject]@{ id = 'signing'; category = 'signature'; status = 'not_required';
            detail = 'Internal unsigned acceptance omits signing only; no signature has been verified.' })
        continue
    }
    Add-Gate $id {
        if ($null -eq $evidence) { throw "Missing current, hash-bound evidence: $id. Not executed/approved is blocked, never green." }
        $entries = @($evidence.entries | Where-Object { $_.id -ceq $id })
        if ($entries.Count -ne 1) { throw "Exactly one current evidence entry is required: $id" }
        $entry = $entries[0]
        if ($id -in @('project-license', 'lexicon-redistribution')) {
            if ($entry.kind -cne 'owner-record' -or $entry.decision -cne 'approved-public-distribution' -or
                    [string]::IsNullOrWhiteSpace($entry.owner) -or [string]::IsNullOrWhiteSpace($entry.record_id) -or
                    $entry.jurisdictions -isnot [array] -or @($entry.jurisdictions).Count -eq 0 -or
                    @($entry.jurisdictions | Where-Object { $_ -isnot [string] -or [string]::IsNullOrWhiteSpace($_) }).Count -ne 0) {
                throw 'An attributable owner/legal record with decision, scope and jurisdictions is required; a manual bool is not evidence.'
            }
            $record = Read-Reference $entry.record
            "Owner-supplied record hash verified: $id. Authenticity/authority remains the release owner's responsibility."
        } else {
            Assert-Execution $entry
            if ($id -eq 'package-diagnose') {
                if ($entry.kind -cne 'diagnose') { throw 'Native diagnosis evidence is required.' }
                $reportPath = Read-Reference $entry.report
                $userRoot = Join-Path $BuildDir 'diagnose-user'
                Assert-PlainPath $reportPath $BuildDir
                Assert-PlainPath $userRoot $BuildDir
                $expected = @((Join-Path $StageRoot 'EpoCanva.exe'), '--diagnose', $reportPath, '--user-data-root', $userRoot)
                if (@($entry.command).Count -ne $expected.Count) { throw 'Diagnosis must use the isolated build/<preset>/diagnose-user root.' }
                for ($argumentIndex = 0; $argumentIndex -lt $expected.Count; ++$argumentIndex) {
                    if ($entry.command[$argumentIndex] -cne $expected[$argumentIndex]) { throw 'Diagnosis command differs from the required isolated invocation.' }
                }
                $values = @{}
                foreach ($line in (Get-Content -LiteralPath $reportPath -Encoding UTF8)) {
                    if ($line -notmatch '^([^=]+)=(.*)$' -or $values.ContainsKey($Matches[1])) { throw 'Malformed or duplicate diagnosis field.' }
                    $values[$Matches[1]] = $Matches[2]
                }
                $expectedValues = @{ product = 'EpoCanva'; package = 'ready'; manifest = 'ok'; manifest_failures = '0';
                    manifest_files = [string]$policy.Count; version = $version; commit = $source.commit; user_data = 'ok'; user_root = $userRoot;
                    manifest_format = '2'; acceptance_profile = $AcceptanceProfile;
                    distribution_policy = $manifest.distribution_policy; path_exception = 'none';
                    readiness_scope = 'local-package-integrity-only' }
                foreach ($key in $expectedValues.Keys) {
                    if ($values[$key] -cne $expectedValues[$key]) { throw "Diagnosis disagrees with current package/isolation: $key" }
                }
                'Recorded headless diagnosis succeeded with an explicit isolated user-data root; registration status is not release evidence.'
            } else {
                if ($entry.kind -cne 'test-run') { throw 'An executed test-run with JUnit and a process log is required.' }
                if ($id -in @('installer-lifecycle', 'signing', 'real-host', 'language-acceptance') -and $null -eq $installer) {
                    throw 'Installer/signing/installed-host tests require the exact installer artifact.'
                }
                if ($id -eq 'language-acceptance' -and -not (Test-GatePassed 'language-readiness')) {
                    throw 'All five implemented languages and actual packaged data are required before aggregate language acceptance.'
                }
                $runnerPath = Read-Reference $entry.runner
                if ($entry.command[0] -cne $runnerPath) { throw 'Executed command does not identify the hashed test runner.' }
                $logPath = Read-Reference $entry.log
                $junitPath = Read-Reference $entry.junit
                $cases = Read-PassingJUnit $junitPath
                # Bind the runner's own report, not only an envelope that could be
                # accidentally repointed at an unrelated historical passing JUnit.
                $reportBindings = @{ version = $version; commit = $source.commit; source_sha256 = $source.source_sha256;
                    package_manifest_sha256 = $manifest.sha256; language_policy_sha256 = $languagePolicyHash;
                    acceptance_profile = $AcceptanceProfile }
                if ($id -in @('installer-lifecycle', 'signing', 'real-host', 'language-acceptance')) {
                    $reportBindings['installer_sha256'] = $evidence.installer.sha256
                }
                foreach ($key in $reportBindings.Keys) {
                    $properties = @($cases[0].OwnerDocument.SelectNodes("//properties/property[@name='epocanva.$key']"))
                    if ($properties.Count -ne 1 -or $properties[0].GetAttribute('value') -cne $reportBindings[$key]) {
                        throw "Test runner report lacks the current artifact binding: epocanva.$key"
                    }
                }
                foreach ($name in $requiredTests[$id]) {
                    $qualifiedName = $id + '.' + $name
                    if (@($cases | Where-Object { $_.GetAttribute('name') -ceq $qualifiedName }).Count -ne 1) {
                        throw "Missing/duplicate executed acceptance case: $qualifiedName"
                    }
                }
                "$($cases.Count) executed passing cases with hashed runner, JUnit and process log: $id."
            }
        }
    } $category
}
Add-Gate 'inputs-stable' {
    if ($null -eq $source) { throw 'Initial source snapshot is unavailable.' }
    $current = Get-ProductSourceIdentity -RepoRoot $RepoRoot
    if ($current.commit -cne $source.commit -or $current.source_sha256 -cne $source.source_sha256 -or
            ($receiptHash -and (Get-Sha256 $receiptPath) -cne $receiptHash) -or
            ($policyHash -and (Get-Sha256 $policyPath) -cne $policyHash) -or
            ($evidenceHash -and (Get-Sha256 $EvidencePath) -cne $evidenceHash) -or
            ($licenseDecisionHash -and (Get-Sha256 (Join-Path $RepoRoot 'data\manifests\license-decision.json')) -cne $licenseDecisionHash) -or
            ($languagePolicyHash -and (Get-Sha256 $languagePolicyPath) -cne $languagePolicyHash) -or
            ($sourceManifestHash -and (Get-Sha256 (Join-Path $BuildDir 'source-manifest.json')) -cne $sourceManifestHash) -or
            ($null -ne $manifest -and (Get-Sha256 (Join-Path $StageRoot 'package.manifest')) -cne $manifest.sha256)) {
        throw 'Source or release inputs changed during the check; rerun on an immutable candidate.'
    }
    if ($null -ne $receipt) {
        $null = Assert-ProductBuildReceipt -RepoRoot $RepoRoot -BuildDir $BuildDir -RimeDll $RimeDll
    }
    if ($null -ne $manifest) {
        foreach ($relative in $manifest.files.Keys) {
            $path = Join-Path $StageRoot $relative
            $file = Get-Item -LiteralPath $path -Force
            if ($file.Length -ne $manifest.files[$relative].bytes -or
                    (Get-Sha256 $path) -cne $manifest.files[$relative].sha256) { throw 'Payload bytes changed during the check.' }
        }
    }
    foreach ($reference in $references) {
        $file = Get-Item -LiteralPath $reference.path -Force
        if ($file.Length -ne $reference.bytes -or (Get-Sha256 $reference.path) -cne $reference.sha256) {
            throw 'An evidence or installer file changed during the check.'
        }
    }
    'Source, tested binaries, receipt, policy, evidence and package identities remained stable during this check.'
}

$blockers = @($checks | Where-Object { $_.status -eq 'blocked' })
$technicalReady = @($checks | Where-Object { $_.category -eq 'technical' -and $_.status -ne 'passed' }).Count -eq 0
$distributionAuthorized = @($checks | Where-Object { $_.category -eq 'distribution' -and $_.status -ne 'passed' }).Count -eq 0
$signatureVerified = Test-GatePassed 'signing'
$releaseReady = $technicalReady -and $distributionAuthorized -and $signatureVerified -and $AcceptanceProfile -ceq 'public-signed'
$acceptanceReady = if ($AcceptanceProfile -ceq 'internal-unsigned') { $technicalReady } else { $releaseReady }
$acceptanceBlockers = @($blockers | Where-Object { $AcceptanceProfile -ceq 'public-signed' -or $_.category -ceq 'technical' })
$exitCode = if ($acceptanceReady) { 0 } else { 1 }
$report = [ordered]@{
    format = 2; checked_utc = [DateTimeOffset]::UtcNow.ToString('o'); scope = 'windows-aggregate-acceptance'
    mode = 'read-only-files'; acceptance_profile = $AcceptanceProfile
    distribution_policy = $(if ($null -ne $manifest) { $manifest.distribution_policy } else { '' })
    acceptance_ready = $acceptanceReady; technical_ready = $technicalReady
    distribution_authorized = $distributionAuthorized; signature_verified = $signatureVerified
    signing_status = $(if ($signatureVerified) { 'verified' } elseif (@($checks | Where-Object { $_.id -ceq 'signing' -and $_.status -ceq 'not_required' }).Count -eq 1) { 'not_required' } else { 'blocked' })
    release_ready = $releaseReady; exit_code = $exitCode
    version = $version; source = $source; package_policy_sha256 = $policyHash
    package_manifest_sha256 = $(if ($null -ne $manifest) { $manifest.sha256 } else { '' })
    build_receipt_sha256 = $receiptHash; license_decision_sha256 = $licenseDecisionHash
    language_policy_sha256 = $languagePolicyHash; source_manifest_sha256 = $sourceManifestHash
    expected_payload_files = $policy.Count
    tools_on_path = $tools; checks = $checks.ToArray(); blockers = $blockers; acceptance_blockers = $acceptanceBlockers
}
try {
    Assert-PlainPath $outputPath $buildBase
    [IO.File]::WriteAllText($outputPath, ($report | ConvertTo-Json -Depth 12), (New-Object Text.UTF8Encoding($false)))
} catch {
    Write-Error -ErrorAction Continue ('Cannot write release-readiness.json: ' + $_.Exception.Message)
    exit 2
}
Write-Host "Release readiness: $($report.release_ready); blockers: $($blockers.Count)"
foreach ($blocker in $blockers) { Write-Host ("BLOCKED [{0}] {1}" -f $blocker.id, $blocker.detail) }
Write-Host "Report: $outputPath"
exit $exitCode
