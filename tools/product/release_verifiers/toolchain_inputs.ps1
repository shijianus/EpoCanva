# Tool discovery is read-only. No installer, signing utility, registry or host is invoked.
# A repo-pinned builder provisioned under build/ is a real builder even though it is not on
# the persistent user PATH. The bootstrap script pins its version; the artifact gate still
# requires a real installer file, and no gate is waived by this discovery.
foreach ($pinnedWix in @((Join-Path $RepoRoot 'build\_local\wix-tool\wix.exe'),
        (Join-Path $RepoRoot 'build\tools\wix\wix.exe'))) {
    if (Test-Path -LiteralPath $pinnedWix -PathType Leaf) {
        $env:PATH = (Split-Path -Parent $pinnedWix) + ';' + $env:PATH
        break
    }
}
foreach ($name in @('wix.exe', 'candle.exe', 'light.exe', 'ISCC.exe', 'makensis.exe', 'msixmgr.exe', 'signtool.exe')) {
    $tools[$name] = [bool](Get-Command $name -CommandType Application -ErrorAction SilentlyContinue)
}
