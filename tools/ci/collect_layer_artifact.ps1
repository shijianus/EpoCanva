[CmdletBinding()]
param([Parameter(Mandatory)][string]$Layer)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $root 'tools/product/build_identity.ps1')
if ($Layer -cnotin @('contract-gate', 'root-unit-tests', 'backend', 'windows-product',
        'windows-integration-acceptance', 'full-quality-stress-acceptance') -or
        $env:GITHUB_RUN_ID -cnotmatch '\A[1-9][0-9]*\z' -or $env:GITHUB_RUN_ATTEMPT -cnotmatch '\A[1-9][0-9]*\z') {
    throw 'Invalid CI artifact scope.'
}
$layerDir = Join-Path $root "build/ci-acceptance-$($env:GITHUB_RUN_ID)-$($env:GITHUB_RUN_ATTEMPT)/$Layer"
$summaryPath = Assert-ProductPlainPath (Join-Path $layerDir 'summary.json') $root
$summary = Read-ProductJson $summaryPath
$destination = Join-Path $layerDir 'artifact'
$null = New-Item -ItemType Directory -Path $destination
Copy-Item -LiteralPath $summaryPath -Destination (Join-Path $destination 'summary.json')
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
function Copy-BoundOutputs($Document) {
    foreach ($property in $Document.outputs.PSObject.Properties) {
        $reference = $property.Value
        Assert-ProductObjectFields $reference @('path', 'sha256', 'bytes')
        Assert-ProductRelativePath $reference.path
        $source = Assert-ProductPlainPath (Join-Path $root $reference.path) $root
        if (-not (Test-ProductJsonInteger $reference.bytes) -or $reference.bytes -le 0 -or
                (Get-Item -LiteralPath $source).Length -ne $reference.bytes -or
                (Get-ProductIdentityFileSha256 $source) -cne $reference.sha256) { throw 'CI artifact source changed before upload.' }
        if ($seen.Add($reference.path)) {
            $target = Join-Path $destination ('payload/' + $reference.path)
            $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target)
            Copy-Item -LiteralPath $source -Destination $target
            if ((Get-ProductIdentityFileSha256 $target) -cne $reference.sha256) { throw 'CI artifact copy failed byte verification.' }
            if ($property.Name.StartsWith('predecessor-', [StringComparison]::Ordinal)) {
                Copy-BoundOutputs (Read-ProductJson $source)
            }
        }
    }
}
Copy-BoundOutputs $summary
