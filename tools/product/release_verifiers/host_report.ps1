function Test-HardAcceptanceReport([string]$Id, $Report, $Entry, $Bindings) {
    # These are measured-evidence contracts, not approval records. The gate never
    # executes an evidence command or infers an installed-host run from unit tests.
    if ($Id -cnotin @('stress-reference-model', 'host-soak', 'acceptance-ci-provenance') -or
            $Report.format -cne ('epocanva-' + $Id + '-v1')) { throw "Unsupported $Id report." }
    foreach ($key in $Bindings.Keys) {
        if ($Report.$key -isnot [string] -or $Report.$key -cne $Bindings[$key]) {
            throw "$Id report differs from the current candidate: $key"
        }
    }
    if ($Report.result -cne 'passed') { throw "$Id did not pass its measured protocol." }
    if ([DateTimeOffset]::Parse($Report.started_utc) -ne [DateTimeOffset]::Parse($Entry.started_utc) -or
            [DateTimeOffset]::Parse($Report.completed_utc) -ne [DateTimeOffset]::Parse($Entry.completed_utc)) {
        throw "$Id report and execution timestamps disagree."
    }
    if ($Id -ceq 'stress-reference-model') {
        $seeds = @('0x5eed000000000001', '0x5eed000000000002', '0x5eed000000000003',
            '0x5eed000000000004', '0x5eed000000000005')
        if ($Report.seeds -isnot [array] -or $Report.seeds.Count -ne $seeds.Count -or
                $Report.total_ops -isnot [int] -or $Report.total_ops -ne 500000 -or
                $Report.oracle -cne 'independent-editor-reference-model' -or
                $Report.failure_reduction -cne 'replay-and-minimize') {
            throw 'Stress requires the independent editor oracle, replay/minimization and all 500000 edits.'
        }
        foreach ($seed in $seeds) {
            $rows = @($Report.seeds | Where-Object { $_.seed -ceq $seed })
            if ($rows.Count -ne 1 -or $rows[0].operations -isnot [int] -or $rows[0].operations -ne 100000 -or
                    $rows[0].mismatches -isnot [int] -or $rows[0].mismatches -ne 0) {
                throw "Stress seed $seed was absent, shortened or failed."
            }
            $operationTotal = 0L
            foreach ($operation in @('insert', 'delete', 'caret-utf16', 'selection-utf16', 'undo', 'invalid-state')) {
                $count = $rows[0].operation_counts.$operation
                if ($count -isnot [int] -or $count -lt 1) { throw "Stress did not execute $operation for $seed." }
                $operationTotal += $count
            }
            if ($operationTotal -ne 100000) { throw "Stress operation counts disagree for $seed." }
        }
        $null = Read-Reference $Report.replay_log
        $null = Read-Reference $Report.resource_samples
        $null = Read-Reference $Report.negative_control
    } elseif ($Id -ceq 'host-soak') {
        $start = [DateTimeOffset]::Parse($Report.started_utc)
        $end = [DateTimeOffset]::Parse($Report.completed_utc)
        $duration = ($end - $start).TotalSeconds
        $reportedDuration = $Report.duration_seconds
        if ($duration -lt 1800 -or $reportedDuration -is [bool] -or
                ($reportedDuration -isnot [int] -and $reportedDuration -isnot [long] -and $reportedDuration -isnot [double]) -or
                [double]::IsNaN([double]$reportedDuration) -or [double]::IsInfinity([double]$reportedDuration) -or
                [double]$reportedDuration -lt 1800 -or
                [math]::Abs([double]$reportedDuration - $duration) -gt 1 -or
                $Report.environment -cne 'authorized-isolated-interactive-windows' -or
                $Report.installed -isnot [bool] -or $Report.installed -ne $true -or
                $Report.activated -isnot [bool] -or $Report.activated -ne $true) {
            throw 'Host soak requires at least 1800 measured seconds in an authorized installed/activated desktop.'
        }
        if ($Report.samples -isnot [array] -or $Report.samples.Count -lt 31) {
            throw 'Host soak lacks continuous resource and input-progress samples.'
        }
        $previous = $start
        $progress = -1L
        $previousCpuSeconds = -1.0
        foreach ($sample in $Report.samples) {
            $time = [DateTimeOffset]::Parse($sample.utc)
            if ($time -lt $previous -or ($time - $previous).TotalSeconds -gt 60 -or $time -gt $end -or
                    $sample.completed_interactions -isnot [int] -or $sample.completed_interactions -le $progress) {
                throw 'Host soak has a sampling gap or no continuous input/candidate progress.'
            }
            foreach ($metric in @('working_set_bytes', 'handles', 'cpu_seconds')) {
                $value = $sample.$metric
                if ($value -is [bool] -or ($value -isnot [int] -and $value -isnot [long] -and $value -isnot [double]) -or
                        [double]::IsNaN([double]$value) -or [double]::IsInfinity([double]$value) -or [double]$value -lt 0) {
                    throw "Host soak lacks a finite nonnegative $metric measurement."
                }
            }
            if ([double]$sample.cpu_seconds -lt $previousCpuSeconds) { throw 'Host soak cumulative CPU time moved backwards.' }
            $previousCpuSeconds = [double]$sample.cpu_seconds
            $previous = $time
            $progress = $sample.completed_interactions
        }
        if (($end - $previous).TotalSeconds -gt 60) { throw 'Host soak stopped monitoring before completion.' }
        $null = Read-Reference $Report.isolation_authorization
        $null = Read-Reference $Report.performance_approval
        $null = Read-Reference $Report.host_observations
    } elseif ($Id -ceq 'acceptance-ci-provenance') {
        . (Join-Path $PSScriptRoot '../../ci/ci_provenance.ps1')
        Assert-CiProvenance $Report $Bindings
    }
    return $true
}
