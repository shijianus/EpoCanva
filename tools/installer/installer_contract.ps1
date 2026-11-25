. (Join-Path $PSScriptRoot '../product/evidence_contract.ps1')
. (Join-Path $PSScriptRoot 'provenance.ps1')

function Assert-InstallerBuildPath([string]$Path, [string]$Boundary) {
    foreach ($candidate in @($Path, $Boundary)) {
        # .NET Framework removes trailing dots/spaces while canonicalizing. Reject
        # these aliases before GetFullPath so PS 5.1 and PS 7 enforce the same rule.
        if (@($candidate.Replace('/', '\').Split('\') | Where-Object {
            $_ -notin @('.', '..') -and ($_.EndsWith('.') -or $_.EndsWith(' ')) }).Count) {
            throw 'Installer path contains a trailing-dot or trailing-space alias.'
        }
    }
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $root = [IO.Path]::GetFullPath($Boundary).TrimEnd('\', '/')
    if ($full -cnotmatch '^[A-Za-z]:\\' -or $full.Substring(3) -match '[:*?"<>|]' -or
        @($full.Substring(3).Split('\') | Where-Object { $_.EndsWith('.') -or $_.EndsWith(' ') -or
            $_ -match '^(CON|PRN|AUX|NUL|COM[1-9\u00b9\u00b2\u00b3]|LPT[1-9\u00b9\u00b2\u00b3])(?:\.|$)' }).Count) {
        throw 'Installer path must be a plain local path without device names or alternate streams.'
    }
    if (-not $full.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Installer path is outside $root"
    }
    $current = $full
    while ($true) {
        if (Test-Path -LiteralPath $current) {
            $entry = Get-Item -LiteralPath $current -Force -ErrorAction Stop
            if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Installer path traverses a reparse point: $current"
            }
        }
        # A checked child is unsafe when its boundary or any boundary ancestor is
        # a junction. Check up to the volume, not just the caller's boundary.
        $current = [IO.Path]::GetDirectoryName($current)
        if (-not $current) { break }
    }
    return $full
}

function Get-InstallerBuildDirectory([string]$RepoRoot, [string]$BuildDir, [string]$Preset) {
    $base = Join-Path $RepoRoot 'build'
    if (-not $BuildDir) { $BuildDir = Join-Path $base $Preset }
    $resolved = Assert-InstallerBuildPath $BuildDir $base
    if ([IO.Path]::GetDirectoryName($resolved) -ine $base) { throw 'BuildDir must be directly below build/' }
    return $resolved
}

function New-InstallerRunDirectory([string]$BuildDir, [string]$Label) {
    $path = Assert-InstallerBuildPath (Join-Path $BuildDir ($Label + '-' + [guid]::NewGuid().ToString('N'))) $BuildDir
    return (New-Item -ItemType Directory -Path $path -ErrorAction Stop).FullName
}

function Publish-InstallerFile([string]$Source, [string]$Destination) {
    if ([IO.File]::Exists($Destination)) { [IO.File]::Replace($Source, $Destination, [NullString]::Value) }
    else { [IO.File]::Move($Source, $Destination) }
}

function Write-InstallerJson([string]$Path, $Document) {
    $pending = $Path + '.' + [guid]::NewGuid().ToString('N') + '.pending'
    [IO.File]::WriteAllText($pending, (($Document | ConvertTo-Json -Depth 12) + "`n"),
        [Text.UTF8Encoding]::new($false))
    Publish-InstallerFile $pending $Path
}

