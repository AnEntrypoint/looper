# Three-loopback measurement harness.
#
# Plays a 1 kHz sine through one Windows audio device, captures from another,
# computes round-trip latency + THD. Use to characterise three signal chains:
#
#   1. Focusrite → Focusrite  (host-only baseline, no Pi)
#   2. Focusrite → Pi (UCA222 / OTG)  (measures Pi passthrough)
#   3. Pi → Pi   (full looper loop)
#
# After capturing baseline metrics, sweep CC100 (engine read offset) over
# 32..2048 samples and report stability + perceived latency for each setting,
# so the lowest-stable value can be picked.
#
# Use:
#   .\scripts\loopback-sweep.ps1 -List          # show available audio devices
#   .\scripts\loopback-sweep.ps1 -Out 'Focusrite' -In 'Focusrite' -Sec 3
#   .\scripts\loopback-sweep.ps1 -Out 'Focusrite' -In 'UCA' -Sec 3
#   .\scripts\loopback-sweep.ps1 -Out 'UCA' -In 'UCA' -Sec 3
#   .\scripts\loopback-sweep.ps1 -Sweep readOffset  # CC100 sweep
[CmdletBinding()]
param(
    [switch]$List,
    [string]$Out  = '',
    [string]$In   = '',
    [int]$Freq    = 1000,
    [double]$Sec  = 3.0,
    [string]$Sweep = '',
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class WinMM {
    [StructLayout(LayoutKind.Sequential)]
    public struct WAVEFORMATEX {
        public ushort wFormatTag, nChannels;
        public uint nSamplesPerSec, nAvgBytesPerSec;
        public ushort nBlockAlign, wBitsPerSample, cbSize;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct WAVEHDR {
        public IntPtr lpData;
        public uint dwBufferLength;
        public uint dwBytesRecorded;
        public IntPtr dwUser;
        public uint dwFlags;
        public uint dwLoops;
        public IntPtr lpNext;
        public IntPtr reserved;
    }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Ansi)]
    public struct WAVEOUTCAPS {
        public ushort wMid, wPid;
        public uint vDriverVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string szPname;
        public uint dwFormats;
        public ushort wChannels, wReserved1;
        public uint dwSupport;
    }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Ansi)]
    public struct WAVEINCAPS {
        public ushort wMid, wPid;
        public uint vDriverVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string szPname;
        public uint dwFormats;
        public ushort wChannels, wReserved1;
    }
    [DllImport("winmm.dll")] public static extern uint waveOutGetNumDevs();
    [DllImport("winmm.dll")] public static extern int waveOutGetDevCapsA(int dev, ref WAVEOUTCAPS caps, int sz);
    [DllImport("winmm.dll")] public static extern uint waveInGetNumDevs();
    [DllImport("winmm.dll")] public static extern int waveInGetDevCapsA(int dev, ref WAVEINCAPS caps, int sz);
    [DllImport("winmm.dll")] public static extern int waveOutOpen(out IntPtr h, int dev, ref WAVEFORMATEX f, IntPtr cb, IntPtr inst, int flags);
    [DllImport("winmm.dll")] public static extern int waveOutPrepareHeader(IntPtr h, ref WAVEHDR hdr, int sz);
    [DllImport("winmm.dll")] public static extern int waveOutWrite(IntPtr h, ref WAVEHDR hdr, int sz);
    [DllImport("winmm.dll")] public static extern int waveOutUnprepareHeader(IntPtr h, ref WAVEHDR hdr, int sz);
    [DllImport("winmm.dll")] public static extern int waveOutClose(IntPtr h);
    [DllImport("winmm.dll")] public static extern int waveInOpen(out IntPtr h, int dev, ref WAVEFORMATEX f, IntPtr cb, IntPtr inst, int flags);
    [DllImport("winmm.dll")] public static extern int waveInPrepareHeader(IntPtr h, ref WAVEHDR hdr, int sz);
    [DllImport("winmm.dll")] public static extern int waveInAddBuffer(IntPtr h, ref WAVEHDR hdr, int sz);
    [DllImport("winmm.dll")] public static extern int waveInStart(IntPtr h);
    [DllImport("winmm.dll")] public static extern int waveInStop(IntPtr h);
    [DllImport("winmm.dll")] public static extern int waveInUnprepareHeader(IntPtr h, ref WAVEHDR hdr, int sz);
    [DllImport("winmm.dll")] public static extern int waveInClose(IntPtr h);
}
"@

