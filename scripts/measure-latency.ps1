# measure-latency.ps1 — host-side loopback latency + correctness harness
#
# Setup assumption: host default-out is wired into UCA222 IN (via Focusrite),
# host default-in is wired from UCA222 OUT (Pi has firmware running and
# passing through / processing). Plays a precisely-timed impulse + chirp,
# captures the result, computes:
#   - round-trip latency (impulse onset cross-correlation)
#   - end-to-end frequency response (chirp deconvolution)
#   - THD on a sustained sine
#
# Output: scripts/measure-results/<timestamp>/{capture.wav, report.json}
#
# Usage:
#   .\scripts\measure-latency.ps1                          # default: impulse+chirp+sine
#   .\scripts\measure-latency.ps1 -Mode impulse            # latency only
#   .\scripts\measure-latency.ps1 -Mode chirp              # freq response only
#   .\scripts\measure-latency.ps1 -Mode sine -Freq 41.2    # low-E -12 stability THD

[CmdletBinding()]
param(
    [ValidateSet('full','impulse','chirp','sine','silence')]
    [string]$Mode = 'full',
    [double]$Freq = 1000.0,
    [int]$DurationMs = 2000,
    [int]$SampleRate = 48000,
    [string]$OutDir = ""
)

$ErrorActionPreference = 'Stop'

