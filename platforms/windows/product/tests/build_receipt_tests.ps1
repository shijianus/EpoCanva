[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$RunRoot,
    [switch]$ForceFailure,
    [switch]$InventoryOnly,
    [string]$RegisteredInventoryPath = '',
    [string]$RegisteredBuildDir = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
. (Join-Path $repo 'tools\product\build_identity.ps1')
. (Join-Path $repo 'tools\product\test_inventory_contract.ps1')
$RunRoot = [System.IO.Path]::GetFullPath($RunRoot)
$buildBase = (Join-Path $repo 'build') + [System.IO.Path]::DirectorySeparatorChar
if (-not $RunRoot.StartsWith($buildBase, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'RunRoot must be inside build/.'
}
New-Item -ItemType Directory -Force -Path (Join-Path $RunRoot 'product\bin') | Out-Null

function Expand-InventoryContractValue($Value, $Roots) {
    if ($Value -is [string]) {
        $expanded = $Value.Replace('{REPO}', $Roots.repo).Replace('{BUILD}', $Roots.build).
            Replace('{POLICY_BUILD_TAG}', $Roots.policy_tag)
        foreach ($tool in @('cmake.exe', 'powershell.exe', 'python.exe')) {
            # These paths are data only and are never executed. Use an external
            # root to exercise the documented tool-basename normalization.
            $expanded = $expanded.Replace('{TOOL:' + $tool + '}',
                ($env:SystemRoot.Replace('\', '/') + '/inventory-contract-tools/' + $tool))
        }
        return $expanded
    }
    if ($Value -is [array]) { return ,@($Value | ForEach-Object { Expand-InventoryContractValue $_ $Roots }) }
    return $Value
}

function Test-RealRegisteredInventoryContract {
    $contract = Get-ProductRegisteredTestContract $repo
    $contractPath = Join-Path $repo 'data/manifests/product-test-inventory.json'
    $contractDigest = Get-ProductIdentityFileSha256 $contractPath
    $registrationBuild = $RunRoot
    if ($RegisteredInventoryPath) {
        if (-not $RegisteredBuildDir) { throw 'RegisteredBuildDir is required with an actual inventory.' }
        $registrationBuild = $RegisteredBuildDir
        $actual = Read-ProductTestInventory $RegisteredInventoryPath
    } else {
        # Ordinary receipt CTest is self-contained. The dedicated audit also
        # passes the independently generated real CTest JSON through the same
        # tests; no CTest/CMake file or evidence-selected program is executed.
        $roots = Get-ProductInventoryRoots $repo $registrationBuild
        $rows = @($contract.tests | ForEach-Object {
            $row = $_
            [pscustomobject]@{ name = $row.name; config = 'Release'
                command = (Expand-InventoryContractValue $row.command $roots)
                properties = @($row.properties.PSObject.Properties | ForEach-Object {
                    [pscustomobject]@{ name = $_.Name; value = (Expand-InventoryContractValue $_.Value $roots) }
                }) }
        })
        $actual = [pscustomobject]@{ kind = 'ctestInfo'; version = [pscustomobject]@{ major = 1; minor = 0 }; tests = $rows }
    }
    Assert-ProductRegisteredTestContract $repo $registrationBuild $actual
    $script:inventoryAssertions = 1
    Write-Host "PASS actual/source-template registered inventory ($($actual.tests.Count) exact registrations)"
    function New-InventoryMutation { return Copy-ProductInventoryValue $actual }
    function Reject-InventoryMutation([string]$Name, $Changed, [string]$Reason) {
        $rejected = $false
        try { Assert-ProductRegisteredTestContract $repo $registrationBuild $Changed }
        catch {
            if ($_.Exception.Message -notlike "*$Reason*") { throw }
            $rejected = $true
        }
        if (-not $rejected) { throw "Registered inventory mutation was accepted: $Name" }
        $script:inventoryAssertions++
        Write-Host "PASS reject registered inventory $Name"
    }
    $focusName = 'windows.tsf_context_observer.real_focus'
    $focus = @($actual.tests | Where-Object { $_.name -ceq $focusName })
    if ($focus.Count -ne 1 -or $focus[0].command[-1] -cne 'real-focus') {
        throw 'The real-focus regression vector is absent from the actual source contract.'
    }
    $changed = New-InventoryMutation
    $changed.tests = @($changed.tests | Where-Object { $_.name -cne $focusName })
    $pairedInventory = Join-Path $RunRoot 'paired-removal-inventory.json'
    $pairedJUnit = Join-Path $RunRoot 'paired-removal-junit.xml'
    Write-ProductJson $pairedInventory $changed
    $xml = '<testsuite tests="' + $changed.tests.Count + '" failures="0" errors="0" skipped="0">' +
        (($changed.tests | ForEach-Object { '<testcase name="' + [Security.SecurityElement]::Escape($_.name) + '" status="run"/>' }) -join '') + '</testsuite>'
    [IO.File]::WriteAllText($pairedJUnit, $xml)
    $paired = Read-ProductTestInventory $pairedInventory
    $pairedCases = Read-ProductPassingJUnit $pairedJUnit
    Assert-ProductTestInventory $paired $pairedCases
    Write-ProductJson (Join-Path $RunRoot 'paired-removal-rehashed.json') ([ordered]@{
        tests = @{ total = $changed.tests.Count; passed = $changed.tests.Count; failed = 0; skipped = 0 }
        ctest_list_sha256 = Get-ProductIdentityFileSha256 $pairedInventory
        ctest_xml_sha256 = Get-ProductIdentityFileSha256 $pairedJUnit
        mutually_consistent = $true
    })
    Reject-InventoryMutation 'simultaneous inventory/JUnit removal with recomputed counts and hashes' $paired 'Source-controlled registered tests are missing'
    $changed = New-InventoryMutation
    ($changed.tests | Where-Object { $_.name -ceq $focusName }).name = 'windows.tsf_context_observer.replacement'
    Reject-InventoryMutation 'same-count test substitution' $changed 'Unknown or duplicate'
    $changed = New-InventoryMutation; $changed.tests += $changed.tests[0]
    Reject-InventoryMutation 'duplicate name' $changed 'Unknown or duplicate'
    $changed = New-InventoryMutation; $changed.tests[0].name = $changed.tests[0].name.ToUpperInvariant()
    Reject-InventoryMutation 'case-aliased name' $changed 'Unknown or duplicate'
    foreach ($mutation in @('executable', 'argument', 'extra-argument', 'missing-argument', 'typed-argument', 'placeholder', 'configuration')) {
        $changed = New-InventoryMutation
        $target = $changed.tests | Where-Object { $_.name -ceq $focusName }
        switch ($mutation) {
            'executable' { $target.command[0] += '.substituted.exe' }
            'argument' { $target.command[-1] = 'contract' }
            'extra-argument' { $target.command += '--skip-focus' }
            'missing-argument' { $target.command = @($target.command[0]) }
            'typed-argument' { $target.command[-1] = 42 }
            'placeholder' { $target.command[0] = '{BUILD}/tsf_context_observer_tests/Release/tsf_context_observer_tests.exe' }
            'configuration' { $target.config = 'Debug' }
        }
        $reason = if ($mutation -ceq 'placeholder') { 'reserved contract placeholder' } else { 'Registered test' }
        Reject-InventoryMutation $mutation $changed $reason
    }
    $changed = New-InventoryMutation
    ($changed.tests | Where-Object { $_.name -ceq $focusName }).properties += [pscustomobject]@{ name = 'DISABLED'; value = $true }
    Reject-InventoryMutation 'disabled test property' $changed 'Unknown or duplicate registered test property'
    $changed = New-InventoryMutation
    $target = $changed.tests | Where-Object { $_.name -ceq $focusName }
    $target.properties = @($target.properties | Where-Object { $_.name -cne 'RUN_SERIAL' })
    Reject-InventoryMutation 'missing focus serialization' $changed 'Missing registered test property'
    $changed = New-InventoryMutation
    $target = $changed.tests | Where-Object { $_.name -ceq $focusName }
    ($target.properties | Where-Object { $_.name -ceq 'WORKING_DIRECTORY' }).value = $registrationBuild
    Reject-InventoryMutation 'changed working directory' $changed 'property mismatch'
    $changed = New-InventoryMutation
    $target = $changed.tests | Where-Object { $_.name -ceq $focusName }
    ($target.properties | Where-Object { $_.name -ceq 'TIMEOUT' }).value = $true
    Reject-InventoryMutation 'boolean timeout' $changed 'property number mismatch'
    $changed = New-InventoryMutation
    $policy = $changed.tests | Where-Object { $_.name -ceq 'product.policy.release_gate_normal' }
    $policy.command = @($policy.command | ForEach-Object { $_ -replace 'policy-[0-9a-f]{12}-', 'policy-000000000000-' })
    Reject-InventoryMutation 'foreign policy build digest' $changed 'command/argument mismatch'
    $changed = New-InventoryMutation; $changed.version.major = '1'
    Reject-InventoryMutation 'string schema version' $changed 'Unsupported registered CTest inventory schema'
    # Ordering is not identity: an otherwise identical complete list remains valid.
    $changed = New-InventoryMutation; [Array]::Reverse($changed.tests)
    Assert-ProductRegisteredTestContract $repo $registrationBuild $changed
    $script:inventoryAssertions++
    Write-Host 'PASS complete registrations in a different order'
    if ((Get-ProductIdentityFileSha256 $contractPath) -cne $contractDigest) {
        throw 'Inventory unit tests modified the real source-controlled manifest.'
    }
    Write-Host "PASS registered inventory assertions=$script:inventoryAssertions; source manifest unchanged"
}

try {
    Test-RealRegisteredInventoryContract
    if ($InventoryOnly) {
        if ($ForceFailure) { throw 'injected inventory test failure' }
        Write-Host 'RESULT PASS registered inventory contract mutations'
        exit 0
    }
} catch { Write-Host "RESULT FAIL $($_.Exception.Message)"; exit 14 }

# Receipt unit tests use an explicit small-contract collaborator, not a mutation
# of the actual source manifest. The real contract/mutation tests above always
# run first. The receipt consumer still invokes the public contract validator.
$script:receiptInventoryContract = Get-ProductRegisteredTestContract $repo
function Get-ProductRegisteredTestContract([string]$RepoRoot) { return $script:receiptInventoryContract }
$paths = @{
    exe = (Join-Path $RunRoot 'product\bin\EpoCanva.exe')
    tsf = (Join-Path $RunRoot 'product\bin\epocanva_tsf.dll')
    rime = (Join-Path $RunRoot 'rime.dll')
}
foreach ($key in $paths.Keys) { [System.IO.File]::WriteAllText($paths[$key], "fixture-$key") }
$identity = Get-ProductSourceIdentity -RepoRoot $repo
$receiptPath = Join-Path $RunRoot 'product-build-receipt.json'
$cachePath = Join-Path $RunRoot 'CMakeCache.txt'
$passingCache = "EPOCANVA_PRODUCT_TRACE:BOOL=OFF`nEPOCANVA_PRODUCT_BUILD_ID:STRING=$($identity.commit)`n"
[System.IO.File]::WriteAllText($cachePath, $passingCache)
$evidenceRoot = Join-Path $RunRoot 'synthetic-verification'
[IO.Directory]::CreateDirectory($evidenceRoot) | Out-Null
$junitPath = Join-Path $evidenceRoot 'ctest.xml'
$inventoryPath = Join-Path $evidenceRoot 'ctest-list.json'
$commandsPath = Join-Path $evidenceRoot 'commands.json'
$testExecutable = Join-Path $RunRoot 'synthetic-test.exe'
[IO.File]::WriteAllText($testExecutable, 'synthetic test executable bytes')
$names = @('windows.a', 'product.b', 'engine_abi.c', 'english.core.d', 'learning.core.e',
    'learning.repository.f', 'chinese.core.g', 'french.core.h', 'composition.core.i', 'language_packages.j',
    'windows.tsf_context_observer.real_focus')
$suite = '<testsuite tests="' + $names.Count + '" failures="0" errors="0" skipped="0">'
$passingJUnit = $suite + (($names | ForEach-Object { '<testcase name="' + $_ + '" status="run"/>' }) -join '') + '</testsuite>'
$inventory = @{ kind = 'ctestInfo'; version = @{ major = 1; minor = 0 }; tests = @($names | ForEach-Object {
    @{ name = $_; command = @($testExecutable); config = 'Release'; properties = @() }
}) }
$script:receiptInventoryContract.tests = @($names | ForEach-Object {
    [pscustomobject]@{ name = $_; command = @('{BUILD}/synthetic-test.exe'); properties = [pscustomobject]@{}
        defined_in = 'platforms/windows/product/CMakeLists.txt'; line = 1 }
})
$passingInventory = ConvertTo-Json -InputObject $inventory -Depth 8
function Reset-ExecutionEvidence {
    $now = [DateTimeOffset]::UtcNow.ToString('o')
    [IO.File]::WriteAllText($junitPath, $passingJUnit)
    [IO.File]::WriteAllText($inventoryPath, $passingInventory)
    $tests = @('--test-dir', $RunRoot, '-C', 'Release')
    $commands = @('configure', 'build', 'inventory', 'ctest') | ForEach-Object {
        $tool = if ($_ -cin @('configure', 'build')) { 'cmake.exe' } else { 'ctest.exe' }
        $arguments = switch ($_) {
            'inventory' { $tests + '--show-only=json-v1' }
            'ctest' { $tests + @('--output-on-failure', '--no-tests=error', '--output-junit', $junitPath) }
            default { $RunRoot }
        }
        @{ stage = $_; executable = (Join-Path $RunRoot $tool); arguments = @($arguments);
            exit_code = 0; started_utc = $now; completed_utc = [DateTimeOffset]::UtcNow.ToString('o') }
    }
    $commands = @($commands)
    for ($stageIndex = 1; $stageIndex -lt $commands.Count; $stageIndex++) {
        $commands[$stageIndex].started_utc = $commands[$stageIndex - 1].completed_utc
    }
    [IO.File]::WriteAllText($commandsPath, (ConvertTo-Json -InputObject @($commands) -Depth 10))
}
Reset-ExecutionEvidence
function New-Receipt {
    return [pscustomobject]@{
        format = 2; status = 'verified'; configuration = 'Release'; trace = $false
        source = ($identity | ConvertTo-Json | ConvertFrom-ProductJson)
        build_exit = 0; ctest_exit = 0
        engine_source_sha256 = ('1' * 64); engine_receipt_sha256 = ('2' * 64)
        evidence = $evidenceRoot
        ctest_xml_sha256 = Get-ProductIdentityFileSha256 -Path $junitPath
        ctest_list_sha256 = Get-ProductIdentityFileSha256 -Path $inventoryPath
        commands_sha256 = Get-ProductIdentityFileSha256 -Path $commandsPath
        cache_sha256 = Get-ProductIdentityFileSha256 -Path $cachePath
        tests = @{ total = $names.Count; passed = $names.Count; skipped = 0; failed = 0; skipped_names = @() }
        test_binaries = @(New-ProductBinaryRecord 'synthetic-test.exe' $testExecutable)
        binaries = @($paths.Keys | ForEach-Object {
            [pscustomobject]@{ name = $_; path = $paths[$_]; bytes = (Get-Item -LiteralPath $paths[$_]).Length
                sha256 = Get-ProductIdentityFileSha256 -Path $paths[$_] }
        })
    }
}
function Save-Receipt($Receipt) {
    [System.IO.File]::WriteAllText($receiptPath, ($Receipt | ConvertTo-Json -Depth 10))
}
function Assert-Rejected([string]$Name, [string]$Expected = '') {
    $rejected = $false
    try { $null = Assert-ProductBuildReceipt -RepoRoot $repo -BuildDir $RunRoot -RimeDll $paths.rime }
    catch {
        if ($Expected -and $_.Exception.Message -notlike "*$Expected*") { throw }
        $rejected = $true
    }
    if (-not $rejected) { throw "$Name was accepted" }
    Write-Host "PASS reject $Name"
}
function Checkpoint-IdentityFailures {
    $vectorPath = Join-Path $RunRoot 'identity-hash-unit.txt'
    [IO.File]::WriteAllBytes($vectorPath, [Text.Encoding]::ASCII.GetBytes('abc'))
    if ((Get-ProductIdentityFileSha256 -Path $vectorPath) -cne
            'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad') {
        throw 'SHA-256 known-answer vector failed'
    }
    $locked = [IO.File]::Open($vectorPath, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $rejected = $false
        try { $null = Get-ProductIdentityFileSha256 -Path $vectorPath } catch { $rejected = $true }
        if (-not $rejected) { throw 'An unreadable input produced a digest' }
    } finally { $locked.Dispose() }
    $rejected = $false
    try { $null = Get-ProductIdentityFileSha256 -Path (Join-Path $RunRoot 'does-not-exist') } catch { $rejected = $true }
    if (-not $rejected) { throw 'Missing file produced a digest' }
    $original = (Get-Command Get-ProductIdentityFileSha256 -CommandType Function).ScriptBlock
    $previous = $ErrorActionPreference
    try {
        foreach ($failure in @({ Write-Error 'unit synthetic hash read failure' }, { return '' })) {
            Set-Item -Path Function:script:Get-ProductIdentityFileSha256 -Value $failure
            $ErrorActionPreference = 'Continue'
            foreach ($getter in @('Get-ProductSourceIdentity', 'Get-ProductEngineIdentity')) {
                $rejected = $false
                try { $null = & $getter -RepoRoot $repo 2>$null } catch { $rejected = $true }
                if (-not $rejected) { throw "$getter manufactured an identity after hash failure" }
            }
        }
    } finally {
        $ErrorActionPreference = $previous
        Set-Item -Path Function:script:Get-ProductIdentityFileSha256 -Value $original
    }
    Write-Host 'PASS unreadable/failed/empty hashes reject source and engine identities even for a Continue caller'
}
try {
    foreach ($wire in @('[]', '[{"id":1}]', '[{"id":1},{"id":2}]')) {
        $roundTripPath = Join-Path $RunRoot 'json-array-roundtrip.json'
        $parsed = ConvertFrom-ProductJson $wire
        if ($parsed -isnot [array]) { throw 'JSON parser changed the root array type' }
        $expectedCount = $parsed.Count
        Write-ProductJson $roundTripPath $parsed
        $readBack = Read-ProductJson $roundTripPath
        if ($readBack -isnot [array] -or $readBack.Count -ne $expectedCount -or
                -not ([IO.File]::ReadAllText($roundTripPath).TrimStart().StartsWith('['))) {
            throw 'JSON array round trip changed the wire type or element count'
        }
    }
    Write-Host 'PASS empty, singleton and multi-record JSON arrays retain their wire type'
    Save-Receipt (New-Receipt)
    $null = Assert-ProductBuildReceipt -RepoRoot $repo -BuildDir $RunRoot -RimeDll $paths.rime
    Write-Host 'PASS current source and binary receipt'
    Checkpoint-IdentityFailures
    $bad = New-Receipt; $bad.status = 'in_progress'; Save-Receipt $bad; Assert-Rejected 'unfinished build'
    foreach ($value in @('2', $true, 2.5)) {
        $bad = New-Receipt; $bad.format = $value; Save-Receipt $bad; Assert-Rejected 'untyped receipt schema'
    }
    $bad = New-Receipt; $bad.format = 1; Save-Receipt $bad; Assert-Rejected 'legacy receipt without execution contract'
    foreach ($value in @('0', $false, 0.5)) {
        $bad = New-Receipt; $bad.build_exit = $value; Save-Receipt $bad; Assert-Rejected 'untyped build exit code'
    }
    $bad = New-Receipt; $bad.PSObject.Properties.Remove('ctest_exit'); Save-Receipt $bad; Assert-Rejected 'missing real exit code'
    $bad = New-Receipt; $bad.trace = 'false'; Save-Receipt $bad; Assert-Rejected 'untyped trace flag'
    $bad = New-Receipt; $bad.build_exit = 1; Save-Receipt $bad; Assert-Rejected 'failed build'
    $bad = New-Receipt; $bad.ctest_exit = 8; Save-Receipt $bad; Assert-Rejected 'failed tests'
    $bad = New-Receipt; $bad.trace = $true; Save-Receipt $bad; Assert-Rejected 'trace binary'
    $bad = New-Receipt; $bad.source.source_sha256 = ('0' * 64); Save-Receipt $bad; Assert-Rejected 'stale source'
    $bad = New-Receipt; $bad.source.commit = 'incorrect'; Save-Receipt $bad; Assert-Rejected 'wrong commit'
    $bad = New-Receipt; $bad.tests.total = 0; Save-Receipt $bad; Assert-Rejected 'empty test suite'
    $bad = New-Receipt; $bad.tests.passed = 1; $bad.tests.skipped = 1; Save-Receipt $bad; Assert-Rejected 'skipped test suite'
    $bad = New-Receipt; $bad.tests.passed = 1; Save-Receipt $bad; Assert-Rejected 'inconsistent executed count'
    foreach ($value in @($true, '2', 2.5)) {
        $bad = New-Receipt; $bad.tests.total = $value; Save-Receipt $bad; Assert-Rejected 'nontyped integer test count'
    }
    # ConvertTo-Json normalizes an integral Double to an integer token. Write
    # the literal float token to exercise the actual parser boundary as well.
    Save-Receipt (New-Receipt)
    $floatToken = (Get-Content -LiteralPath $receiptPath -Raw) -replace ('"total"\s*:\s*' + $names.Count), ('"total": ' + $names.Count + '.0')
    [IO.File]::WriteAllText($receiptPath, $floatToken)
    Assert-Rejected 'floating JSON token test count'
    Save-Receipt (New-Receipt)
    $duplicateMember = (Get-Content -LiteralPath $receiptPath -Raw) -replace '"format"\s*:\s*2', '"format": 1, "format": 2'
    [IO.File]::WriteAllText($receiptPath, $duplicateMember)
    Assert-Rejected 'duplicate JSON schema members' 'Duplicate or case-aliased'
    Save-Receipt (New-Receipt)
    $alias = (Get-Content -LiteralPath $receiptPath -Raw) -replace '"format"\s*:\s*2', '"Format": 2, "for\u006dat": 2'
    [IO.File]::WriteAllText($receiptPath, $alias)
    Assert-Rejected 'escaped case alias' 'Duplicate or case-aliased'
    Save-Receipt (New-Receipt)
    $nonfiniteToken = (Get-Content -LiteralPath $receiptPath -Raw) -replace '"build_exit"\s*:\s*0', '"build_exit": NaN'
    [IO.File]::WriteAllText($receiptPath, $nonfiniteToken)
    Assert-Rejected 'nonfinite JSON process exit token'
    Save-Receipt (New-Receipt)
    $overflow = (Get-Content -LiteralPath $receiptPath -Raw) -replace '"build_exit"\s*:\s*0', '"build_exit": 1e9000'
    [IO.File]::WriteAllText($receiptPath, $overflow)
    Assert-Rejected 'overflowing JSON number' 'outside the finite range'
    foreach ($text in @(
            ($passingJUnit -replace '<testcase name="product.b" status="run"/>', '<testcase name="product.b"><failure/></testcase>'),
            ($passingJUnit -replace '<testcase name="product.b" status="run"/>', '<testcase name="product.b"><skipped/></testcase>'),
            ($passingJUnit -replace ('tests="' + $names.Count + '"'), ('tests="' + ($names.Count + 1) + '"')),
            ($passingJUnit -replace 'product.b', 'windows.a'),
            ('<!DOCTYPE testsuite>' + $passingJUnit),
            ($passingJUnit -replace 'status="run"', 'status="notrun"'))) {
        [IO.File]::WriteAllText($junitPath, $text)
        Save-Receipt (New-Receipt) # Rehashing a mutated outer JSON must not bless bad JUnit.
        Assert-Rejected 'semantically invalid rehashed JUnit'
    }
    Reset-ExecutionEvidence
    Save-Receipt (New-Receipt)
    [IO.File]::WriteAllText($junitPath, $passingJUnit + ' ')
    Assert-Rejected 'tampered original JUnit'
    Reset-ExecutionEvidence
    [IO.File]::WriteAllText($inventoryPath, ($passingInventory -replace 'product.b', 'product.c'))
    Save-Receipt (New-Receipt); Assert-Rejected 'rehashed different registered inventory'
    Reset-ExecutionEvidence
    $removed = ConvertFrom-ProductJson $passingInventory
    $removed.tests = @($removed.tests | Where-Object { $_.name -cne 'windows.tsf_context_observer.real_focus' })
    Write-ProductJson $inventoryPath $removed
    $reducedJUnit = $passingJUnit.Replace('<testcase name="windows.tsf_context_observer.real_focus" status="run"/>', '').
        Replace(('tests="' + $names.Count + '"'), ('tests="' + ($names.Count - 1) + '"'))
    [IO.File]::WriteAllText($junitPath, $reducedJUnit)
    $bad = New-Receipt; $bad.tests.total--; $bad.tests.passed--; Save-Receipt $bad
    Assert-Rejected 'joint inventory/JUnit omission after rehash' 'Source-controlled registered tests are missing'
    Reset-ExecutionEvidence
    $changedCommand = ConvertFrom-ProductJson $passingInventory
    $changedCommand.tests[0].command += '--quietly-skip'
    Write-ProductJson $inventoryPath $changedCommand
    Save-Receipt (New-Receipt)
    Assert-Rejected 'rehashed altered registered command' 'Registered test configuration/command shape mismatch'
    Reset-ExecutionEvidence
    foreach ($field in @('exit_code', 'started_utc', 'completed_utc')) {
        $records = Get-Content -LiteralPath $commandsPath -Raw | ConvertFrom-ProductJson
        $records[3].PSObject.Properties.Remove($field)
        [IO.File]::WriteAllText($commandsPath, (ConvertTo-Json -InputObject @($records) -Depth 10))
        Save-Receipt (New-Receipt); Assert-Rejected "completed process record missing $field"
        Reset-ExecutionEvidence
    }
    $records = Get-Content -LiteralPath $commandsPath -Raw | ConvertFrom-ProductJson
    $records[3].exit_code = 8
    [IO.File]::WriteAllText($commandsPath, (ConvertTo-Json -InputObject @($records) -Depth 10))
    Save-Receipt (New-Receipt); Assert-Rejected 'rehashed failed CTest command'
    Reset-ExecutionEvidence
    $bad = New-Receipt; $bad.evidence = Split-Path $RunRoot; Save-Receipt $bad; Assert-Rejected 'evidence boundary escape'
    [IO.File]::SetLastWriteTimeUtc($junitPath, [DateTime]::UtcNow.AddDays(-1))
    Save-Receipt (New-Receipt); Assert-Rejected 'reused old JUnit bytes' 'outside its recorded invocation'
    Reset-ExecutionEvidence
    $records = Read-ProductJson $commandsPath
    $records[3].arguments += @('-R', 'product.b')
    Write-ProductJson $commandsPath $records
    Save-Receipt (New-Receipt); Assert-Rejected 'CTest command filter' 'unfiltered run'
    Reset-ExecutionEvidence
    Save-Receipt (New-Receipt)
    $null = Assert-ProductBuildReceipt -RepoRoot $repo -BuildDir $RunRoot -RimeDll $paths.rime
    [IO.File]::WriteAllText($testExecutable, 'replaced test executable bytes')
    Assert-Rejected 'replaced test executable after CTest' 'Binary changed'
    Reset-ExecutionEvidence
    Save-Receipt (New-Receipt)
    [System.IO.File]::WriteAllText($cachePath, "EPOCANVA_PRODUCT_TRACE:BOOL=ON`n")
    Assert-Rejected 'changed build configuration'
    [System.IO.File]::WriteAllText($cachePath, $passingCache)
    [System.IO.File]::WriteAllText($cachePath, "EPOCANVA_PRODUCT_TRACE:BOOL=OFF`nEPOCANVA_PRODUCT_BUILD_ID:STRING=old-sha`n")
    Save-Receipt (New-Receipt); Assert-Rejected 'rehash cannot bless a different configured source revision'
    [System.IO.File]::WriteAllText($cachePath, $passingCache)
    $bad = New-Receipt; $bad.binaries += $bad.binaries[0]; Save-Receipt $bad; Assert-Rejected 'duplicate binary'
    Save-Receipt (New-Receipt)
    [System.IO.File]::WriteAllText($paths.tsf, 'altered fixture')
    Assert-Rejected 'changed product DLL'
    if ($ForceFailure) { throw 'injected receipt test failure' }
    Write-Host 'RESULT PASS build receipt tampering tests'
    exit 0
} catch {
    Write-Host "RESULT FAIL $($_.Exception.Message)"
    exit 14
}