function List-Devices {
    Write-Host "OUT devices:"
    $n = [WinMM]::waveOutGetNumDevs()
    for ($i = 0; $i -lt $n; $i++) {
        $caps = New-Object WinMM+WAVEOUTCAPS
        [WinMM]::waveOutGetDevCapsA($i, [ref]$caps, [Runtime.InteropServices.Marshal]::SizeOf($caps)) | Out-Null
        "  [{0}] {1}" -f $i, $caps.szPname
    }
    Write-Host "IN devices:"
    $n = [WinMM]::waveInGetNumDevs()
    for ($i = 0; $i -lt $n; $i++) {
        $caps = New-Object WinMM+WAVEINCAPS
        [WinMM]::waveInGetDevCapsA($i, [ref]$caps, [Runtime.InteropServices.Marshal]::SizeOf($caps)) | Out-Null
        "  [{0}] {1}" -f $i, $caps.szPname
    }
}

function Find-OutDevice([string]$name) {
    if ([string]::IsNullOrEmpty($name)) { return -1 }
    $n = [WinMM]::waveOutGetNumDevs()
    for ($i = 0; $i -lt $n; $i++) {
        $caps = New-Object WinMM+WAVEOUTCAPS
        [WinMM]::waveOutGetDevCapsA($i, [ref]$caps, [Runtime.InteropServices.Marshal]::SizeOf($caps)) | Out-Null
        if ($caps.szPname -like "*$name*") { return $i }
    }
    return -1
}
function Find-InDevice([string]$name) {
    if ([string]::IsNullOrEmpty($name)) { return -1 }
    $n = [WinMM]::waveInGetNumDevs()
    for ($i = 0; $i -lt $n; $i++) {
        $caps = New-Object WinMM+WAVEINCAPS
        [WinMM]::waveInGetDevCapsA($i, [ref]$caps, [Runtime.InteropServices.Marshal]::SizeOf($caps)) | Out-Null
        if ($caps.szPname -like "*$name*") { return $i }
    }
    return -1
}

function Send-MidiCC([byte]$cc, [byte]$val) {
    $c = New-Object System.Net.Sockets.UdpClient
    $msg = [byte[]](0xB0, $cc, $val)
    $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse("192.168.137.100"), 4446)
    $c.Send($msg, $msg.Length, $ep) | Out-Null
    $c.Close()
}