function Get-InstallerSegmentSha256([IO.Stream]$Stream, [long]$ByteOffset, [long]$Bytes) {
    $ErrorActionPreference = 'Stop'
    if ($ByteOffset -lt 0 -or $Bytes -lt 0 -or $Bytes -gt $Stream.Length - $ByteOffset) {
        throw 'Invalid file byte span for SHA-256.'
    }
    $Stream.Position = $ByteOffset
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $buffer = New-Object byte[] 65536
        while ($Bytes -gt 0) {
            $read = $Stream.Read($buffer, 0, [int][Math]::Min($Bytes, $buffer.Length))
            if ($read -le 0) { throw 'Unexpected end of file while hashing.' }
            $null = $sha.TransformBlock($buffer, 0, $read, $null, 0)
            $Bytes -= $read
        }
        $null = $sha.TransformFinalBlock($buffer, 0, 0)
        return [BitConverter]::ToString($sha.Hash).Replace('-', '').ToLowerInvariant()
    } finally { $sha.Dispose() }
}

function Get-InstallerFileIdentity([string]$Path) {
    $ErrorActionPreference = 'Stop'
    $full = [IO.Path]::GetFullPath($Path)
    $stream = [IO.File]::Open($full, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        return [pscustomobject]@{ path = $full; sha256 = Get-InstallerSegmentSha256 $stream 0 $stream.Length;
            bytes = $stream.Length }
    } finally { $stream.Dispose() }
}

function Get-InstallerPeLayout([IO.BinaryReader]$Reader) {
    $stream = $Reader.BaseStream
    $stream.Position = 0
    if ($stream.Length -lt 64 -or $Reader.ReadUInt16() -ne 0x5a4d) { throw 'Installer has no DOS MZ header.' }
    $stream.Position = 60
    $pe = [long]$Reader.ReadUInt32()
    if ($pe -lt 64 -or $pe + 264 -gt $stream.Length) { throw 'Installer PE header is out of bounds.' }
    $stream.Position = $pe
    if ($Reader.ReadUInt32() -ne 0x00004550 -or $Reader.ReadUInt16() -ne 0x8664) {
        throw 'Installer is not an AMD64 PE image.'
    }
    $sections = $Reader.ReadUInt16()
    $stream.Position = $pe + 20
    $optionalBytes = $Reader.ReadUInt16()
    $characteristics = $Reader.ReadUInt16()
    $optional = $pe + 24
    if ($sections -lt 1 -or $sections -gt 96 -or $optionalBytes -ne 240 -or
        ($characteristics -band 2) -eq 0 -or ($characteristics -band 0x2000) -ne 0 -or
        $Reader.ReadUInt16() -ne 0x20b) { throw 'Installer is not an x64 PE32+ executable.' }
    $stream.Position = $optional + 60
    $headerBytes = [long]$Reader.ReadUInt32()
    $table = $optional + $optionalBytes
    if ($table + 40 * $sections -gt $headerBytes -or $headerBytes -gt $stream.Length) {
        throw 'Installer PE section table is out of bounds.'
    }
    $stream.Position = $optional + 108
    if ($Reader.ReadUInt32() -ne 16) { throw 'Installer PE data directory is incomplete.' }
    $stream.Position = $optional + 144
    $certificateOffset = [long]$Reader.ReadUInt32()
    $certificateBytes = [long]$Reader.ReadUInt32()
    $rawSections = @(
        for ($section = 0; $section -lt $sections; $section++) {
            $stream.Position = $table + 40 * $section + 16
            $bytes = [long]$Reader.ReadUInt32()
            $offset = [long]$Reader.ReadUInt32()
            if ($bytes -gt 0) { [pscustomobject]@{ byte_offset = $offset; bytes = $bytes } }
        }
    )
    $imageBytes = $headerBytes
    foreach ($section in ($rawSections | Sort-Object byte_offset)) {
        if ($section.byte_offset -lt $imageBytes) { throw 'Installer PE sections overlap.' }
        $imageBytes = $section.byte_offset + $section.bytes
    }
    $payloadLimit = $stream.Length
    if ($certificateOffset -ne 0 -or $certificateBytes -ne 0) {
        if ($certificateOffset % 8 -ne 0 -or $certificateBytes -lt 8 -or $certificateBytes % 8 -ne 0 -or
            $certificateOffset + $certificateBytes -ne $stream.Length) { throw 'Invalid PE certificate table byte span.' }
        $payloadLimit = $certificateOffset
    }
    if ($imageBytes -gt $payloadLimit) { throw 'Installer PE image is truncated.' }
    return [pscustomobject]@{ image_bytes = $imageBytes; payload_limit = $payloadLimit; certificate_bytes = $certificateBytes }
}

