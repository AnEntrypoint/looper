# Generate guitar-like single-note WAVs with realistic harmonic spectrum.
# Plucked-string approximation: fundamental + N harmonics with decay,
# attack transient, exponential body decay. Output: 16-bit mono PCM.

param(
    [int]$SampleRate = 48000,
    [double]$DurationSec = 3.0
)

$ErrorActionPreference = 'Stop'

function Make-GuitarTone {
    param([double]$Fund, [string]$Out)
    $N = [int]($SampleRate * $DurationSec)
    $buf = New-Object 'Int16[]' $N

    # Harmonic series with plucked-string-like spectrum:
    # H_k amplitude ~ 1/k^0.7, decay rate increases with harmonic
    # Slight inharmonicity (stretched harmonics: f_k = k * f0 * sqrt(1 + k^2 * B), B=2e-4)
    $B = 0.00015
    $nHarm = 8
    $twoPi = 2.0 * [Math]::PI

    # Per-sample envelope: 5ms attack, then exponential decay tau=1.5s
    $tauSamp = $SampleRate * 1.5
    $atkSamp = [int]($SampleRate * 0.005)

    # Pre-compute harmonic amps + freqs
    $hAmp = New-Object 'Double[]' $nHarm
    $hFrq = New-Object 'Double[]' $nHarm
    $hPhi = New-Object 'Double[]' $nHarm
    for ($k = 0; $k -lt $nHarm; $k++) {
        $kk = $k + 1
        $hFrq[$k] = $Fund * $kk * [Math]::Sqrt(1.0 + $kk * $kk * $B)
        $hAmp[$k] = 1.0 / [Math]::Pow($kk, 0.7)
        $hPhi[$k] = ($kk * 0.13) % 1.0 * $twoPi   # phase scatter
    }

    # Normalize harmonic amps so peak <= 1.0
    $sumAmp = 0.0; foreach ($a in $hAmp) { $sumAmp += $a }
    for ($k = 0; $k -lt $nHarm; $k++) { $hAmp[$k] /= $sumAmp }

    for ($i = 0; $i -lt $N; $i++) {
        $t = $i / [double]$SampleRate
        # Envelope
        $env = 1.0
        if ($i -lt $atkSamp) { $env = $i / [double]$atkSamp }
        $env *= [Math]::Exp(-$i / $tauSamp)

        # Sum harmonics, each with slightly different decay
        $s = 0.0
        for ($k = 0; $k -lt $nHarm; $k++) {
            $kk = $k + 1
            $hDec = [Math]::Exp(-$i / ($tauSamp / [Math]::Sqrt($kk)))
            $s += $hAmp[$k] * $hDec * [Math]::Sin($twoPi * $hFrq[$k] * $t + $hPhi[$k])
        }

        $sample = $s * $env * 0.65 * 32767.0
        if ($sample -gt 32767) { $sample = 32767 }
        if ($sample -lt -32768) { $sample = -32768 }
        $buf[$i] = [Int16][Math]::Round($sample)
    }

    # Write 16-bit mono WAV
    $byteRate = $SampleRate * 2
    $dataSize = $buf.Length * 2
    $fs = [System.IO.File]::Create($Out)
    $bw = New-Object System.IO.BinaryWriter $fs
    $bw.Write([byte[]][char[]]'RIFF')
    $bw.Write([int](36 + $dataSize))
    $bw.Write([byte[]][char[]]'WAVE')
    $bw.Write([byte[]][char[]]'fmt ')
    $bw.Write([int]16)
    $bw.Write([Int16]1)        # PCM
    $bw.Write([Int16]1)        # mono
    $bw.Write([int]$SampleRate)
    $bw.Write([int]$byteRate)
    $bw.Write([Int16]2)        # block align
    $bw.Write([Int16]16)       # bits/sample
    $bw.Write([byte[]][char[]]'data')
    $bw.Write([int]$dataSize)
    foreach ($s in $buf) { $bw.Write($s) }
    $bw.Close(); $fs.Close()
    Write-Host ('wrote {0} ({1:F1}Hz, {2:F1}s)' -f $Out, $Fund, $DurationSec)
}

# Standard guitar tuning notes
$notes = @(
    @{ name='E2_low_E';   freq= 82.41 },
    @{ name='A2_A_str';   freq=110.00 },
    @{ name='D3_D_str';   freq=146.83 },
    @{ name='G3_G_str';   freq=196.00 },
    @{ name='B3_B_str';   freq=246.94 },
    @{ name='E4_high_E';  freq=329.63 }
)

$dir = $PSScriptRoot
foreach ($n in $notes) {
    Make-GuitarTone -Fund $n.freq -Out (Join-Path $dir ("guitar_" + $n.name + ".wav"))
}
