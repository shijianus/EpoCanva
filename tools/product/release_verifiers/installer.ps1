Add-Gate 'installer-artifact' {
    if ($null -eq $evidence -or $null -eq $evidence.PSObject.Properties['installer']) {
        throw 'No hash-bound distributable installer artifact or lifecycle evidence.'
    }
    $script:installer = Read-Reference $evidence.installer
    if ($null -eq $source -or $null -eq $manifest) { throw 'Installer verification requires source and package identity.' }
    . (Join-Path $RepoRoot 'tools/installer/installer_contract.ps1')
    $verifiedInstaller = Assert-EpoCanvaInstallerPackage -BuildDir $BuildDir -ExePath $installer `
        -ExpectedCommit $source.commit -ExpectedPackageSha256 $manifest.sha256
    if ($verifiedInstaller.bootstrapper.sha256 -cne $evidence.installer.sha256) {
        throw 'Installer changed during independent payload verification.'
    }
    'Actual x64 EXE, embedded MSI, source, toolchain and payload receipts independently verified; lifecycle remains separate.'
}
Add-Gate 'installer-toolchain' {
    $builder = $tools['wix.exe'] -or ($tools['candle.exe'] -and $tools['light.exe']) -or $tools['ISCC.exe'] -or $tools['makensis.exe']
    if (-not $builder) { throw 'No WiX/Inno Setup/NSIS builder found on PATH. Probe: Get-Command wix.exe,candle.exe,light.exe,ISCC.exe,makensis.exe,msixmgr.exe -CommandType Application. msixmgr alone is a deployment tool, not a builder.' }
    'An installer builder is available; lifecycle testing remains a separate mandatory gate.'
}

