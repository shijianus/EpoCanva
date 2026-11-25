function Assert-ProductQualityBinaryBindings($Receipt, [string]$BuildDir, [string]$Harness, [string]$RimeDll) {
    $expected = Join-Path $BuildDir 'candidate_evaluation_tests/Release/epocanva_candidate_eval_cli.exe'
    $actual = Assert-ProductPlainPath $Harness $BuildDir
    if ($actual -ine [IO.Path]::GetFullPath($expected)) { throw 'Quality harness belongs to a different candidate build.' }
    $matches = @($Receipt.test_binaries | Where-Object { $_.path -is [string] -and $_.path -ieq $actual })
    if ($matches.Count -ne 1) { throw 'Quality harness is not uniquely bound by the verified build receipt.' }
    Assert-ProductBinaryRecord $matches[0] 'epocanva_candidate_eval_cli.exe' $actual
    $engines = @($Receipt.binaries | Where-Object { $_.name -ceq 'rime' })
    if ($engines.Count -ne 1) { throw 'Quality engine is absent from the verified build receipt.' }
    Assert-ProductBinaryRecord $engines[0] 'rime' $RimeDll
    return [pscustomobject]@{ harness = $matches[0]; rime = $engines[0] }
}
