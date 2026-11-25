function Get-Sha256([string]$Path) {
    return Get-ProductIdentityFileSha256 $Path
}
function Assert-RelativePath([string]$Path) {
    if (-not $Path -or $Path -notmatch '^[A-Za-z0-9_.\-/]+$') { throw 'Invalid relative payload/evidence path.' }
    Assert-ProductRelativePath $Path
}
function Assert-PlainPath([string]$Path, [string]$Boundary) {
    $null = Assert-ProductPlainPath $Path $Boundary
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
    return ,(Read-ProductPassingJUnit $Path)
}
function Read-Reference($Reference, [switch]$CorrespondingSource) {
    Assert-ProductObjectFields $Reference @('path', 'sha256', 'bytes')
    if ($Reference.path -isnot [string]) { throw 'An evidence reference requires a relative string path.' }
    if ($CorrespondingSource) {
        Assert-ProductSourceRelativePath $Reference.path
    } else {
        Assert-RelativePath $Reference.path
    }
    if (-not (Test-ProductSha256 $Reference.sha256) -or -not (Test-ProductJsonInteger $Reference.bytes) -or
            $Reference.bytes -le 0) {
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
    $start = ConvertTo-ProductUtc $Entry.started_utc
    $end = ConvertTo-ProductUtc $Entry.completed_utc
    if ($end -lt $start -or $end -gt [DateTimeOffset]::UtcNow.AddMinutes(5)) { throw 'Invalid execution timestamps.' }
    if ($Entry.command -isnot [array] -or @($Entry.command).Count -lt 1 -or
            @($Entry.command | Where-Object { $_ -isnot [string] -or [string]::IsNullOrWhiteSpace($_) }).Count -ne 0) {
        throw 'The exact executed command must be recorded as a nonempty string array.'
    }
}