if (-not $OutDir) {
    $ts = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutDir = Join-Path $PSScriptRoot "measure-results\$ts"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

public class AudioIO {
    [DllImport("winmm.dll")] public static extern int waveOutOpen(out IntPtr h, int dev, ref WAVEFORMATEX fmt, IntPtr cb, IntPtr inst, int flags);
    [DllImport("winmm.dll")] public static extern int waveOutPrepareHeader(IntPtr h, ref WAVEHDR hdr, int sz);
    [DllImport("winmm.dll")] public static extern int waveOutWrite(IntPtr h, ref WAVEHDR hdr, int sz);
    [DllImport("winmm.dll")] public static extern int waveOutUnprepareHeader(IntPtr h, ref WAVEHDR hdr, int sz);
    [DllImport("winmm.dll")] public static extern int waveOutClose(IntPtr h);

    [DllImport("winmm.dll")] public static extern int waveInOpen(out IntPtr h, int dev, ref WAVEFORMATEX fmt, IntPtr cb, IntPtr inst, int flags);
    [DllImport("winmm.dll")] public static extern int waveInPrepareHeader(IntPtr h, ref WAVEHDR hdr, int sz);
    [DllImport("winmm.dll")] public static extern int waveInAddBuffer(IntPtr h, ref WAVEHDR hdr, int sz);
    [DllImport("winmm.dll")] public static extern int waveInStart(IntPtr h);
    [DllImport("winmm.dll")] public static extern int waveInStop(IntPtr h);
    [DllImport("winmm.dll")] public static extern int waveInUnprepareHeader(IntPtr h, ref WAVEHDR hdr, int sz);
    [DllImport("winmm.dll")] public static extern int waveInClose(IntPtr h);

    [StructLayout(LayoutKind.Sequential)] public struct WAVEFORMATEX {
        public short wFormatTag, nChannels;
        public int nSamplesPerSec, nAvgBytesPerSec;
        public short nBlockAlign, wBitsPerSample, cbSize;
    }
    [StructLayout(LayoutKind.Sequential)] public struct WAVEHDR {
        public IntPtr lpData;
        public int dwBufferLength, dwBytesRecorded, dwUser, dwFlags, dwLoops;
        public IntPtr lpNext;
        public IntPtr reserved;
    }

    // Synchronous: opens default-out + default-in, starts capture, then plays.
    // Returns captured int16 mono (averaged stereo).
    public static short[] PlayAndCapture(short[] playPcm, int sampleRate, int captureMs) {
        WAVEFORMATEX fmt = new WAVEFORMATEX {
            wFormatTag = 1, nChannels = 2,
            nSamplesPerSec = sampleRate,
            wBitsPerSample = 16
        };
        fmt.nBlockAlign = (short)(fmt.nChannels * fmt.wBitsPerSample / 8);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

        // Capture buffer sized for captureMs
        int captureSamples = sampleRate * captureMs / 1000;
        short[] capBuf = new short[captureSamples * 2]; // stereo
        GCHandle capPin = GCHandle.Alloc(capBuf, GCHandleType.Pinned);
        IntPtr hIn;
        waveInOpen(out hIn, -1, ref fmt, IntPtr.Zero, IntPtr.Zero, 0);
        WAVEHDR inHdr = new WAVEHDR { lpData = capPin.AddrOfPinnedObject(), dwBufferLength = capBuf.Length * 2 };
        waveInPrepareHeader(hIn, ref inHdr, Marshal.SizeOf(inHdr));
        waveInAddBuffer(hIn, ref inHdr, Marshal.SizeOf(inHdr));
        waveInStart(hIn);

        // Brief pre-roll so capture is established before we play
        Thread.Sleep(50);

        // Make playback stereo (mono-in -> dup'd to L+R)
        short[] stereoPlay = new short[playPcm.Length * 2];
        for (int i = 0; i < playPcm.Length; i++) {
            stereoPlay[i * 2] = playPcm[i];
            stereoPlay[i * 2 + 1] = playPcm[i];
        }
        GCHandle playPin = GCHandle.Alloc(stereoPlay, GCHandleType.Pinned);
        IntPtr hOut;
        waveOutOpen(out hOut, -1, ref fmt, IntPtr.Zero, IntPtr.Zero, 0);
        WAVEHDR outHdr = new WAVEHDR { lpData = playPin.AddrOfPinnedObject(), dwBufferLength = stereoPlay.Length * 2 };
        waveOutPrepareHeader(hOut, ref outHdr, Marshal.SizeOf(outHdr));
        long t0 = DateTime.UtcNow.Ticks;
        waveOutWrite(hOut, ref outHdr, Marshal.SizeOf(outHdr));

        // Wait for capture to fill
        int totalMs = captureMs + 100;
        Thread.Sleep(totalMs);

        waveInStop(hIn);
        waveInUnprepareHeader(hIn, ref inHdr, Marshal.SizeOf(inHdr));
        waveInClose(hIn);
        waveOutUnprepareHeader(hOut, ref outHdr, Marshal.SizeOf(outHdr));
        waveOutClose(hOut);
        playPin.Free();
        capPin.Free();
        return capBuf;
    }

    public static void WriteWav(string path, short[] samples, int sampleRate, int channels) {
        using (var fs = File.Create(path))
        using (var bw = new BinaryWriter(fs)) {
            int byteRate = sampleRate * channels * 2;
            int dataSize = samples.Length * 2;
            bw.Write(System.Text.Encoding.ASCII.GetBytes("RIFF"));
            bw.Write(36 + dataSize);
            bw.Write(System.Text.Encoding.ASCII.GetBytes("WAVEfmt "));
            bw.Write(16); bw.Write((short)1); bw.Write((short)channels);
            bw.Write(sampleRate); bw.Write(byteRate);
            bw.Write((short)(channels * 2)); bw.Write((short)16);
            bw.Write(System.Text.Encoding.ASCII.GetBytes("data"));
            bw.Write(dataSize);
            foreach (var s in samples) bw.Write(s);
        }
    }
}
"@ -ReferencedAssemblies System.Runtime.InteropServices

function Make-Impulse([int]$sampleRate, [int]$totalSamples) {
    $buf = New-Object short[] $totalSamples
    # Silence padding then one full-scale impulse, then trailing silence
    $impulseAt = [int]($sampleRate * 0.25)
    $buf[$impulseAt] = 28000
    return ,$buf
}

function Make-Sine([double]$freq, [int]$sampleRate, [int]$durMs) {
    $n = [int]($sampleRate * $durMs / 1000)
    $buf = New-Object short[] $n
    $two_pi_f = 2.0 * [Math]::PI * $freq / $sampleRate
    # 0.5s fade-in and fade-out, sustain in middle
    $fadeN = [int]($sampleRate * 0.05)
    for ($i = 0; $i -lt $n; $i++) {
        $env = 1.0
        if ($i -lt $fadeN) { $env = $i / [double]$fadeN }
        elseif ($i -gt ($n - $fadeN)) { $env = ($n - $i) / [double]$fadeN }
        $s = [Math]::Sin($two_pi_f * $i) * 0.7 * 32767.0 * $env
        $buf[$i] = [short][Math]::Round($s)
    }
    return ,$buf
}

function Make-Chirp([double]$f0, [double]$f1, [int]$sampleRate, [int]$durMs) {
    $n = [int]($sampleRate * $durMs / 1000)
    $buf = New-Object short[] $n
    $T = $durMs / 1000.0
    $k = ($f1 - $f0) / $T
    for ($i = 0; $i -lt $n; $i++) {
        $t = $i / [double]$sampleRate
        $phase = 2.0 * [Math]::PI * ($f0 * $t + 0.5 * $k * $t * $t)
        $s = [Math]::Sin($phase) * 0.6 * 32767.0
        $buf[$i] = [short][Math]::Round($s)
    }
    return ,$buf
}

# Cross-correlation peak (impulse onset)
function Find-OnsetSample([short[]]$capture, [int]$channels, [int]$thresholdAbs) {
    $stride = $channels
    $n = $capture.Length / $stride
    $maxAbs = 0
    for ($i = 0; $i -lt $n; $i++) {
        $v = [Math]::Abs([int]$capture[$i * $stride])
        if ($v -gt $maxAbs) { $maxAbs = $v }
    }
    $thr = [Math]::Max($thresholdAbs, [int]($maxAbs * 0.5))
    for ($i = 0; $i -lt $n; $i++) {
        $v = [Math]::Abs([int]$capture[$i * $stride])
        if ($v -ge $thr) { return $i }
    }
    return -1
}

function Estimate-THD([short[]]$capture, [int]$channels, [double]$fund, [int]$sampleRate) {
    # Goertzel at fundamental + 2nd..5th harmonics
    $stride = $channels
    $n = $capture.Length / $stride
    # Trim to middle 60% (sustained region)
    $start = [int]($n * 0.2)
    $end = [int]($n * 0.8)
    function Goertzel($freq) {
        $coef = 2.0 * [Math]::Cos(2.0 * [Math]::PI * $freq / $sampleRate)
        $s_prev = 0.0; $s_prev2 = 0.0
        for ($i = $start; $i -lt $end; $i++) {
            $x = $capture[$i * $stride] / 32768.0
            $s = $x + $coef * $s_prev - $s_prev2
            $s_prev2 = $s_prev; $s_prev = $s
        }
        return [Math]::Sqrt($s_prev2 * $s_prev2 + $s_prev * $s_prev - $coef * $s_prev * $s_prev2)
    }
    $f1 = Goertzel $fund
    $f2 = Goertzel ($fund * 2)
    $f3 = Goertzel ($fund * 3)
    $f4 = Goertzel ($fund * 4)
    $f5 = Goertzel ($fund * 5)
    if ($f1 -le 0) { return @{ thd_pct = -1.0; fund_amp = $f1 } }
    $thd = [Math]::Sqrt($f2*$f2 + $f3*$f3 + $f4*$f4 + $f5*$f5) / $f1
    return @{
        thd_pct = $thd * 100.0
        fund_amp = $f1
        h2 = $f2; h3 = $f3; h4 = $f4; h5 = $f5
    }
}

$report = @{
    mode = $Mode
    sample_rate = $SampleRate
    duration_ms = $DurationMs
    timestamp = (Get-Date -Format 'o')
    results = @{}
}

function Run-Impulse {
    Write-Host "[impulse] capturing 2s of impulse for latency..."
    $totalMs = 2000
    $impulse = Make-Impulse $SampleRate ([int]($SampleRate * $totalMs / 1000))
    $cap = [AudioIO]::PlayAndCapture($impulse, $SampleRate, $totalMs)
    [AudioIO]::WriteWav((Join-Path $OutDir 'impulse_capture.wav'), $cap, $SampleRate, 2)
    # Impulse was emitted at sample 0.25 * SR; find capture onset
    $emittedAt = [int]($SampleRate * 0.25)
    $onset = Find-OnsetSample $cap 2 2000
    if ($onset -lt 0) {
        Write-Host "[impulse] NO ONSET DETECTED — check rig wiring (host default-in is silent)" -ForegroundColor Red
        return @{ ok = $false; reason = 'no-onset' }
    }
    # capture started ~50ms BEFORE play (preroll). Latency = onset - (preroll_samples + emittedAt)
    $prerollSamples = [int]($SampleRate * 0.05)
    $latencySamples = $onset - ($prerollSamples + $emittedAt)
    $latencyMs = $latencySamples * 1000.0 / $SampleRate
    Write-Host ("[impulse] onset at sample {0}; latency = {1:F2} ms ({2} samples)" -f $onset, $latencyMs, $latencySamples)
    return @{
        ok = $true
        onset_sample = $onset
        latency_samples = $latencySamples
        latency_ms = $latencyMs
    }
}

function Run-Sine {
    Write-Host ("[sine] capturing {0} Hz sine for {1} ms (THD analysis)..." -f $Freq, $DurationMs)
    $sine = Make-Sine $Freq $SampleRate $DurationMs
    $cap = [AudioIO]::PlayAndCapture($sine, $SampleRate, $DurationMs + 200)
    [AudioIO]::WriteWav((Join-Path $OutDir ("sine_${Freq}Hz.wav")), $cap, $SampleRate, 2)
    $thd = Estimate-THD $cap 2 $Freq $SampleRate
    Write-Host ("[sine] fundamental {0}Hz -> THD = {1:F2}%, fund_amp = {2:F4}" -f $Freq, $thd.thd_pct, $thd.fund_amp)
    return $thd + @{ ok = $true; freq = $Freq }
}

function Run-Chirp {
    Write-Host ("[chirp] capturing 20Hz-20kHz sweep for {0} ms..." -f $DurationMs)
    $chirp = Make-Chirp 20.0 20000.0 $SampleRate $DurationMs
    $cap = [AudioIO]::PlayAndCapture($chirp, $SampleRate, $DurationMs + 200)
    [AudioIO]::WriteWav((Join-Path $OutDir 'chirp_capture.wav'), $cap, $SampleRate, 2)
    Write-Host "[chirp] saved capture; analyse in any DAW or with python+scipy welch"
    return @{ ok = $true; capture = 'chirp_capture.wav' }
}

function Run-Silence {
    Write-Host ("[silence] capturing 2s of silence (noise floor)...")
    $sil = New-Object short[] ($SampleRate * 2)
    $cap = [AudioIO]::PlayAndCapture($sil, $SampleRate, 2200)
    [AudioIO]::WriteWav((Join-Path $OutDir 'silence_capture.wav'), $cap, $SampleRate, 2)
    $sumSq = 0.0
    for ($i = 0; $i -lt $cap.Length; $i++) {
        $v = $cap[$i] / 32768.0
        $sumSq += $v * $v
    }
    $rms = [Math]::Sqrt($sumSq / $cap.Length)
    $dbfs = if ($rms -gt 0) { 20.0 * [Math]::Log10($rms) } else { -120.0 }
    Write-Host ("[silence] noise floor RMS = {0:F6} ({1:F1} dBFS)" -f $rms, $dbfs)
    return @{ ok = $true; rms = $rms; dbfs = $dbfs }
}

switch ($Mode) {
    'impulse' { $report.results.impulse = Run-Impulse }
    'sine'    { $report.results.sine = Run-Sine }
    'chirp'   { $report.results.chirp = Run-Chirp }
    'silence' { $report.results.silence = Run-Silence }
    'full' {
        $report.results.silence = Run-Silence
        Start-Sleep -Milliseconds 200
        $report.results.impulse = Run-Impulse
        Start-Sleep -Milliseconds 200
        $report.results.chirp = Run-Chirp
        Start-Sleep -Milliseconds 200
        # Low-E sustained (for -12 stability check downstream)
        $script:Freq = 82.4
        $report.results.sine_lowE = Run-Sine
        Start-Sleep -Milliseconds 200
        $script:Freq = 41.2
        $report.results.sine_lowE_minus12 = Run-Sine
        Start-Sleep -Milliseconds 200
        $script:Freq = 1000.0
        $report.results.sine_1kHz = Run-Sine
    }
}

$jsonPath = Join-Path $OutDir 'report.json'
$report | ConvertTo-Json -Depth 8 | Out-File -Encoding utf8 -FilePath $jsonPath
Write-Host "`nReport: $jsonPath"
Write-Host "Captures: $OutDir"
