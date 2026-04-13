
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class WaveOut {
    [DllImport("winmm.dll")] public static extern int waveOutGetNumDevs();
    [DllImport("winmm.dll", CharSet=CharSet.Auto)] 
    public static extern int waveOutGetDevCaps(int uDeviceID, ref WAVEOUTCAPS pwoc, int cbwoc);
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Auto)]
    public struct WAVEOUTCAPS {
        public short wMid, wPid;
        public int vDriverVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string szPname;
        public int dwFormats, wChannels, wReserved1, dwSupport;
    }
}
"@
$n = [WaveOut]::waveOutGetNumDevs()
Write-Host "WaveOut devices: $n"
for ($i = -1; $i -lt $n; $i++) {
    $caps = New-Object WaveOut+WAVEOUTCAPS
    [WaveOut]::waveOutGetDevCaps($i, [ref]$caps, [Runtime.InteropServices.Marshal]::SizeOf($caps)) | Out-Null
    Write-Host "  [$i] $($caps.szPname)"
}