function Get-InstallerExeFacts([string]$ExePath, [string]$MsiPath) {
    $stream = [IO.File]::Open($ExePath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        $layout = Get-InstallerPeLayout $reader
        if ($layout.payload_limit - $layout.image_bytes -lt 56) { throw 'Installer overlay is missing or truncated.' }
        $stream.Position = $layout.image_bytes
        if ([Text.Encoding]::ASCII.GetString($reader.ReadBytes(8)) -cne 'EPOINST1') { throw 'Missing EPOINST1 at the PE overlay byte offset.' }
        $declaredBytes = $reader.ReadUInt64()
        $declaredHash = [BitConverter]::ToString($reader.ReadBytes(32)).Replace('-', '').ToLowerInvariant()
        $payloadOffset = $stream.Position
        if ($declaredBytes -lt 8 -or $declaredBytes -gt [uint64]($layout.payload_limit - $payloadOffset)) {
            throw 'Embedded MSI byte count is outside the overlay.'
        }
        if ([BitConverter]::ToString($reader.ReadBytes(8)) -cne 'D0-CF-11-E0-A1-B1-1A-E1') {
            throw 'Embedded payload is missing the MSI structured-storage signature.'
        }
        $stream.Position = $payloadOffset + [long]$declaredBytes
        $paddingBytes = $layout.payload_limit - $stream.Position
        if ($paddingBytes -gt 0) {
            if ($layout.certificate_bytes -eq 0 -or $paddingBytes -gt 7) { throw 'Unexpected trailing bytes after the embedded MSI.' }
            foreach ($byte in $reader.ReadBytes([int]$paddingBytes)) {
                if ($byte -ne 0) { throw 'Nonzero PE certificate alignment padding.' }
            }
        }
        $payloadHash = Get-InstallerSegmentSha256 $stream $payloadOffset ([long]$declaredBytes)
        $msi = Get-InstallerFileIdentity $MsiPath
        if ($payloadHash -cne $declaredHash -or $payloadHash -cne $msi.sha256 -or $declaredBytes -ne $msi.bytes) {
            throw 'Embedded MSI, overlay header and standalone MSI do not have the same identity.'
        }
        $stream.Position = 0
        if ($layout.image_bytes -gt 16777216) { throw 'Unexpected launcher image size over 16 MiB.' }
        $imageText = [Text.Encoding]::ASCII.GetString($reader.ReadBytes([int]$layout.image_bytes))
        $needle = $payloadHash + [char]0
        $first = $imageText.IndexOf($needle, [StringComparison]::Ordinal)
        if ($first -lt 0 -or $imageText.IndexOf($needle, $first + 1, [StringComparison]::Ordinal) -ge 0) {
            throw 'Launcher image does not contain exactly one compiled MSI digest.'
        }
        return [pscustomobject]@{
            path = [IO.Path]::GetFullPath($ExePath)
            sha256 = Get-InstallerSegmentSha256 $stream 0 $stream.Length
            bytes = $stream.Length
            architecture = 'x64'
            image_sha256 = Get-InstallerSegmentSha256 $stream 0 $layout.image_bytes
            overlay_byte_offset = $layout.image_bytes
            embedded_msi_sha256 = $payloadHash
            embedded_msi_bytes = [long]$declaredBytes
            certificate_table_bytes = $layout.certificate_bytes
            product_version = [Diagnostics.FileVersionInfo]::GetVersionInfo($ExePath).ProductVersion
            file_version = [Diagnostics.FileVersionInfo]::GetVersionInfo($ExePath).FileVersion
        }
    } finally { $reader.Dispose() }
}

