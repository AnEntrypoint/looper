# Batch-render A/B candidate engines on representative material.
# Outputs to scripts/ab-renders/engines/<base>__<engine>.wav
$root = "C:\dev\looper"
$harness = Join-Path $root "scripts\quality-harness.exe"
$outDir = Join-Path $root "scripts\ab-renders\engines"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$samples = @(
    @{path="scripts\samples\guitar_E2_low_E.wav"; fund=82.4},
    @{path="scripts\samples\guitar_G3_G_str.wav"; fund=196.0},
    @{path="scripts\samples\guitar_C3_48k.wav";   fund=130.8},
    @{path="scripts\samples\guitar_E4_high_E.wav";fund=329.6},
    @{path="scripts\quality-corpus\sine_82_4.wav";fund=82.4},
    @{path="scripts\quality-corpus\pluck_E2.wav"; fund=82.4}
)
$engines = @("signalsmith-128-48","signalsmith-64-32","yin-psola","sinc-delay-192","sinc-delay-256")

$results = @()
foreach ($s in $samples) {
    $base = [IO.Path]::GetFileNameWithoutExtension($s.path)
    foreach ($e in $engines) {
        $out = Join-Path $outDir "${base}__${e}.wav"
        $inp = Join-Path $root $s.path
        $json = & $harness $e $inp $s.fund $out 2>&1
        $line = "${base}|${e}|${json}"
        $results += $line
        Write-Host $line
    }
}
$results | Set-Content -Encoding utf8 (Join-Path $outDir "metrics.txt")
Write-Host "DONE. Outputs in $outDir"
