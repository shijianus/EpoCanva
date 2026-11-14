# Synthetic input/command binding regression, never release evidence.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$RunRoot,[switch]$ForceFailure)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$RunRoot=[IO.Path]::GetFullPath($RunRoot).TrimEnd('\','/')
if ((Split-Path -Parent $RunRoot) -ne (Join-Path $repo 'build')) { throw 'RunRoot must be directly below build/.' }
$null=New-Item -ItemType Directory -Path $RunRoot -Force
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $repo 'tools\product\run_candidate_quality.ps1'),[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw 'Candidate runner parse failure.' }
function Run-Assignment([string]$Name) {
    $nodes=@($ast.FindAll({param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Operator -eq 'Equals' -and $n.Left.Extent.Text -ceq ('$'+$Name)},$true))
    if ($nodes.Count -ne 1) { throw "Expected one assignment for $Name" }
    . ([scriptblock]::Create($nodes[0].Extent.Text))
    return Get-Variable -Name $Name -ValueOnly
}
$Manifest=Join-Path $RunRoot 'UNIT-custom-manifest.json'
[IO.File]::WriteAllText($Manifest,'{"unit_fixture":true}')
$CasesDir=$RunRoot
$corpusManifestPath=Run-Assignment 'corpusManifestPath'
if ($corpusManifestPath -cne (Get-Item -LiteralPath $Manifest).FullName) { throw 'Custom manifest identity is not the executed input.' }
$runnerPath='UNIT-RUNNER'; $script:repoRoot=$repo; $BuildDir=$RunRoot
$PackageManifest=$Manifest; $Resources=$RunRoot; $Harness=$Manifest
$RimeDll=$Manifest; $RimeShared=$RunRoot; $EvidencePath=$Manifest
$AcceptanceProfile='internal-unsigned'; $TimeoutSeconds=123; $BaselineReport=$Manifest
$command=Run-Assignment 'runnerCommand'
foreach ($name in @('CasesDir','Manifest','TimeoutSeconds')) {
    $index=[array]::IndexOf($command,('-'+$name))
    if ($index -lt 0 -or $command[$index+1] -cne [string](Get-Variable -Name $name -ValueOnly)) { throw "Command omits actual $name" }
}
$tokens=$null; $errors=$null
$installerAst=[Management.Automation.Language.Parser]::ParseFile((Join-Path $repo 'tools\installer\build_msi.ps1'),[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw 'Installer parse failure.' }
$nodes=@($installerAst.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq 'Assert-WixVersion'},$true))
if ($nodes.Count -ne 1) { throw 'Missing production WiX version verifier.' }
. ([scriptblock]::Create($nodes[0].Extent.Text))
Assert-WixVersion '6.0.2+b3f3403' 0
foreach ($version in @('6.0.1+b3f3403','6.0.2+different','6.1.0','7.0.0','')) {
    $rejected=$false
    try { Assert-WixVersion $version 0 } catch { $rejected=$true }
    if (-not $rejected) { throw 'Unpinned WiX version accepted.' }
}
$rejected=$false
try { Assert-WixVersion '6.0.2+b3f3403' 5 } catch { $rejected=$true }
if (-not $rejected) { throw 'Failed version probe accepted.' }
if ($ForceFailure) { Write-Output 'RESULT FAIL intentional negative control'; exit 14 }
Write-Output 'RESULT PASS actual custom-manifest binding, complete input command and exact WiX-version behavior'
exit 0