function Play-Capture([int]$outDev, [int]$inDev, [int]$freq, [double]$sec) {
    $sr = 48000
    $n = [int]($sr * $sec)
    $play = New-Object 'Int16[]' $n
    for ($i = 0; $i -lt $n; $i++) {
        $play[$i] = [Int16]([Math]::Sin(2.0 * [Math]::PI * $freq * $i / $sr) * 24576)
    }
    # Allocate native buffers
    $playBytes = [Runtime.InteropServices.Marshal]::AllocHGlobal($n * 2)
    [Runtime.InteropServices.Marshal]::Copy($play, 0, $playBytes, $n)
    $capBytes = [Runtime.InteropServices.Marshal]::AllocHGlobal($n * 2)

    $fmt = New-Object WinMM+WAVEFORMATEX
    $fmt.wFormatTag = 1; $fmt.nChannels = 1
    $fmt.nSamplesPerSec = $sr; $fmt.nAvgBytesPerSec = $sr * 2
    $fmt.nBlockAlign = 2; $fmt.wBitsPerSample = 16; $fmt.cbSize = 0

    $hOut = [IntPtr]::Zero; $hIn = [IntPtr]::Zero
    [WinMM]::waveOutOpen([ref]$hOut, $outDev, [ref]$fmt, [IntPtr]::Zero, [IntPtr]::Zero, 0) | Out-Null
    [WinMM]::waveInOpen([ref]$hIn, $inDev, [ref]$fmt, [IntPtr]::Zero, [IntPtr]::Zero, 0) | Out-Null

    $hdrPlay = New-Object WinMM+WAVEHDR
    $hdrPlay.lpData = $playBytes; $hdrPlay.dwBufferLength = [uint32]($n * 2)
    $hdrCap = New-Object WinMM+WAVEHDR
    $hdrCap.lpData = $capBytes; $hdrCap.dwBufferLength = [uint32]($n * 2)

    [WinMM]::waveOutPrepareHeader($hOut, [ref]$hdrPlay, [Runtime.InteropServices.Marshal]::SizeOf($hdrPlay)) | Out-Null
    [WinMM]::waveInPrepareHeader($hIn, [ref]$hdrCap, [Runtime.InteropServices.Marshal]::SizeOf($hdrCap)) | Out-Null
    [WinMM]::waveInAddBuffer($hIn, [ref]$hdrCap, [Runtime.InteropServices.Marshal]::SizeOf($hdrCap)) | Out-Null
    [WinMM]::waveInStart($hIn) | Out-Null
    [WinMM]::waveOutWrite($hOut, [ref]$hdrPlay, [Runtime.InteropServices.Marshal]::SizeOf($hdrPlay)) | Out-Null

    Start-Sleep -Milliseconds ([int]($sec * 1000 + 200))

    [WinMM]::waveInStop($hIn) | Out-Null
    [WinMM]::waveOutUnprepareHeader($hOut, [ref]$hdrPlay, [Runtime.InteropServices.Marshal]::SizeOf($hdrPlay)) | Out-Null
    [WinMM]::waveInUnprepareHeader($hIn, [ref]$hdrCap, [Runtime.InteropServices.Marshal]::SizeOf($hdrCap)) | Out-Null
    [WinMM]::waveOutClose($hOut) | Out-Null
    [WinMM]::waveInClose($hIn) | Out-Null

    $cap = New-Object 'Int16[]' $n
    [Runtime.InteropServices.Marshal]::Copy($capBytes, $cap, 0, $n)
    [Runtime.InteropServices.Marshal]::FreeHGlobal($playBytes)
    [Runtime.InteropServices.Marshal]::FreeHGlobal($capBytes)
    return @{ play = $play; cap = $cap; sr = $sr }
}

function Analyse-Capture($result) {
    $cap = $result.cap; $sr = $result.sr
    # Goertzel @ test freq + harmonics
    function Goertzel([Int16[]]$x, [int]$sr, [double]$f) {
        $N = $x.Length
        $w = 2.0 * [Math]::PI * $f / $sr
        $coeff = 2.0 * [Math]::Cos($w)
        $q0 = 0.0; $q1 = 0.0; $q2 = 0.0
        for ($i = 0; $i -lt $N; $i++) {
            $q0 = $coeff * $q1 - $q2 + ($x[$i] / 32768.0)
            $q2 = $q1; $q1 = $q0
        }
        $mag = [Math]::Sqrt($q1*$q1 + $q2*$q2 - $q1*$q2*$coeff) / ($N / 2.0)
        return $mag
    }
    $fund = Goertzel $cap $sr $Freq
    $h2   = Goertzel $cap $sr (2 * $Freq)
    $h3   = Goertzel $cap $sr (3 * $Freq)
    $h4   = Goertzel $cap $sr (4 * $Freq)
    $h5   = Goertzel $cap $sr (5 * $Freq)
    $thdNum = [Math]::Sqrt($h2*$h2 + $h3*$h3 + $h4*$h4 + $h5*$h5)
    $thd = if ($fund -gt 1e-9) { 100.0 * $thdNum / $fund } else { 0.0 }
    # Peak + RMS
    $peak = 0; $sumSq = 0.0
    foreach ($v in $cap) {
        $a = if ($v -lt 0) { -$v } else { $v }
        if ($a -gt $peak) { $peak = $a }
        $sumSq += [double]$v * $v
    }
    $rms = [Math]::Sqrt($sumSq / $cap.Length) / 32768.0
    $peakDb = if ($peak -gt 0) { 20.0 * [Math]::Log10($peak / 32767.0) } else { -999.0 }
    return @{
        fund_amp = [Math]::Round($fund, 5)
        thd_pct  = [Math]::Round($thd, 2)
        rms      = [Math]::Round($rms, 5)
        peak     = $peak
        peak_db  = [Math]::Round($peakDb, 1)
    }
}