function Get-InstallerMsiRows($Database, [string]$Query, [int]$ColumnCount) {
    $view = $Database.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $Database, @($Query))
    try {
        $null = $view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, @())
        while ($null -ne ($record = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, @()))) {
            try {
                $values = @(for ($column = 1; $column -le $ColumnCount; $column++) {
                    $record.GetType().InvokeMember('StringData', 'GetProperty', $null, $record, @($column))
                })
                ,$values
            } finally { $null = [Runtime.InteropServices.Marshal]::FinalReleaseComObject($record) }
        }
    } finally { $null = [Runtime.InteropServices.Marshal]::FinalReleaseComObject($view) }
}

function Assert-InstallerMsiName([string]$Name, [switch]$Directory) {
    # MSI DefaultDir uses target:source and short|long names. Every alternative
    # must be a single Windows component; source/target paths cannot escape /a.
    $sides = @($Name.Split(':'))
    if (-not $Name -or $sides.Count -gt 2 -or (-not $Directory -and $sides.Count -ne 1)) {
        throw 'Unsafe MSI directory or file name.'
    }
    foreach ($side in $sides) {
        $names = @($side.Split('|'))
        if ($names.Count -gt 2) { throw 'Unsafe MSI name alternatives.' }
        foreach ($part in $names) {
            if ($Directory -and $part -ceq '.') { continue }
            if (-not $part -or $part -match '[\\/<>:"|?*\x00-\x1f]' -or $part.EndsWith('.') -or $part.EndsWith(' ') -or
                $part -match '^(CON|PRN|AUX|NUL|COM[1-9\u00b9\u00b2\u00b3]|LPT[1-9\u00b9\u00b2\u00b3])(?:\.|$)') {
                throw 'Unsafe MSI directory or file name.'
            }
        }
    }
}

