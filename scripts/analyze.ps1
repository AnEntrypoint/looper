param([string]$ResultPath = 'C:\dev\looper\scripts\quality-results\all.jsonl')

$rows = [IO.File]::ReadAllLines($ResultPath) | ForEach-Object { $_ | ConvertFrom-Json }

# Filter out failed rows
$ok = $rows | Where-Object { $_.error -eq $null }

# Aggregate per engine
$agg = $ok | Group-Object engine | ForEach-Object {
    $rs = $_.Group
    [PSCustomObject]@{
        engine    = $_.Name
        n         = $rs.Count
        avg_lat_ms = [Math]::Round((($rs | Measure-Object lat_ms -Average).Average), 2)
        avg_thd    = [Math]::Round((($rs | Measure-Object thd_pct -Average).Average), 1)
        avg_fund_err_abs = [Math]::Round((($rs | ForEach-Object { [Math]::Abs($_.fund_err_hz) } | Measure-Object -Average).Average), 2)
        avg_att_corr = [Math]::Round((($rs | Measure-Object att_corr -Average).Average), 3)
        avg_clicks = [Math]::Round((($rs | Measure-Object clicks -Average).Average), 0)
        # Quality composite: lower = better
        # weights: fund_err (3x), thd (1x), -att_corr (2x), clicks (0.01x), penalty if lat>8ms (+1000)
    }
}

$agg = $agg | ForEach-Object {
    $latPen = if ($_.avg_lat_ms -gt 8) { 1000 } else { 0 }
    $_ | Add-Member -NotePropertyName composite -NotePropertyValue ([Math]::Round(($_.avg_fund_err_abs * 3 + $_.avg_thd * 0.1 + (1 - $_.avg_att_corr) * 50 + $_.avg_clicks * 0.001 + $latPen), 2)) -PassThru
}

# Sort by composite (lower is better) — but show all
$agg | Sort-Object composite | Format-Table engine, n, avg_lat_ms, avg_thd, avg_fund_err_abs, avg_att_corr, avg_clicks, composite -AutoSize

# Per-test detail for in-budget engines
Write-Host "`n=== Per-test detail for engines with avg_lat_ms <= 8 ==="
$inBudget = $agg | Where-Object { $_.avg_lat_ms -le 8 } | Select-Object -ExpandProperty engine
foreach ($eng in $inBudget) {
    Write-Host "`n--- $eng ---"
    $ok | Where-Object engine -eq $eng | ForEach-Object {
        '{0,-25} fund_err={1,6:F2}Hz  thd={2,6:F1}%  att={3,5:F2}  clicks={4,4}' -f (Split-Path $_.input -Leaf), $_.fund_err_hz, $_.thd_pct, $_.att_corr, $_.clicks
    }
}
