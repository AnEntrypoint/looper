param([string]$ResultPath = 'C:\dev\looper\scripts\quality-results\all.jsonl')

$harness = 'C:\dev\looper\scripts\quality-harness.exe'
$corpus = 'C:\dev\looper\scripts\quality-corpus'
$dir = Split-Path $ResultPath -Parent
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
[IO.File]::WriteAllText($ResultPath, '')

$tests = @(
    @('sine_82_4.wav',     82.41),
    @('sine_110.wav',     110.00),
    @('sine_220.wav',     220.00),
    @('sine_440.wav',     440.00),
    @('guitar_E2.wav',     82.41),
    @('guitar_A2.wav',    110.00),
    @('guitar_D3.wav',    146.83),
    @('pluck_E2.wav',      82.41),
    @('pluck_A2.wav',     110.00)
)
$engines = @(
    'signalsmith-64-32','signalsmith-64-16','signalsmith-96-24','signalsmith-96-32',
    'signalsmith-128-32','signalsmith-128-48','signalsmith-128-48-split',
    'signalsmith-160-48','signalsmith-160-64',
    'signalsmith-192-48','signalsmith-192-64',
    'signalsmith-256-64','signalsmith-256-96','signalsmith-256-96-split',
    'downsample-64-32','downsample-96-32','downsample-128-32',
    'sinc-delay-256','sinc-delay-384'
)

$total = $engines.Count * $tests.Count; $i = 0
foreach ($eng in $engines) {
    foreach ($t in $tests) {
        $i++
        $wav = Join-Path $corpus $t[0]
        $fund = $t[1]
        Write-Host -NoNewline "[$i/$total] $eng $($t[0])..."
        $tmpOut = [IO.Path]::GetTempFileName()
        $p = Start-Process -FilePath $harness -ArgumentList @($eng, $wav, $fund) -RedirectStandardOutput $tmpOut -NoNewWindow -Wait -PassThru
        $content = [IO.File]::ReadAllText($tmpOut)
        [IO.File]::Delete($tmpOut)
        if ($p.ExitCode -eq 0 -and $content.StartsWith('{')) {
            [IO.File]::AppendAllText($ResultPath, $content)
            Write-Host ' ok'
        } else {
            Write-Host " FAIL (rc=$($p.ExitCode))"
            [IO.File]::AppendAllText($ResultPath, "{`"engine`":`"$eng`",`"input`":`"$($t[0])`",`"error`":`"failed-rc-$($p.ExitCode)`"}`n")
        }
    }
}
Write-Host ""
$lines = [IO.File]::ReadAllLines($ResultPath)
Write-Host "result lines: $($lines.Length)"