function Get-InstallerMsiFacts([string]$MsiPath) {
    $installer = New-Object -ComObject WindowsInstaller.Installer -ErrorAction Stop
    $database = $null
    $summary = $null
    try {
        $database = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $installer,
            @([IO.Path]::GetFullPath($MsiPath), 0))
        $summary = $database.GetType().InvokeMember('SummaryInformation', 'GetProperty', $null, $database, @(0))
        $template = $summary.GetType().InvokeMember('Property', 'GetProperty', $null, $summary, @(7))
        if ($template -cne 'x64;1033') { throw 'MSI Template Summary is not x64;1033.' }
        $properties = @{}
        foreach ($row in Get-InstallerMsiRows $database 'SELECT `Property`, `Value` FROM `Property`' 2) {
            $properties.Add($row[0], $row[1])
        }
        if ($properties.ALLUSERS -cne '1' -or $properties.ProductName -cne 'EpoCanva' -or
            $properties.UpgradeCode -cne '{4AE1849E-029B-4213-8457-B756806C4FCD}' -or
            $properties.ProductVersion -cnotmatch '^\d{1,3}\.\d{1,3}\.\d{1,5}$') {
            throw 'MSI product, per-machine scope or version differs from the EpoCanva contract.'
        }
        $numericVersion = [version]$properties.ProductVersion
        if ($numericVersion.Major -gt 255 -or $numericVersion.Minor -gt 255 -or $numericVersion.Build -gt 65535) {
            throw 'MSI version is outside Windows Installer bounds.'
        }
        $directories = @{}
        $directoryNames = @{}
        foreach ($row in Get-InstallerMsiRows $database 'SELECT `Directory`, `Directory_Parent`, `DefaultDir` FROM `Directory`' 3) {
            if ($row[0] -cnotmatch '^[A-Za-z_][A-Za-z0-9_.]*$' -or $properties.ContainsKey($row[0])) {
                throw 'MSI directory identity or property override is unsafe.'
            }
            Assert-InstallerMsiName $row[2] -Directory
            $directories.Add($row[0], $row[1])
            $directoryNames.Add($row[0], ($row[2].Split(':')[0].Split('|')[-1]))
            if ($row[0] -ceq 'TARGETDIR' -and ($row[1] -or $row[2] -cne 'SourceDir')) {
                throw 'MSI TARGETDIR is not the administrative image root.'
            }
        }
        if (-not $directories.ContainsKey('TARGETDIR')) { throw 'MSI administrative image root is absent.' }
        foreach ($directory in $directories.Keys) {
            $visited = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
            $current = $directory
            while ($current -cne 'TARGETDIR') {
                if (-not $visited.Add($current) -or -not $directories.ContainsKey($current)) {
                    throw 'MSI directory graph is cyclic or disconnected.'
                }
                $current = $directories[$current]
                if (-not $current) { throw 'MSI directory graph is disconnected.' }
            }
        }
        $components = @{}
        foreach ($row in Get-InstallerMsiRows $database 'SELECT `Component`, `Directory_`, `Attributes` FROM `Component`' 3) {
            if (-not $directories.ContainsKey($row[1]) -or ([int]$row[2] -band 256) -eq 0) {
                throw 'MSI component directory or x64 bitness is invalid.'
            }
            $components.Add($row[0], $row[1])
        }
        $payload = @(foreach ($row in Get-InstallerMsiRows $database 'SELECT `File`, `FileName`, `Component_`, `FileSize` FROM `File`' 4) {
            if ($row[0] -cnotmatch '^[A-Za-z_][A-Za-z0-9_.]*$') { throw 'Unsafe MSI cabinet member identity.' }
            Assert-InstallerMsiName $row[1]
            if (-not $components.ContainsKey($row[2])) { throw 'MSI file component is absent.' }
            $relative = $row[1].Split('|')[-1]
            $current = $components[$row[2]]
            while ($current -cne 'TARGETDIR') {
                if ($directoryNames[$current] -cne '.') { $relative = $directoryNames[$current] + '/' + $relative }
                $current = $directories[$current]
            }
            [pscustomobject]@{ id = $row[0]; path = $relative; bytes = [long]$row[3] }
        })
        $tables = @(Get-InstallerMsiRows $database 'SELECT `Name` FROM `_Tables`' 1 | ForEach-Object { $_[0] })
        foreach ($table in @('Binary', 'Icon')) {
            if ($table -cin $tables) {
                foreach ($row in Get-InstallerMsiRows $database ('SELECT `Name` FROM `' + $table + '`') 1) {
                    if ($row[0] -cnotmatch '^[A-Za-z_][A-Za-z0-9_.]*$') { throw 'Unsafe MSI exported stream identity.' }
                }
            }
        }
        $cabinets = @(Get-InstallerMsiRows $database 'SELECT `Cabinet` FROM `Media`' 1)
        if ($cabinets.Count -eq 0) { throw 'MSI has no media rows.' }
        foreach ($row in $cabinets) {
            if ($row[0] -cnotmatch '^#[A-Za-z0-9._-]+\.cab$') { throw 'MSI must use embedded cabinets only.' }
        }
        $allow = @{
            AdminUISequence = @('CostInitialize', 'FileCost', 'CostFinalize', 'ExecuteAction')
            AdminExecuteSequence = @('CostInitialize', 'FileCost', 'CostFinalize', 'InstallValidate',
                'InstallInitialize', 'InstallAdminPackage', 'InstallFiles', 'InstallFinalize')
        }
        $actions = @(Get-InstallerMsiRows $database 'SELECT `Action` FROM `CustomAction`' 1 | ForEach-Object { $_[0] })
        foreach ($table in $allow.Keys) {
            $administrative = @(Get-InstallerMsiRows $database ('SELECT `Action` FROM `' + $table + '` ORDER BY `Sequence`') 1 |
                ForEach-Object { $_[0] })
            if (($administrative -join ',') -cne ($allow[$table] -join ',') -or
                @($administrative | Where-Object { $_ -in $actions }).Count -ne 0) {
                throw "MSI contains an unexpected or custom administrative action in $table. Extraction was refused."
            }
        }
        return [pscustomobject]@{ template = $template; version = $properties.ProductVersion; files = $payload;
            product_code = $properties.ProductCode; administrative_actions_verified = $true }
    } finally {
        foreach ($object in @($summary, $database, $installer)) {
            if ($null -ne $object) { $null = [Runtime.InteropServices.Marshal]::FinalReleaseComObject($object) }
        }
    }
}

