# Isolated staging producer tests. All product/build inputs below are synthetic fixtures.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$RunRoot,
    [switch]$ForceFailure
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$RunRoot = [IO.Path]::GetFullPath($RunRoot).TrimEnd('\', '/')
if ((Split-Path -Parent $RunRoot) -ine (Join-Path $repo 'build') -or
        (Split-Path -Leaf $RunRoot) -cnotmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$' -or
        $RunRoot.EndsWith('.')) { throw 'RunRoot must be exactly <repo>/build/<preset>.' }
$fixtureRoot = Join-Path $RunRoot 'stage-fixtures'
$null = New-Item -ItemType Directory -Force -Path $fixtureRoot
$hostExe = (Get-Command powershell.exe -CommandType Application).Source
$utf8 = [Text.UTF8Encoding]::new($false)
$script:assertions = 0
$script:results = [Collections.Generic.List[object]]::new()

function Check([bool]$Condition, [string]$Message) {
    ++$script:assertions
    if (-not $Condition) { throw $Message }
}
function Write-Text([string]$Path, [string]$Text) {
    $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path)
    [IO.File]::WriteAllText($Path, $Text, $utf8)
}
function Write-Bytes([string]$Path, [byte[]]$Bytes) {
    $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path)
    [IO.File]::WriteAllBytes($Path, $Bytes)
}
function Get-Hash([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function New-Fixture([string]$Name) {
    $root = Join-Path $fixtureRoot $Name
    $product = Join-Path $root 'build/preset'
    $policy = Join-Path $root 'data/manifests/product-package-files.inc'
    $resourceSource = Join-Path $root 'data/backend/resources.tsv'
    $backend = Join-Path $root 'backend-resources'
    $scriptDir = Join-Path $root 'tools/product'
    $null = New-Item -ItemType Directory -Force -Path $scriptDir
    Copy-Item -LiteralPath (Join-Path $repo 'tools/product/stage_product.ps1') -Destination (Join-Path $scriptDir 'stage_product.ps1')
    Write-Text (Join-Path $scriptDir 'build_identity.ps1') @'
# SYNTHETIC TEST COLLABORATORS; never real product build evidence.
function Assert-ProductBuildReceipt {
    param([string]$RepoRoot, [string]$BuildDir, [string]$RimeDll)
    [pscustomobject]@{ source = [pscustomobject]@{ commit = ('1' * 40) }; engine_source_sha256 = ('c' * 64) }
}
function Assert-ProductEngineReceipt {
    param([string]$RepoRoot, [string]$RimeDll)
    [pscustomobject]@{ source_sha256 = ('c' * 64) }
}
'@
    Write-Text (Join-Path $root 'platforms/windows/product/VERSION') "1.2.3-unit`n"
    Write-Bytes (Join-Path $product 'product/bin/EpoCanva.exe') ([Text.Encoding]::ASCII.GetBytes('UNIT-EXE'))
    Write-Bytes (Join-Path $product 'product/bin/epocanva_tsf.dll') ([Text.Encoding]::ASCII.GetBytes('UNIT-TSF'))
    $rime = Join-Path $product 'rime.dll'
    Write-Bytes $rime ([Text.Encoding]::ASCII.GetBytes('UNIT-RIME'))
    Write-Bytes $resourceSource ([Text.Encoding]::ASCII.GetBytes("resource`n"))
    Write-Bytes (Join-Path $backend 'data/backend/resources.tsv') ([IO.File]::ReadAllBytes($resourceSource))
    Write-Bytes (Join-Path $backend 'data/backend/runtime.bin') ([Text.Encoding]::ASCII.GetBytes('runtime-extra'))
    $fixedHash = Get-Hash $resourceSource
    $fixedBytes = (Get-Item -LiteralPath $resourceSource).Length
    $rows = @(
        'EPOCANVA_PACKAGE_FILE("EpoCanva.exe", "", "", 0)',
        'EPOCANVA_PACKAGE_FILE("epocanva_tsf.dll", "", "", 0)',
        'EPOCANVA_PACKAGE_FILE("rime.dll", "", "", 0)',
        'EPOCANVA_PACKAGE_FILE("VERSION.txt", "", "", 0)',
        ('EPOCANVA_PACKAGE_FILE("data/backend/resources.tsv", "data/backend/resources.tsv", "{0}", {1})' -f $fixedHash, $fixedBytes)
    )
    Write-Text $policy (($rows -join "`n") + "`n")
    $manifest = [pscustomobject]@{ files = @(
        [pscustomobject]@{ path = 'data/backend/resources.tsv'; sha256 = $fixedHash; bytes = $fixedBytes },
        [pscustomobject]@{ path = 'data/backend/runtime.bin'; sha256 = (Get-Hash (Join-Path $backend 'data/backend/runtime.bin')); bytes = (Get-Item -LiteralPath (Join-Path $backend 'data/backend/runtime.bin')).Length }
    ) }
    Write-Text (Join-Path $backend 'data/backend/staging-manifest.json') ($manifest | ConvertTo-Json -Depth 8)
    return [pscustomobject]@{ name = $Name; root = $root; build = $product; backend = $backend; rime = $rime;
        stage = (Join-Path $product 'stage/EpoCanva-1.2.3-unit'); manifest = (Join-Path $backend 'data/backend/staging-manifest.json');
        resourceSource = $resourceSource }
}
function Save-Manifest($Fixture, $Manifest) {
    Write-Text $Fixture.manifest ($Manifest | ConvertTo-Json -Depth 8)
}
function Read-Manifest($Fixture) {
    Get-Content -LiteralPath $Fixture.manifest -Raw -Encoding UTF8 | ConvertFrom-Json
}
function Invoke-Stage($Fixture, [string[]]$Extra = @()) {
    $arguments = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File',
        (Join-Path $Fixture.root 'tools/product/stage_product.ps1'), '-RepoRoot', $Fixture.root,
        '-BuildDir', $Fixture.build, '-RimeDll', $Fixture.rime, '-BackendResourcesRoot', $Fixture.backend,
        '-StageRoot', $Fixture.stage) + $Extra
    $output = @(& $hostExe @arguments 2>&1 | ForEach-Object { $_.ToString() })
    $code = $LASTEXITCODE
    $script:results.Add([pscustomobject]@{ case = $Fixture.name; command = @($hostExe) + $arguments;
        exit_code = $code; output = ($output -join "`n") })
    return [pscustomobject]@{ exit_code = $code; output = ($output -join "`n") }
}
function Add-OldStageSentinel($Fixture) {
    Write-Text (Join-Path $Fixture.stage 'sentinel.txt') 'existing stage must remain intact'
}
function Assert-ConflictPreserved($Fixture, [string]$Name, [string]$ExpectedMessage) {
    Add-OldStageSentinel $Fixture
    $result = Invoke-Stage $Fixture @('-Replace')
    Check ($result.exit_code -eq 5) "$Name must exit 5; got $($result.exit_code): $($result.output)"
    Check ($result.output.Contains($ExpectedMessage)) "$Name must explain the conflict: $($result.output)"
    Check ((Get-Content -LiteralPath (Join-Path $Fixture.stage 'sentinel.txt') -Raw) -ceq
        'existing stage must remain intact') "$Name removed or altered the prior stage"
}

try {
    $valid = New-Fixture 'identical-overlap'
    $result = Invoke-Stage $valid
    Check ($result.exit_code -eq 0) "Byte-identical policy overlap was rejected: $($result.output)"
    $manifestLines = @(Get-Content -LiteralPath (Join-Path $valid.stage 'package.manifest') -Encoding UTF8)
    $fileRows = @($manifestLines | Where-Object { $_.StartsWith("file`t", [StringComparison]::Ordinal) })
    Check ($fileRows.Count -eq 6) 'Fixed overlap was not staged exactly once with the full backend payload.'
    Check (@($fileRows | Where-Object { $_ -match '^file\tdata/backend/resources\.tsv\t' }).Count -eq 1) 'Fixed overlapping path appears more than once in the package manifest.'
    Check (([Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $valid.stage 'data/backend/resources.tsv'))) -ceq
        [Convert]::ToBase64String([IO.File]::ReadAllBytes($valid.resourceSource)))) 'Fixed overlap staged bytes differ from the fixed-policy source.'
    Check (Test-Path -LiteralPath (Join-Path $valid.stage 'data/backend/runtime.bin') -PathType Leaf) 'Non-overlapping backend file was not staged.'

    $allFixed = New-Fixture 'all-fixed-overlap'
    $policyLines = @(Get-Content -LiteralPath (Join-Path $allFixed.root 'data/manifests/product-package-files.inc') -Encoding UTF8)
    $runtimePath = 'data/backend/runtime.bin'
    $runtimeSource = Join-Path $allFixed.backend $runtimePath
    $runtimeHash = Get-Hash $runtimeSource
    $runtimeBytes = (Get-Item -LiteralPath $runtimeSource).Length
    Write-Bytes (Join-Path $allFixed.root $runtimePath) ([IO.File]::ReadAllBytes($runtimeSource))
    $policyLines += ('EPOCANVA_PACKAGE_FILE("{0}", "{0}", "{1}", {2})' -f $runtimePath, $runtimeHash, $runtimeBytes)
    Write-Text (Join-Path $allFixed.root 'data/manifests/product-package-files.inc') (($policyLines -join "`n") + "`n")
    $result = Invoke-Stage $allFixed
    Check ($result.exit_code -eq 0) "All-fixed backend overlap was rejected: $($result.output)"
    $allFixedRows = @(Get-Content -LiteralPath (Join-Path $allFixed.stage 'package.manifest') -Encoding UTF8 |
        Where-Object { $_.StartsWith("file`t", [StringComparison]::Ordinal) })
    Check ($allFixedRows.Count -eq 6) 'An all-fixed backend set was not staged exactly once from policy.'
    Check (@($allFixedRows | Where-Object { $_ -match '^file\tdata/backend/runtime\.bin\t' }).Count -eq 1) 'All-fixed backend overlap appears more than once.'

    $badHash = New-Fixture 'overlap-hash-mismatch'
    $manifest = Read-Manifest $badHash
    $manifest.files[0].sha256 = '0' * 64
    Save-Manifest $badHash $manifest
    Assert-ConflictPreserved $badHash 'overlap hash mismatch' 'differs from fixed product policy'

    $badBytes = New-Fixture 'overlap-byte-count-mismatch'
    $manifest = Read-Manifest $badBytes
    $manifest.files[0].bytes = [long]$manifest.files[0].bytes + 1
    Save-Manifest $badBytes $manifest
    Assert-ConflictPreserved $badBytes 'overlap byte-count mismatch' 'differs from fixed product policy'

    $badBytesType = New-Fixture 'overlap-byte-count-type'
    $manifest = Read-Manifest $badBytesType
    $manifest.files[0].bytes = 'not-an-integer'
    Save-Manifest $badBytesType $manifest
    Add-OldStageSentinel $badBytesType
    $result = Invoke-Stage $badBytesType @('-Replace')
    Check ($result.exit_code -eq 4) "Non-integer backend byte count must exit 4: $($result.output)"
    Check ($result.output.Contains('invalid backend payload digest or byte count')) 'Non-integer backend byte count was not rejected as malformed.'
    Check ((Get-Content -LiteralPath (Join-Path $badBytesType.stage 'sentinel.txt') -Raw) -ceq
        'existing stage must remain intact') 'Malformed backend byte count removed or altered the prior stage.'

    $badSource = New-Fixture 'overlap-source-mismatch'
    Write-Bytes (Join-Path $badSource.backend 'data/backend/resources.tsv') ([Text.Encoding]::ASCII.GetBytes('different bytes'))
    Assert-ConflictPreserved $badSource 'overlap source mismatch' 'required content differs from product policy'

    $duplicate = New-Fixture 'duplicate-backend-manifest-path'
    $manifest = Read-Manifest $duplicate
    $manifest.files = @($manifest.files) + $manifest.files[0]
    Save-Manifest $duplicate $manifest
    Add-OldStageSentinel $duplicate
    $result = Invoke-Stage $duplicate @('-Replace')
    Check ($result.exit_code -eq 4) "Duplicate backend manifest path must exit 4: $($result.output)"
    Check ($result.output.Contains('repeats a payload path')) 'Duplicate backend path did not identify the malformed manifest.'
    Check ((Get-Content -LiteralPath (Join-Path $duplicate.stage 'sentinel.txt') -Raw) -ceq
        'existing stage must remain intact') 'Duplicate backend path removed or altered the prior stage.'

    if ($ForceFailure) { throw 'Injected product-stage test assertion failure.' }
    Write-Host "RESULT PASS product staging tests ($($script:results.Count) child executions, $script:assertions assertions; synthetic fixtures only)."
    exit 0
} catch {
    Write-Host ('RESULT FAIL ' + $_.Exception.Message)
    Write-Host "Product-stage child executions: $($script:results.Count); assertions: $script:assertions"
    exit 14
}
