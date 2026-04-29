
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

class PlayToDevice {
    [DllImport("winmm.dll")] static extern int waveOutOpen(out IntPtr hWaveOut, int uDeviceID, ref WAVEFORMATEX lpFormat, IntPtr dwCallback, IntPtr dwInstance, int fdwOpen);
    [DllImport("winmm.dll")] static extern int waveOutPrepareHeader(IntPtr hWaveOut, ref WAVEHDR lpWaveOutHdr, int uSize);
    [DllImport("winmm.dll")] static extern int waveOutWrite(IntPtr hWaveOut, ref WAVEHDR lpWaveOutHdr, int uSize);
    [DllImport("winmm.dll")] static extern int waveOutUnprepareHeader(IntPtr hWaveOut, ref WAVEHDR lpWaveOutHdr, int uSize);
    [DllImport("winmm.dll")] static extern int waveOutClose(IntPtr hWaveOut);
    [DllImport("winmm.dll")] static extern int waveOutGetPosition(IntPtr hWaveOut, ref MMTIME pmmt, int cbmmt);

    [StructLayout(LayoutKind.Sequential)]
    struct WAVEFORMATEX { public short wFormatTag, nChannels; public int nSamplesPerSec, nAvgBytesPerSec; public short nBlockAlign, wBitsPerSample, cbSize; }
    [StructLayout(LayoutKind.Sequential)]
    struct WAVEHDR { public IntPtr lpData; public int dwBufferLength, dwBytesRecorded, dwUser, dwFlags, dwLoops; public IntPtr lpNext; public IntPtr reserved; }
    [StructLayout(LayoutKind.Sequential)]
    struct MMTIME { public int wType; public int u; }

    static void Main(string[] args) {
        int deviceId = int.Parse(args[0]);
        string wavFile = args[1];
        
        // Read WAV file (skip 44-byte header for standard PCM WAV)
        byte[] wav = File.ReadAllBytes(wavFile);
        // Find 'data' chunk
        int dataOffset = 12;
        while (dataOffset < wav.Length - 8) {
            string id = System.Text.Encoding.ASCII.GetString(wav, dataOffset, 4);
            int chunkSize = BitConverter.ToInt32(wav, dataOffset + 4);
            if (id == "data") { dataOffset += 8; break; }
            dataOffset += 8 + chunkSize;
        }
        byte[] pcm = new byte[wav.Length - dataOffset];
        Buffer.BlockCopy(wav, dataOffset, pcm, 0, pcm.Length);
        
        // WAV format from header
        short channels = BitConverter.ToInt16(wav, 22);
        int sampleRate = BitConverter.ToInt32(wav, 24);
        short bitsPerSample = BitConverter.ToInt16(wav, 34);
        
        WAVEFORMATEX fmt = new WAVEFORMATEX {
            wFormatTag = 1, nChannels = channels,
            nSamplesPerSec = sampleRate, wBitsPerSample = bitsPerSample,
            nBlockAlign = (short)(channels * bitsPerSample / 8),
            nAvgBytesPerSec = sampleRate * channels * bitsPerSample / 8
        };
        
        IntPtr hWave;
        int r = waveOutOpen(out hWave, deviceId, ref fmt, IntPtr.Zero, IntPtr.Zero, 0);
        if (r != 0) { Console.Error.WriteLine("waveOutOpen failed: " + r); return; }
        
        GCHandle handle = GCHandle.Alloc(pcm, GCHandleType.Pinned);
        WAVEHDR hdr = new WAVEHDR { lpData = handle.AddrOfPinnedObject(), dwBufferLength = pcm.Length };
        waveOutPrepareHeader(hWave, ref hdr, Marshal.SizeOf(hdr));
        waveOutWrite(hWave, ref hdr, Marshal.SizeOf(hdr));
        
        // Wait for playback to finish
        double dur = (double)pcm.Length / fmt.nAvgBytesPerSec;
        Console.WriteLine("Playing " + dur.ToString("F1") + "s to device " + deviceId);
        Thread.Sleep((int)(dur * 1000) + 500);
        
        waveOutUnprepareHeader(hWave, ref hdr, Marshal.SizeOf(hdr));
        handle.Free();
        waveOutClose(hWave);
        Console.WriteLine("Done");
    }
}
