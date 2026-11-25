function Get-PinnedInstallerWix([string]$LauncherPath) {
    $lock = Get-InstallerWixLock
    $package = Join-Path (Split-Path -Parent $LauncherPath) '.store\wix\6.0.2\wix\6.0.2'
    $archivePath = Assert-InstallerBuildPath (Join-Path $package 'wix.6.0.2.nupkg') (Split-Path -Parent $LauncherPath)
    Add-Type -AssemblyName System.IO.Compression
    $stream = [IO.File]::OpenRead($archivePath)
    try {
        if ((Get-InstallerSegmentSha256 $stream 0 $stream.Length) -cne $lock.package_sha256) {
            throw 'WiX 6.0.2 NuGet package SHA-256 differs from the pin.'
        }
        $stream.Position = 0
        $archive = [IO.Compression.ZipArchive]::new($stream)
        try {
            $expected = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
            $toolsRoot = Join-Path $package 'tools'
            $sha = [Security.Cryptography.SHA256]::Create()
            try {
                foreach ($entry in $archive.Entries) {
                    if (-not $entry.FullName.StartsWith('tools/', [StringComparison]::Ordinal) -or -not $entry.Name) { continue }
                    $path = Assert-InstallerBuildPath (Join-Path $package $entry.FullName) $toolsRoot
                    if (-not $expected.Add($path)) { throw 'Duplicate file in the pinned WiX tool package.' }
                    $entryStream = $entry.Open()
                    try { $expectedHash = [BitConverter]::ToString($sha.ComputeHash($entryStream)).Replace('-', '').ToLowerInvariant() }
                    finally { $entryStream.Dispose() }
                    if ((Get-InstallerFileIdentity $path).sha256 -cne $expectedHash) {
                        throw "Provisioned WiX file differs from the pinned archive: $($entry.FullName)"
                    }
                }
            } finally { $sha.Dispose() }
            $pending = [Collections.Generic.Queue[string]]::new()
            $pending.Enqueue($toolsRoot)
            while ($pending.Count) {
                foreach ($entry in Get-ChildItem -LiteralPath $pending.Dequeue() -Force) {
                    if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'WiX tool tree contains a reparse point.' }
                    if ($entry.PSIsContainer) { $pending.Enqueue($entry.FullName) }
                    elseif (-not $expected.Remove($entry.FullName)) { throw "Unpinned file in WiX tool tree: $($entry.FullName)" }
                }
            }
            if ($expected.Count -ne 0) { throw 'WiX tool tree is incomplete.' }
            return [pscustomobject]@{ executable = Get-InstallerFileIdentity (Join-Path $toolsRoot 'net6.0\any\wix.exe');
                package_sha256 = $lock.package_sha256; source_commit = $lock.source_commit }
        } finally { $archive.Dispose() }
    } finally { $stream.Dispose() }
}
