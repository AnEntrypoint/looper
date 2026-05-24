# Better analysis: focus on plucks for latency, sustained for THD, fund tracking.
$rows = [IO.File]::ReadAllLines('C:\dev\looper\scripts\quality-results\all.jsonl') | ForEach-Object { $_ | ConvertFrom-Json }
$ok = $rows | Where-Object { $_.error -eq $null }

function GetFile($p) { Split-Path $p -Leaf }

$agg = $ok | Group-Object engine | ForEach-Object {
    $rs = $_.Group
    $pluck = $rs | Where-Object { (Split-Path $_.input -Leaf) -like 'pluck*' }
    $sust  = $rs | Where-Object { (Split-Path $_.input -Leaf) -like 'guitar*' -or (Split-Path $_.input -Leaf) -like 'sine*' }
    $pluckLat = if ($pluck.Count) { ($pluck | Measure-Object lat_ms -Average).Average } else { 99 }
    $sustFund = if ($sust.Count) { ($sust | ForEach-Object { [Math]::Abs($_.fund_err_hz) } | Measure-Object -Average).Average } else { 99 }
    $sustThd  = if ($sust.Count) { ($sust | Measure-Object thd_pct -Average).Average } else { 9999 }
    $pluckThd = if ($pluck.Count) { ($pluck | Measure-Object thd_pct -Average).Average } else { 9999 }
    $sustAttCorr = if ($sust.Count) { ($sust | Measure-Object att_corr -Average).Average } else { 0 }

    [PSCustomObject]@{
        engine     = $_.Name
        pluck_lat  = [Math]::Round($pluckLat, 2)
        sust_fund_err = [Math]::Round($sustFund, 2)
        sust_thd   = [Math]::Round($sustThd, 1)
        pluck_thd  = [Math]::Round($pluckThd, 1)
        sust_att   = [Math]::Round($sustAttCorr, 3)
        # Quality: lower = better. Penalty if pluck_lat > 8ms.
        # weight fund_err (10x — pitch accuracy is the most-important quality metric)
        # weight thd (0.05x — already in pct)
        # bonus for high att_corr
    }
}

$agg = $agg | ForEach-Object {
    $latPen = if ($_.pluck_lat -gt 8) { 500 } else { 0 }
    $score = $_.sust_fund_err * 10 + $_.sust_thd * 0.05 + $_.pluck_thd * 0.05 + (1 - $_.sust_att) * 20 + $latPen
    $_ | Add-Member -NotePropertyName score -NotePropertyValue ([Math]::Round($score, 2)) -PassThru
}

$agg | Sort-Object score | Format-Table -AutoSize
