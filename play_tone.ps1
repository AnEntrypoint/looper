
Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
public class WavePlayer {
    [DllImport("winmm.dll")] public static extern int waveOutOpen(out IntPtr h, int dev, ref WAVEFORMATEX fmt, IntPtr cb, IntPtr inst, int flags);
    [DllImport("winmm.dll")] public static extern int waveOutPrepareHeader(IntPtr h, ref WAVEHDR hdr, int sz);
    [DllImport("winmm.dll")] public static extern int waveOutWrite(IntPtr h, ref WAVEHDR hdr, int sz);
    [DllImport("winmm.dll")] public static extern int waveOutUnprepareHeader(IntPtr h, ref WAVEHDR hdr, int sz);
    [DllImport("winmm.dll")] public static extern int waveOutClose(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct WAVEFORMATEX { public short wFormatTag, nChannels; public int nSamplesPerSec, nAvgBytesPerSec; public short nBlockAlign, wBitsPerSample, cbSize; }
    [StructLayout(LayoutKind.Sequential)] public struct WAVEHDR { public IntPtr lpData; public int dwBufferLength, dwBytesRecorded, dwUser, dwFlags, dwLoops; public IntPtr lpNext; public IntPtr reserved; }
    public static void Play(int deviceId, string wavFile) {
        byte[] wav = File.ReadAllBytes(wavFile);
        int dataOffset = 12;
        while (dataOffset < wav.Length - 8) {
            string id = System.Text.Encoding.ASCII.GetString(wav, dataOffset, 4);
            int chunkSize = BitConverter.ToInt32(wav, dataOffset + 4);
            if (id == "data") { dataOffset += 8; break; }
            dataOffset += 8 + chunkSize;
        }
        byte[] pcm = new byte[wav.Length - dataOffset];
        Buffer.BlockCopy(wav, dataOffset, pcm, 0, pcm.Length);
        WAVEFORMATEX fmt = new WAVEFORMATEX { wFormatTag=1, nChannels=BitConverter.ToInt16(wav,22), nSamplesPerSec=BitConverter.ToInt32(wav,24), wBitsPerSample=BitConverter.ToInt16(wav,34) };
        fmt.nBlockAlign=(short)(fmt.nChannels*fmt.wBitsPerSample/8); fmt.nAvgBytesPerSec=fmt.nSamplesPerSec*fmt.nBlockAlign;
        IntPtr hWave; waveOutOpen(out hWave, deviceId, ref fmt, IntPtr.Zero, IntPtr.Zero, 0);
        GCHandle pin = GCHandle.Alloc(pcm, GCHandleType.Pinned);
        WAVEHDR hdr = new WAVEHDR { lpData=pin.AddrOfPinnedObject(), dwBufferLength=pcm.Length };
        waveOutPrepareHeader(hWave, ref hdr, System.Runtime.InteropServices.Marshal.SizeOf(hdr));
        waveOutWrite(hWave, ref hdr, System.Runtime.InteropServices.Marshal.SizeOf(hdr));
        double dur = (double)pcm.Length / fmt.nAvgBytesPerSec;
        Console.WriteLine("Playing " + dur.ToString("F1") + "s");
        Thread.Sleep((int)(dur*1000)+500);
        waveOutUnprepareHeader(hWave, ref hdr, System.Runtime.InteropServices.Marshal.SizeOf(hdr));
        pin.Free(); waveOutClose(hWave);
        Console.WriteLine("Done");
    }
}
"@ -Language CSharp
[WavePlayer]::Play(0, 'C:/dev/looper/tone440.wav')
