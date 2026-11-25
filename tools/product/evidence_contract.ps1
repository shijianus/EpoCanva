# Shared parsing contracts. Producers still validate freshly observed output;
# consumers independently reopen and validate every bound file.
Set-StrictMode -Version Latest

function ConvertFrom-ProductJson {
    param([Parameter(Mandatory, ValueFromPipeline)][string]$InputObject)
    process {
        Assert-ProductJsonTokens $InputObject
        # PowerShell 7.5+ otherwise silently converts ISO strings to DateTime;
        # preserve the JSON wire types used by the 5.1 producer/consumer.
        if ((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')) {
            return ,(ConvertFrom-Json -InputObject $InputObject -DateKind String -ErrorAction Stop)
        }
        return ,(ConvertFrom-Json -InputObject $InputObject -ErrorAction Stop)
    }
}

function Read-ProductJson([string]$Path) {
    $reader = [IO.StreamReader]::new($Path, [Text.UTF8Encoding]::new($false, $true), $false)
    try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
    if ($text.StartsWith([string][char]0xFEFF, [StringComparison]::Ordinal)) { $text = $text.Substring(1) }
    return ,(ConvertFrom-ProductJson $text)
}

function Write-ProductJson([string]$Path, $Value) {
    $pending = "$Path.$([guid]::NewGuid().ToString('N')).tmp"
    try {
        # WinPS 5.1 attaches an adapted Count member to parsed arrays. Passing
        # that wrapper back to ConvertTo-Json writes {value: [...], Count: N}.
        # Copy the outer array so the JSON wire type remains an array, including
        # the zero- and one-element cases. Preserve the element objects.
        $wireValue = $Value
        if ($Value -is [array]) {
            $wireValue = [object[]]::new($Value.Length)
            [Array]::Copy($Value, $wireValue, $Value.Length)
        }
        [IO.File]::WriteAllText($pending, (ConvertTo-Json -InputObject $wireValue -Depth 20), [Text.UTF8Encoding]::new($false))
        if ([IO.File]::Exists($Path)) { [IO.File]::Replace($pending, $Path, [NullString]::Value) }
        else { [IO.File]::Move($pending, $Path) }
    } finally {
        if ([IO.File]::Exists($pending)) { [IO.File]::Delete($pending) }
    }
}

function Assert-ProductJsonTokens([string]$Text) {
    # Scan UTF-16 code units before ConvertFrom-Json discards duplicate members.
    # This is a token/security check; the native parser still validates grammar.
    $objects = [Collections.Generic.Stack[object]]::new()
    $expectKey = $false
    $offsetUtf16 = 0
    $previous = ''
    $tokenPattern = [regex]'\G(?:[ \t\n\r]+|"(?:[^"\\\x00-\x1f]|\\(?:["\\/bfnrt]|u[0-9a-fA-F]{4}))*"|-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?(?=[ \t\n\r,}\]]|\z)|true|false|null|[{}\[\],:])'
    while ($offsetUtf16 -lt $Text.Length) {
        $match = $tokenPattern.Match($Text, $offsetUtf16)
        if (-not $match.Success) { throw 'Invalid JSON token.' }
        $token = $match.Value
        $offsetUtf16 += $token.Length
        if ([string]::IsNullOrWhiteSpace($token)) { continue }
        if ($token -cmatch '^-?[0-9].*[.eE]') {
            $number = 0.0
            if (-not [double]::TryParse($token, [Globalization.NumberStyles]::Float,
                    [Globalization.CultureInfo]::InvariantCulture, [ref]$number) -or [double]::IsInfinity($number)) {
                throw 'JSON number is outside the finite range.'
            }
        }
        if ($token -eq '{') {
            $objects.Push([Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase))
            $expectKey = $true
        } elseif ($token -eq '[') {
            $objects.Push($null)
            $expectKey = $false
        } elseif ($token -eq '}' -or $token -eq ']') {
            if ($objects.Count -eq 0) { throw 'Unbalanced JSON container.' }
            if ($previous -eq ',') { throw 'Trailing commas are not accepted in JSON.' }
            $null = $objects.Pop()
            $expectKey = $false
        } elseif ($token -eq ',') {
            $expectKey = $objects.Count -gt 0 -and $null -ne $objects.Peek()
        } elseif ($token -eq ':') {
            $expectKey = $false
        } elseif ($expectKey) {
            if (-not $token.StartsWith('"') -or $objects.Count -eq 0) { throw 'Invalid JSON object member.' }
            $key = if ($token.Contains('\')) { ConvertFrom-Json -InputObject $token -ErrorAction Stop }
                else { $token.Substring(1, $token.Length - 2) }
            if (-not $objects.Peek().Add($key)) { throw 'Duplicate or case-aliased JSON object member.' }
            $expectKey = $false
        }
        $previous = $token
    }
    if ($objects.Count -ne 0) { throw 'Unbalanced JSON container.' }
}

function Test-ProductJsonInteger($Value) {
    return $Value -is [int] -or $Value -is [long]
}

function Assert-ProductObjectFields($Value, [string[]]$Required, [string[]]$Optional = @()) {
    if ($Value -isnot [pscustomobject]) { throw 'Evidence schema expects a JSON object.' }
    $names = @($Value.PSObject.Properties.Name)
    if (@($Required | Where-Object { $_ -cnotin $names }).Count -ne 0 -or
            @($names | Where-Object { $_ -cnotin ($Required + $Optional) }).Count -ne 0) {
        throw 'Evidence JSON object has missing or unexpected fields.'
    }
}

function Test-ProductSha256($Value) {
    return $Value -is [string] -and $Value -cmatch '\A[0-9a-f]{64}\z'
}

function ConvertTo-ProductUtc($Value) {
    $timestamp = [DateTimeOffset]::MinValue
    if ($Value -isnot [string] -or $Value -cnotmatch '\A[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(?:\.[0-9]{1,7})?(?:Z|\+00:00)\z' -or
            -not [DateTimeOffset]::TryParse($Value, [Globalization.CultureInfo]::InvariantCulture,
                [Globalization.DateTimeStyles]::None, [ref]$timestamp) -or $timestamp.Offset -ne [TimeSpan]::Zero) {
        throw 'Evidence requires a system UTC timestamp in ISO 8601 format.'
    }
    return $timestamp
}

function Assert-ProductRelativePath([string]$Path) {
    if (-not $Path -or $Path -match '[\\\x00-\x1f<>:"|?*]') { throw 'Invalid relative file path.' }
    foreach ($part in $Path.Split('/')) {
        if (-not $part -or $part.EndsWith('.') -or $part.EndsWith(' ') -or
                $part -match '\A(?:con|prn|aux|nul|(?:com|lpt)[1-9\u00b9\u00b2\u00b3])(?:\.|\z)') {
            throw 'Invalid relative file path component.'
        }
    }
}

function Assert-ProductPlainPath([string]$Path, [string]$Boundary) {
    $full = [IO.Path]::GetFullPath($Path)
    $base = [IO.Path]::GetFullPath($Boundary).TrimEnd('\', '/')
    if ($full -ine $base -and -not $full.StartsWith($base + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) { throw 'Evidence path escapes its permitted directory.' }
    if ($full -ine $base) { Assert-ProductRelativePath ($full.Substring($base.Length + 1).Replace('\', '/')) }
    $current = $full
    while ($current -and $current.Length -ge $base.Length) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -and
                    ($item.LinkType -or $item.Target)) { throw 'Evidence path redirects through a link.' }
        }
        if ($current -ieq $base) { break }
        $current = Split-Path -Parent $current
    }
    return $full
}

function Read-ProductPassingJUnit([string]$Path) {
    $settings = New-Object System.Xml.XmlReaderSettings
    $settings.DtdProcessing = [System.Xml.DtdProcessing]::Prohibit
    $settings.XmlResolver = $null
    $settings.MaxCharactersInDocument = 64MB
    $reader = [System.Xml.XmlReader]::Create($Path, $settings)
    try {
        $document = New-Object System.Xml.XmlDocument
        $document.XmlResolver = $null
        $document.Load($reader)
    } finally { $reader.Dispose() }
    if ($document.DocumentElement.LocalName -cnotin @('testsuite', 'testsuites') -or
            $document.DocumentElement.NamespaceURI) { throw 'Unsupported JUnit schema.' }
    if ($document.SelectNodes('//*[namespace-uri() != ""]').Count -ne 0 -or
            $document.SelectNodes('//testcase[not(parent::testsuite)]').Count -ne 0) {
        throw 'JUnit contains namespaced or misplaced testcases.'
    }
    $cases = @($document.SelectNodes('//testcase'))
    if ($cases.Count -eq 0 -or $document.SelectNodes('//failure | //error | //skipped').Count -ne 0) {
        throw 'JUnit is empty or contains failed, errored or skipped cases.'
    }
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($case in $cases) {
        $name = $case.GetAttribute('name')
        if ([string]::IsNullOrWhiteSpace($name) -or -not $names.Add($name) -or
                ($case.HasAttribute('status') -and $case.GetAttribute('status') -cnotin @('run', 'passed')) -or
                ($case.HasAttribute('result') -and $case.GetAttribute('result') -cnotin @('completed', 'passed'))) {
            throw 'JUnit has duplicate, unnamed, unexecuted or nonpassing cases.'
        }
    }
    foreach ($suite in $document.SelectNodes('//testsuite | /testsuites')) {
        foreach ($attribute in @('failures', 'errors', 'skipped', 'disabled')) {
            if ($suite.HasAttribute($attribute) -and $suite.GetAttribute($attribute) -cnotmatch '^0+$') {
                throw 'JUnit summary reports failure, error, skip or disabled tests.'
            }
        }
        if ($suite.HasAttribute('tests') -and
                ($suite.GetAttribute('tests') -cnotmatch '^[0-9]+$' -or
                 [uint64]$suite.GetAttribute('tests') -ne $suite.SelectNodes('.//testcase').Count)) {
            throw 'JUnit summary count differs from the actual cases.'
        }
    }
    return ,$cases
}

function Read-ProductTestInventory([string]$InventoryPath) {
    $inventory = Read-ProductJson $InventoryPath
    if ($inventory.kind -cne 'ctestInfo' -or -not (Test-ProductJsonInteger $inventory.version.major) -or
            $inventory.version.major -ne 1 -or -not (Test-ProductJsonInteger $inventory.version.minor) -or
            $inventory.version.minor -lt 0 -or $inventory.tests -isnot [array] -or $inventory.tests.Count -eq 0) {
        throw 'Unsupported or empty CTest inventory schema.'
    }
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($test in $inventory.tests) {
        if ($test.name -isnot [string] -or [string]::IsNullOrWhiteSpace($test.name) -or -not $names.Add($test.name) -or
                $test.command -isnot [array] -or $test.command.Count -eq 0 -or
                [string]::IsNullOrWhiteSpace($test.command[0]) -or
                @($test.command | Where-Object { $_ -isnot [string] }).Count -ne 0) {
            throw 'CTest inventory contains unnamed or duplicate registrations.'
        }
    }
    foreach ($prefix in @('windows.', 'product.', 'engine_abi.', 'english.core.', 'learning.core.',
            'learning.repository.', 'chinese.core.', 'french.core.', 'composition.core.', 'language_packages.')) {
        if (@($names | Where-Object { $_.StartsWith($prefix, [StringComparison]::Ordinal) }).Count -eq 0) {
            throw "Mandatory test group was not registered: $prefix"
        }
    }
    return $inventory
}

function Assert-ProductTestInventory($Inventory, $Cases) {
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($test in $Inventory.tests) { $null = $names.Add($test.name) }
    if ($names.Count -ne @($Cases).Count) { throw 'CTest registered/executed counts differ.' }
    foreach ($case in $Cases) {
        if (-not $names.Remove($case.GetAttribute('name'))) { throw 'CTest execution differs from its registered names.' }
    }
    if ($names.Count -ne 0) { throw 'CTest registered tests were not executed.' }
}

function Get-ProductTestExecutablePaths($Inventory, [string]$BuildDir) {
    $paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $base = [IO.Path]::GetFullPath($BuildDir).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    foreach ($test in $Inventory.tests) {
        if ($null -ne $test.PSObject.Properties['command'] -and $test.command -is [array] -and $test.command.Count -gt 0) {
            $executable = [IO.Path]::GetFullPath($test.command[0])
            if ($executable.StartsWith($base, [StringComparison]::OrdinalIgnoreCase) -and
                    [IO.Path]::GetExtension($executable) -ieq '.exe') { $null = $paths.Add($executable) }
        }
    }
    return @($paths | Sort-Object)
}

function Assert-ProductBuildCommands($Commands, [string]$BuildDir, [string]$EvidenceDir, [string]$Configuration) {
    $stages = @('configure', 'build', 'inventory', 'ctest')
    if ($Commands -isnot [array] -or $Commands.Count -ne $stages.Count) {
        throw 'Build requires configure, build, inventory and CTest process records.'
    }
    $previousEnd = [DateTimeOffset]::MinValue
    for ($stageIndex = 0; $stageIndex -lt $stages.Count; $stageIndex++) {
        $record = $Commands[$stageIndex]
        Assert-ProductObjectFields $record @('stage', 'exit_code', 'executable', 'arguments', 'started_utc', 'completed_utc')
        $tool = if ($stageIndex -lt 2) { 'cmake.exe' } else { 'ctest.exe' }
        if (-not [object]::Equals($record.stage, $stages[$stageIndex]) -or -not (Test-ProductJsonInteger $record.exit_code) -or
                $record.exit_code -ne 0 -or $record.executable -isnot [string] -or
                -not [IO.Path]::IsPathRooted($record.executable) -or
                [IO.Path]::GetFileName($record.executable) -ine $tool -or $record.arguments -isnot [array] -or
                $record.arguments.Count -eq 0 -or
                @($record.arguments | Where-Object { $_ -isnot [string] -or [string]::IsNullOrWhiteSpace($_) }).Count -ne 0) {
            throw 'Build process record has a missing/failed exit code or invalid command.'
        }
        $start = ConvertTo-ProductUtc $record.started_utc
        $end = ConvertTo-ProductUtc $record.completed_utc
        if ($start -lt $previousEnd -or $end -lt $start -or $end -gt [DateTimeOffset]::UtcNow.AddMinutes(5)) {
            throw 'Build process records have invalid chronological order.'
        }
        $previousEnd = $end
        # Record the actual requested build directory, not just a successful
        # command for another candidate left next to this receipt.
        if (@($record.arguments | Where-Object { $_ -ieq $BuildDir }).Count -ne 1) {
            throw 'Build process record targets a different build directory.'
        }
        if ($stageIndex -ge 2) {
            $arguments = @('--test-dir', $BuildDir, '-C', $Configuration)
            if ($stageIndex -eq 2) {
                $arguments += '--show-only=json-v1'
                $outputPath = Join-Path $EvidenceDir 'ctest-list.json'
            } else {
                $outputPath = Join-Path $EvidenceDir 'ctest.xml'
                $arguments += @('--output-on-failure', '--no-tests=error', '--output-junit', $outputPath)
            }
            if (($arguments -join "`0") -cne ($record.arguments -join "`0")) {
                throw 'CTest command does not target the exact configuration and unfiltered run.'
            }
            $written = [IO.File]::GetLastWriteTimeUtc($outputPath)
            if ($written -lt $start.UtcDateTime.AddSeconds(-2) -or $written -gt $end.UtcDateTime.AddSeconds(2)) {
                throw 'CTest output timestamp is outside its recorded invocation.'
            }
        }
    }
}
