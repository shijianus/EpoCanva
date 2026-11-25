# Source-controlled registration contract; never evaluate generated CTest/CMake
# files or execute a command obtained from evidence. Import on both producer and
# consumer paths. Updating CMake registrations requires a reviewed manifest edit.
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'evidence_contract.ps1')
$script:productInventoryContractCache = $null

function Copy-ProductInventoryValue($Value) {
    if ($Value -is [pscustomobject]) {
        $copy = [ordered]@{}
        foreach ($property in $Value.PSObject.Properties) {
            $copy[$property.Name] = Copy-ProductInventoryValue $property.Value
        }
        return [pscustomobject]$copy
    }
    if ($Value -is [array]) {
        $copy = [object[]]::new($Value.Count)
        for ($itemIndex = 0; $itemIndex -lt $Value.Count; $itemIndex++) {
            $copy[$itemIndex] = Copy-ProductInventoryValue $Value[$itemIndex]
        }
        return ,$copy
    }
    return $Value
}

function Get-ProductInventorySha256([byte[]]$Bytes) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose() }
}

function Get-ProductInventorySourceSha256([string]$Path) {
    # Match Git's text eol=lf contract across Windows checkouts. This digest is
    # UTF-8 without BOM, with CRLF/CR normalized to LF, not a binary file digest.
    $text = [IO.File]::ReadAllText($Path, [Text.UTF8Encoding]::new($false, $true))
    $text = $text.Replace("`r`n", "`n").Replace("`r", "`n")
    return Get-ProductInventorySha256 ([Text.Encoding]::UTF8.GetBytes($text))
}