function Test-InstallerInteger($Value) { return $Value -is [int] -or $Value -is [long] }

function Get-InstallerWixLock {
    return [pscustomobject]@{ package_sha256 = '13caed0aa86898c9952eb8ba82c6ac6b43d1575bb731ac848e5edf5490a10428';
        source_commit = 'b3f340393117094a75ea8ced77f2357e4aa095e7' }
}

function Assert-InstallerEvidenceFile([string]$Path, [string]$Sha256, [string]$Boundary) {
    $safe = Assert-InstallerBuildPath $Path $Boundary
    $reference = Get-InstallerFileIdentity $safe
    if ($reference.bytes -eq 0 -or $reference.sha256 -cne $Sha256) {
        throw "Installer evidence file is empty or its SHA-256 differs: $safe"
    }
    return $safe
}

function Assert-InstallerDiagnoseReport([string]$Path) {
    $fields = @{}
    foreach ($line in Get-Content -LiteralPath $Path -Encoding UTF8 -ErrorAction Stop) {
        if ($line -cmatch '^([A-Za-z0-9_.]+)=(.*)$' -and $Matches[1] -cin @('package', 'manifest', 'manifest_failures', 'user_data')) {
            $fields.Add($Matches[1], $Matches[2])
        }
    }
    if ($fields['package'] -cne 'ready' -or $fields['manifest'] -cne 'ok' -or
        $fields['manifest_failures'] -cne '0' -or $fields['user_data'] -cne 'ok') {
        throw 'Extracted product diagnosis did not verify package integrity and isolated user data.'
    }
}