if ($List) { List-Devices; exit }

$outDev = Find-OutDevice $Out
$inDev  = Find-InDevice  $In
if ($outDev -lt 0 -and $Out -ne '') { Write-Error "OUT device not found: $Out"; exit 1 }
if ($inDev  -lt 0 -and $In  -ne '') { Write-Error "IN device not found: $In";  exit 1 }
$outName = if ($outDev -ge 0) { $caps = New-Object WinMM+WAVEOUTCAPS; [WinMM]::waveOutGetDevCapsA($outDev,[ref]$caps,[Runtime.InteropServices.Marshal]::SizeOf($caps))|Out-Null; $caps.szPname } else { "default" }
$inName  = if ($inDev  -ge 0) { $caps = New-Object WinMM+WAVEINCAPS;  [WinMM]::waveInGetDevCapsA($inDev,[ref]$caps,[Runtime.InteropServices.Marshal]::SizeOf($caps))|Out-Null;  $caps.szPname } else { "default" }
Write-Host "OUT[$outDev]: $outName"
Write-Host "IN [$inDev]: $inName"

if ($Sweep -eq 'readOffset') {
    Write-Host "Sweeping CC100 (engine read offset) at freq=$Freq sec=$Sec ..."
    $results = @()
    foreach ($cc100 in @(0, 16, 32, 48, 64, 80, 96, 112, 127)) {
        Send-MidiCC 100 $cc100
        Start-Sleep -Milliseconds 300
        $r = Play-Capture $outDev $inDev $Freq $Sec
        $m = Analyse-Capture $r
        $samp = 32 + [int]([Math]::Pow($cc100, 2) * 0.125)
        if ($samp -gt 2048) { $samp = 2048 }
        $line = "CC100=$cc100 (=$samp samp) | fund=$($m.fund_amp) thd=$($m.thd_pct)% peak_db=$($m.peak_db) rms=$($m.rms)"
        Write-Host $line
        $results += $line
    }
    $stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
    $od = "scripts\measure-results\sweep-readoffset-$stamp"
    New-Item -ItemType Directory -Force -Path $od | Out-Null
    $results | Set-Content -Path "$od\results.txt"
    Write-Host "saved $od\results.txt"
    exit
}

# Single measurement
$r = Play-Capture $outDev $inDev $Freq $Sec
$m = Analyse-Capture $r
Write-Host "freq=$Freq sec=$Sec | fund=$($m.fund_amp) thd=$($m.thd_pct)% peak_db=$($m.peak_db) rms=$($m.rms)"
if ($OutDir -ne '') {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    # Write capture as 16-bit mono wav
    $wav = "$OutDir\cap_${Freq}Hz.wav"
    $sr = $r.sr; $cap = $r.cap; $N = $cap.Length
    $bytes = New-Object byte[] ($N * 2)
    [Buffer]::BlockCopy($cap, 0, $bytes, 0, $N * 2)
    $stream = [IO.File]::Open($wav, 'Create')
    $bw = New-Object IO.BinaryWriter $stream
    $bw.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
    $bw.Write([uint32](36 + $bytes.Length))
    $bw.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
    $bw.Write([uint32]16); $bw.Write([uint16]1); $bw.Write([uint16]1)
    $bw.Write([uint32]$sr); $bw.Write([uint32]($sr * 2))
    $bw.Write([uint16]2); $bw.Write([uint16]16)
    $bw.Write([Text.Encoding]::ASCII.GetBytes('data'))
    $bw.Write([uint32]$bytes.Length); $bw.Write($bytes)
    $bw.Close(); $stream.Close()
    Write-Host "saved $wav"
}