function Get-ProductInventoryRoots([string]$RepoRoot, [string]$BuildDir) {
    $root = [IO.Path]::GetFullPath($RepoRoot).Replace('\', '/').TrimEnd('/')
    $build = [IO.Path]::GetFullPath($BuildDir).Replace('\', '/').TrimEnd('/')
    if (-not $build.StartsWith($root + '/build/', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Registered test contract requires a build directory under the repository build/ boundary.'
    }
    # Same UTF-8 bytes as string(SHA256 ... "${CMAKE_BINARY_DIR}") in product
    # CMakeLists.txt. Do not wildcard the directory's twelve hexadecimal digits.
    $tag = (Get-ProductInventorySha256 ([Text.Encoding]::UTF8.GetBytes($build))).Substring(0, 12)
    return [pscustomobject]@{ repo = $root; build = $build; policy_tag = $tag }
}

function ConvertTo-ProductInventoryToken([string]$Value, $Roots, [bool]$Executable = $false) {
    if ($Value -match '[\x00-\x1f]' -or $Value -match '\{(?:REPO|BUILD|POLICY_BUILD_TAG|TOOL:)') {
        throw 'Registered test evidence contains a control character or a reserved contract placeholder.'
    }
    $parts = $Value.Split([char]';')
    $normalized = [Collections.Generic.List[string]]::new()
    for ($partIndex = 0; $partIndex -lt $parts.Count; $partIndex++) {
        $part = $parts[$partIndex]
        $prefix = ''
        # CMake -D arguments and environment assignments retain their literal
        # key. Only a complete absolute Windows path in the value is normalized.
        if ($part -match '\A((?:-D)?[A-Za-z_][A-Za-z0-9_]*=)(.*)\z') {
            $prefix = $Matches[1]
            $part = $Matches[2]
        }
        if ($part -match '\A[A-Za-z]:[\\/]') {
            $path = [IO.Path]::GetFullPath($part).Replace('\', '/')
            if ($path.Equals($Roots.build, [StringComparison]::OrdinalIgnoreCase)) {
                $part = '{BUILD}'
            } elseif ($path.StartsWith($Roots.build + '/', [StringComparison]::OrdinalIgnoreCase)) {
                $part = '{BUILD}/' + $path.Substring($Roots.build.Length + 1)
            } elseif ($path.Equals($Roots.repo, [StringComparison]::OrdinalIgnoreCase)) {
                $part = '{REPO}'
            } elseif ($path.StartsWith($Roots.repo + '/', [StringComparison]::OrdinalIgnoreCase)) {
                $part = '{REPO}/' + $path.Substring($Roots.repo.Length + 1)
                $policyPrefix = '{REPO}/build/policy-' + $Roots.policy_tag + '-'
                if ($part.StartsWith($policyPrefix, [StringComparison]::Ordinal)) {
                    $part = '{REPO}/build/policy-{POLICY_BUILD_TAG}-' + $part.Substring($policyPrefix.Length)
                }
            } elseif (($Executable -and $partIndex -eq 0 -and $prefix -ceq '') -or $prefix -ceq '-DEXE=') {
                $basename = [IO.Path]::GetFileName($path)
                if ($basename -cnotin @('cmake.exe', 'powershell.exe', 'python.exe')) {
                    throw 'Registered test command refers to an unapproved external tool basename.'
                }
                # This contract verifies the role/name and exact arguments.
                # Toolchain provenance separately verifies actual tool bytes.
                $part = '{TOOL:' + $basename + '}'
            } else {
                throw 'Registered test argument escapes the repository/build roots.'
            }
        } elseif ($part -match '\A(?:[\\/]{2}|[A-Za-z]:)') {
            throw 'Registered test command contains an unsupported Windows path form.'
        }
        $normalized.Add($prefix + $part)
    }
    return ($normalized -join ';')
}

function Get-ProductRegisteredTestContract([string]$RepoRoot) {
    $ErrorActionPreference = 'Stop'
    $path = Assert-ProductPlainPath (Join-Path $RepoRoot 'data/manifests/product-test-inventory.json') $RepoRoot
    # Re-read/re-hash the bytes, rather than trusting mtime or a caller's hash.
    # Reuse only the expensive strict parse of identical source bytes; return a
    # deep copy so an in-process caller cannot mutate the cached contract.
    $bytes = [IO.File]::ReadAllBytes($path)
    $digest = Get-ProductInventorySha256 $bytes
    if ($null -eq $script:productInventoryContractCache -or
            $script:productInventoryContractCache.path -cne $path -or
            $script:productInventoryContractCache.sha256 -cne $digest) {
        $text = [Text.UTF8Encoding]::new($false, $true).GetString($bytes)
        $parsed = ConvertFrom-ProductJson $text
        $script:productInventoryContractCache = @{ path = $path; sha256 = $digest; parsed = $parsed }
    }
    return Copy-ProductInventoryValue $script:productInventoryContractCache.parsed
}

function Assert-ProductInventoryValueEqual($Expected, $Actual, $Roots, [string]$Context) {
    if ($Expected -is [string]) {
        if ($Actual -isnot [string] -or
                $Expected -cne (ConvertTo-ProductInventoryToken $Actual $Roots)) {
            throw "Registered test contract property mismatch: $Context"
        }
    } elseif ($Expected -is [array]) {
        if ($Actual -isnot [array] -or $Expected.Count -ne $Actual.Count) {
            throw "Registered test contract property array mismatch: $Context"
        }
        for ($itemIndex = 0; $itemIndex -lt $Expected.Count; $itemIndex++) {
            Assert-ProductInventoryValueEqual $Expected[$itemIndex] $Actual[$itemIndex] $Roots $Context
        }
    } elseif ($Expected -is [bool]) {
        if ($Actual -isnot [bool] -or $Expected -ne $Actual) {
            throw "Registered test contract property boolean mismatch: $Context"
        }
    } elseif ((Test-ProductJsonInteger $Expected) -or $Expected -is [double] -or $Expected -is [decimal]) {
        if ((-not (Test-ProductJsonInteger $Actual) -and $Actual -isnot [double] -and $Actual -isnot [decimal]) -or
                [double]::IsNaN([double]$Actual) -or [double]::IsInfinity([double]$Actual) -or $Expected -ne $Actual) {
            throw "Registered test contract property number mismatch: $Context"
        }
    } else { throw "Unsupported registered test contract property type: $Context" }
}

function Assert-ProductRegisteredTestContract {
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][string]$BuildDir,
        [Parameter(Mandatory)]$Inventory
    )
    $ErrorActionPreference = 'Stop'
    $roots = Get-ProductInventoryRoots $RepoRoot $BuildDir
    # Read the fixed source manifest afresh on every invocation. Neither the
    # receipt nor the untrusted inventory can select/replace the expected list.
    $contract = Get-ProductRegisteredTestContract $RepoRoot
    Assert-ProductObjectFields $contract @('schema', 'kind', 'configuration', 'normalization', 'sources', 'tests')
    if (-not (Test-ProductJsonInteger $contract.schema) -or $contract.schema -ne 1 -or
            -not [object]::Equals($contract.kind, 'epocanva-product-registered-tests') -or
            -not [object]::Equals($contract.configuration, 'Release') -or
            -not [object]::Equals($contract.normalization, 'windows-root-build-tool-v1') -or
            $contract.sources -isnot [array] -or $contract.sources.Count -eq 0 -or
            $contract.tests -isnot [array] -or $contract.tests.Count -eq 0) {
        throw 'Unsupported or empty source-controlled registered test contract.'
    }
    $sourceNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($source in $contract.sources) {
        Assert-ProductObjectFields $source @('path', 'sha256')
        if ($source.path -isnot [string] -or -not $sourceNames.Add($source.path) -or
                -not (Test-ProductSha256 $source.sha256)) { throw 'Invalid registration source binding.' }
        Assert-ProductRelativePath $source.path
        $sourcePath = Assert-ProductPlainPath (Join-Path $RepoRoot $source.path) $RepoRoot
        $digest = Get-ProductInventorySourceSha256 $sourcePath
        if ($digest -cne $source.sha256) {
            throw "Registered test CMake source changed; review and update the source contract: $($source.path)"
        }
    }
    Assert-ProductObjectFields $Inventory @('kind', 'version', 'tests') @('backtraceGraph')
    Assert-ProductObjectFields $Inventory.version @('major', 'minor')
    if (-not [object]::Equals($Inventory.kind, 'ctestInfo') -or -not (Test-ProductJsonInteger $Inventory.version.major) -or
            $Inventory.version.major -ne 1 -or -not (Test-ProductJsonInteger $Inventory.version.minor) -or
            $Inventory.version.minor -ne 0 -or $Inventory.tests -isnot [array]) {
        throw 'Unsupported registered CTest inventory schema.'
    }
    $expectedByName = [Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
    foreach ($expected in $contract.tests) {
        Assert-ProductObjectFields $expected @('name', 'command', 'properties', 'defined_in', 'line')
        if ($expected.name -isnot [string] -or [string]::IsNullOrWhiteSpace($expected.name) -or
                $expectedByName.ContainsKey($expected.name) -or $expected.command -isnot [array] -or
                $expected.command.Count -eq 0 -or @($expected.command | Where-Object { $_ -isnot [string] }).Count -ne 0 -or
                $expected.properties -isnot [pscustomobject] -or $expected.defined_in -isnot [string] -or
                -not $sourceNames.Contains($expected.defined_in) -or
                -not (Test-ProductJsonInteger $expected.line) -or $expected.line -le 0) {
            throw 'Invalid or duplicate registration in the source-controlled contract.'
        }
        $expectedByName.Add($expected.name, $expected)
    }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($test in $Inventory.tests) {
        Assert-ProductObjectFields $test @('name', 'command', 'config', 'properties') @('backtrace')
        if ($test.name -isnot [string] -or -not $seen.Add($test.name) -or -not $expectedByName.ContainsKey($test.name)) {
            throw "Unknown or duplicate registered test name: $($test.name)"
        }
        $expected = $expectedByName[$test.name]
        if (-not [object]::Equals($test.config, $contract.configuration) -or $test.command -isnot [array] -or
                $test.command.Count -ne $expected.command.Count) {
            throw "Registered test configuration/command shape mismatch: $($test.name)"
        }
        for ($argumentIndex = 0; $argumentIndex -lt $expected.command.Count; $argumentIndex++) {
            if ($test.command[$argumentIndex] -isnot [string] -or
                    $expected.command[$argumentIndex] -cne (ConvertTo-ProductInventoryToken $test.command[$argumentIndex] $roots ($argumentIndex -eq 0))) {
                throw "Registered test command/argument mismatch: $($test.name), argument ordinal $argumentIndex"
            }
        }
        if ($test.properties -isnot [array]) { throw 'CTest test properties must be an array.' }
        $propertyNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        foreach ($property in $test.properties) {
            Assert-ProductObjectFields $property @('name', 'value')
            if ($property.name -isnot [string] -or -not $propertyNames.Add($property.name) -or
                    $property.name -cnotin @($expected.properties.PSObject.Properties.Name)) {
                throw "Unknown or duplicate registered test property: $($test.name)"
            }
            Assert-ProductInventoryValueEqual $expected.properties.($property.name) $property.value $roots "$($test.name)/$($property.name)"
        }
        if ($propertyNames.Count -ne @($expected.properties.PSObject.Properties).Count) {
            throw "Missing registered test property: $($test.name)"
        }
    }
    $missing = @($expectedByName.Keys | Where-Object { -not $seen.Contains($_) } | Sort-Object)
    if ($missing.Count -ne 0) { throw ('Source-controlled registered tests are missing: ' + ($missing -join ', ')) }
    # No success object: callers must not serialize a self-asserted verified flag.
}