function Assert-InstallerMsiManifest([string]$Path, [string]$MsiPath) {
    $identity = Get-InstallerFileIdentity $Path
    $manifest = Read-ProductJson $identity.path
    if (-not (Test-InstallerInteger $manifest.format) -or $manifest.format -ne 4 -or
        $manifest.extraction.verified -isnot [bool] -or -not $manifest.extraction.verified -or
        $manifest.installer.manifest_included -isnot [bool] -or -not $manifest.installer.manifest_included -or
        $manifest.installer.helper_included -isnot [bool] -or -not $manifest.installer.helper_included -or
        $manifest.install_directory -cne 'ProgramFiles64Folder/EpoCanva' -or
        $manifest.installer.architecture -cne 'x64' -or $manifest.installer.scope -cne 'per-machine') {
        throw 'MSI manifest lacks verified extraction and x64 per-machine package bindings.'
    }
    $lock = Get-InstallerWixLock
    if ($manifest.builder.version -cne '6.0.2+b3f3403' -or $manifest.builder.source_commit -cne $lock.source_commit -or
        $manifest.builder.package_sha256 -cne $lock.package_sha256) { throw 'MSI manifest differs from the pinned WiX package.' }
    if ($manifest.extraction.method -cne 'wix-msi-decompile-static') { throw 'MSI evidence requires pure static extraction.' }
    foreach ($code in @($manifest.builder.probe_exit, $manifest.builder.build_exit, $manifest.extraction.static_exit,
        $manifest.extraction.diagnose_exit)) {
        if (-not (Test-InstallerInteger $code) -or $code -ne 0) { throw 'MSI manifest has a failed or missing build, extraction or diagnose exit.' }
    }
    $repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
    Assert-InstallerSources $manifest.source $repoRoot 'msi'
    $helper = Assert-InstallerHelperReceipt $repoRoot $manifest.helper.receipt_path $manifest.helper.binary_path
    if ($helper.reference.sha256 -cne $manifest.helper.receipt_sha256 -or
        $helper.binary.sha256 -cne $manifest.helper.sha256 -or
        -not (Test-InstallerInteger $manifest.helper.bytes) -or $helper.binary.bytes -ne $manifest.helper.bytes -or
        $helper.receipt.source_sha256 -cne $manifest.helper.source_sha256 -or
        $helper.receipt.toolchain_sha256 -cne $manifest.helper.toolchain_sha256 -or
        $helper.receipt.configuration_sha256 -cne $manifest.helper.configuration_sha256) {
        throw 'MSI helper provenance differs from its current source/build receipt.'
    }
    $directory = [IO.Path]::GetDirectoryName($identity.path)
    $null = Assert-InstallerEvidenceFile $manifest.builder.build_log $manifest.builder.build_log_sha256 $directory
    $null = Assert-InstallerEvidenceFile $manifest.extraction.static_log $manifest.extraction.static_log_sha256 $directory
    $diagnose = Assert-InstallerEvidenceFile $manifest.extraction.diagnose_report $manifest.extraction.diagnose_report_sha256 $directory
    Assert-InstallerDiagnoseReport $diagnose
    $msi = Get-InstallerFileIdentity $MsiPath
    if ($manifest.installer.path -isnot [string] -or $msi.path -ine [IO.Path]::GetFullPath($manifest.installer.path) -or
        $manifest.installer.sha256 -isnot [string] -or $manifest.installer.sha256 -cne $msi.sha256 -or
        -not (Test-InstallerInteger $manifest.installer.bytes) -or $manifest.installer.bytes -ne $msi.bytes) {
        throw 'Standalone MSI differs from the MSI manifest file reference.'
    }
    $facts = Get-InstallerMsiFacts $msi.path
    if ($manifest.msi_product_version -cne $facts.version -or
        (Get-InstallerFileIdentity $identity.path).sha256 -cne $identity.sha256 -or
        (Get-InstallerFileIdentity $msi.path).sha256 -cne $msi.sha256) { throw 'MSI identity changed during inspection.' }
    return [pscustomobject]@{ manifest = $manifest; reference = $identity; installer = $msi }
}

