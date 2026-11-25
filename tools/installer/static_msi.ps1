# Extract with the pinned WiX reader. Never invoke Windows Installer actions or
# the Windows Installer service. Reconstruct paths from the checked MSI tables.
function Export-InstallerMsiStatic([string]$MsiPath, [string]$WixPath, [string]$RunDir, [string]$Destination) {
    $facts = Get-InstallerMsiFacts $MsiPath
    $exports = Join-Path $RunDir 'exported-streams'
    $intermediate = Join-Path $RunDir 'extract-work'
    $decompiled = Join-Path $RunDir 'decompiled.wxs'
    $log = Join-Path $RunDir 'static-extract.log'
    foreach ($path in @($exports, $intermediate, $Destination)) {
        $null = Assert-InstallerBuildPath $path $RunDir
        $null = New-Item -ItemType Directory -Path $path -ErrorAction Stop
    }
    $savedTemp, $savedTmp, $savedError = $env:TEMP, $env:TMP, $ErrorActionPreference
    try {
        $env:TEMP = $intermediate
        $env:TMP = $intermediate
        $ErrorActionPreference = 'Continue'
        $PSNativeCommandUseErrorActionPreference = $false
        $output = & $WixPath msi decompile $MsiPath -x $exports -intermediateFolder $intermediate -o $decompiled 2>&1 | Out-String
        $code = $LASTEXITCODE
    } finally { $env:TEMP = $savedTemp; $env:TMP = $savedTmp; $ErrorActionPreference = $savedError }
    [IO.File]::WriteAllText($log, ("method=wix-msi-decompile-static`nexit=$code`n" + $output), [Text.UTF8Encoding]::new($false))
    if ($code -ne 0) { throw "Static MSI extraction failed with exit $code. See $log" }
    $fileRoot = Join-Path $exports 'File'
    $expected = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $destinations = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in $facts.files) {
        $inputFile = Assert-InstallerBuildPath (Join-Path $fileRoot $file.id) $exports
        $target = Assert-InstallerBuildPath (Join-Path $Destination $file.path) $Destination
        if (-not $expected.Add($inputFile) -or -not $destinations.Add($target)) { throw 'Duplicate static MSI payload path.' }
        $identity = Get-InstallerFileIdentity $inputFile
        if ($identity.bytes -ne $file.bytes) { throw 'Extracted MSI file byte count differs from its File table.' }
        $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($target)) -Force
        Copy-Item -LiteralPath $inputFile -Destination $target -ErrorAction Stop
        if ((Get-InstallerFileIdentity $target).sha256 -cne $identity.sha256) { throw 'Static MSI copy changed.' }
    }
    foreach ($entry in Get-ChildItem -LiteralPath $fileRoot -Force) {
        if ($entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
            -not $expected.Remove($entry.FullName)) { throw 'Extra or redirected static MSI payload.' }
    }
    if ($expected.Count) { throw 'Missing static MSI payload.' }
    return [pscustomobject]@{ method = 'wix-msi-decompile-static'; exit_code = $code; log = Get-InstallerFileIdentity $log }
}