function Assert-EpoCanvaInstallerPackage {
    param([string]$BuildDir, [string]$ExePath, [string]$ExpectedCommit, [string]$ExpectedPackageSha256,
        [string]$InstallerManifest = '', [string]$BootstrapperManifest = '')

    $directory = Join-Path $BuildDir 'installer'
    if (-not $InstallerManifest) { $InstallerManifest = Join-Path $directory 'installer-manifest.json' }
    if (-not $BootstrapperManifest) { $BootstrapperManifest = Join-Path $directory 'bootstrapper-manifest.json' }
    $exe = Assert-InstallerBuildPath $ExePath $directory
    $msiManifestPath = Assert-InstallerBuildPath $InstallerManifest $directory
    $exeManifestPath = Assert-InstallerBuildPath $BootstrapperManifest $directory
    $exeReference = Get-InstallerFileIdentity $exeManifestPath
    $manifest = Read-ProductJson $exeManifestPath
    if (-not (Test-InstallerInteger $manifest.format) -or $manifest.format -ne 3 -or
        $manifest.destination_commit -cne $ExpectedCommit -or $manifest.package_manifest_sha256 -cne $ExpectedPackageSha256 -or
        $ExpectedCommit -cnotmatch '^[0-9a-f]{40}$' -or $ExpectedPackageSha256 -cnotmatch '^[0-9a-f]{64}$' -or
        $manifest.bootstrapper.path -isnot [string] -or $exe -ine [IO.Path]::GetFullPath($manifest.bootstrapper.path) -or
        [IO.Path]::GetExtension($exe) -ine '.exe' -or $manifest.bootstrapper.architecture -cne 'x64' -or
        $manifest.bootstrapper.payload_magic -cne 'EPOINST1' -or
        -not (Test-InstallerInteger $manifest.compiler.exit_code) -or $manifest.compiler.exit_code -ne 0 -or
        -not (Test-InstallerInteger $manifest.verification.exit_code) -or $manifest.verification.exit_code -ne 0 -or
        $manifest.verification.argument -cne '--verify') { throw 'Bootstrapper manifest identity or verification record is invalid.' }
    $repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
    Assert-InstallerSources $manifest.source $repoRoot 'bootstrapper'
    foreach ($reference in @($manifest.toolchain.compiler, $manifest.toolchain.linker, $manifest.toolchain.resource_compiler)) {
        $null = Assert-InstallerFileReference $reference
    }
    if ($manifest.compiler.identity.sha256 -cne $manifest.toolchain.compiler.sha256 -or
        -not (Test-InstallerInteger $manifest.resource.exit_code) -or $manifest.resource.exit_code -ne 0) {
        throw 'Bootstrapper toolchain or resource build differs.'
    }
    $null = Assert-InstallerEvidenceFile $manifest.compiler.log $manifest.compiler.log_sha256 $directory
    $null = Assert-InstallerFileReference $manifest.resource.log $directory
    $null = Assert-InstallerFileReference $manifest.resource.source $directory
    $reportPath = Assert-InstallerEvidenceFile $manifest.verification.report $manifest.verification.report_sha256 $directory
    $report = Read-ProductJson $reportPath
    if ($report.argument -cne '--verify' -or -not (Test-InstallerInteger $report.exit_code) -or $report.exit_code -ne 0 -or
        $report.sha256 -cne $manifest.bootstrapper.sha256) { throw 'Bootstrapper verification result differs from the EXE reference.' }
    $msiPath = Assert-InstallerBuildPath $manifest.msi_path $directory
    $msi = Assert-InstallerMsiManifest $msiManifestPath $msiPath
    if ($msi.reference.sha256 -cne $manifest.bootstrapper.installer_manifest_sha256 -or
        $msi.manifest.destination_commit -cne $ExpectedCommit -or $msi.manifest.package_manifest_sha256 -cne $ExpectedPackageSha256 -or
        $msi.manifest.product_version -cne $manifest.product_version) { throw 'EXE and MSI manifests describe different packages.' }
    $facts = Get-InstallerExeFacts $exe $msiPath
    if ($facts.sha256 -cne $manifest.bootstrapper.sha256 -or
        $facts.product_version -cne $manifest.product_version -or
        $facts.file_version -cne ($msi.manifest.msi_product_version + '.0') -or
        -not (Test-InstallerInteger $manifest.bootstrapper.bytes) -or $facts.bytes -ne $manifest.bootstrapper.bytes -or
        $facts.image_sha256 -cne $manifest.image_sha256 -or $facts.embedded_msi_sha256 -cne $manifest.embedded_msi_sha256 -or
        -not (Test-InstallerInteger $manifest.embedded_msi_bytes) -or $facts.embedded_msi_bytes -ne $manifest.embedded_msi_bytes -or
        -not (Test-InstallerInteger $manifest.overlay_byte_offset) -or $facts.overlay_byte_offset -ne $manifest.overlay_byte_offset -or
        $facts.certificate_table_bytes -ne 0 -or
        (Get-InstallerFileIdentity $exeManifestPath).sha256 -cne $exeReference.sha256) {
        throw 'Actual EXE, compiled image and embedded MSI do not match the bootstrapper manifest.'
    }
    return [pscustomobject]@{ bootstrapper = $facts; bootstrapper_manifest = $exeReference; msi = $msi }
}
