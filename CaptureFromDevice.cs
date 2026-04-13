using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

class CaptureFromDevice {
    [DllImport("winmm.dll")] static extern int waveInGetNumDevs();
    [DllImport("winmm.dll", CharSet=CharSet.Auto)]
    static extern int waveInGetDevCaps(int uDeviceID, ref WAVEINCAPS pwic, int cbwic);
    [DllImport("winmm.dll")] static extern int waveInOpen(out IntPtr hWaveIn, int uDeviceID, ref WAVEFORMATEX lpFormat, WaveInProc dwCallback, IntPtr dwInstance, int fdwOpen);
    [DllImport("winmm.dll")] static extern int waveInPrepareHeader(IntPtr hWaveIn, ref WAVEHDR lpWaveInHdr, int uSize);
    [DllImport("winmm.dll")] static extern int waveInAddBuffer(IntPtr hWaveIn, ref WAVEHDR lpWaveInHdr, int uSize);
    [DllImport("winmm.dll")] static extern int waveInStart(IntPtr hWaveIn);
    [DllImport("winmm.dll")] static extern int waveInStop(IntPtr hWaveIn);
    [DllImport("winmm.dll")] static extern int waveInUnprepareHeader(IntPtr hWaveIn, ref WAVEHDR lpWaveInHdr, int uSize);
    [DllImport("winmm.dll")] static extern int waveInClose(IntPtr hWaveIn);

    delegate void WaveInProc(IntPtr hWaveIn, int uMsg, IntPtr dwInstance, ref WAVEHDR dwParam1, IntPtr dwParam2);

    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Auto)]
    struct WAVEINCAPS { public short wMid, wPid; public int vDriverVersion; [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string szPname; public int dwFormats, wChannels; }
    [StructLayout(LayoutKind.Sequential)]
    struct WAVEFORMATEX { public short wFormatTag, nChannels; public int nSamplesPerSec, nAvgBytesPerSec; public short nBlockAlign, wBitsPerSample, cbSize; }
    [StructLayout(LayoutKind.Sequential)]
    struct WAVEHDR { public IntPtr lpData; public int dwBufferLength, dwBytesRecorded, dwUser, dwFlags, dwLoops; public IntPtr lpNext; public IntPtr reserved; }

    const int CALLBACK_FUNCTION = 0x30000;
    const int WIM_DATA = 0x3C0;

    static int sampleRate = 48000;
    static int channels = 2;
    static int bits = 16;
    static int durationSec = 5;
    static string outFile = "otg_capture.wav";
    static MemoryStream captured = new MemoryStream();
    static volatile int buffersReturned = 0;
    static int totalBuffers = 0;

    static void Main(string[] args) {
        if (args.Length > 0 && args[0] == "--list") {
            int n = waveInGetNumDevs();
            Console.WriteLine("WaveIn devices: " + n);
            for (int i = 0; i < n; i++) {
                var caps = new WAVEINCAPS();
                waveInGetDevCaps(i, ref caps, Marshal.SizeOf(caps));
                Console.WriteLine("  [" + i + "] " + caps.szPname);
            }
            return;
        }

        int deviceId = args.Length > 0 ? int.Parse(args[0]) : 0;
        if (args.Length > 1) durationSec = int.Parse(args[1]);
        if (args.Length > 2) outFile = args[2];

        int bytesPerSec = sampleRate * channels * bits / 8;
        int bufSize = bytesPerSec / 10; // 100ms chunks
        totalBuffers = durationSec * 10;

        WAVEFORMATEX fmt = new WAVEFORMATEX {
            wFormatTag = 1, nChannels = (short)channels,
            nSamplesPerSec = sampleRate, wBitsPerSample = (short)bits,
            nBlockAlign = (short)(channels * bits / 8),
            nAvgBytesPerSec = bytesPerSec
        };

        IntPtr hWaveIn;
        WaveInProc cb = OnBuffer;
        int r = waveInOpen(out hWaveIn, deviceId, ref fmt, cb, IntPtr.Zero, CALLBACK_FUNCTION);
        if (r != 0) { Console.Error.WriteLine("waveInOpen failed: " + r + " (try --list to see devices)"); return; }

        Console.WriteLine("Recording " + durationSec + "s from device " + deviceId + " -> " + outFile);

        var bufs = new byte[2][];
        var hdrs = new WAVEHDR[2];
        var handles = new GCHandle[2];
        for (int i = 0; i < 2; i++) {
            bufs[i] = new byte[bufSize];
            handles[i] = GCHandle.Alloc(bufs[i], GCHandleType.Pinned);
            hdrs[i] = new WAVEHDR { lpData = handles[i].AddrOfPinnedObject(), dwBufferLength = bufSize };
            waveInPrepareHeader(hWaveIn, ref hdrs[i], Marshal.SizeOf(hdrs[i]));
            waveInAddBuffer(hWaveIn, ref hdrs[i], Marshal.SizeOf(hdrs[i]));
        }

        waveInStart(hWaveIn);
        Thread.Sleep(durationSec * 1000 + 500);
        waveInStop(hWaveIn);

        for (int i = 0; i < 2; i++) {
            waveInUnprepareHeader(hWaveIn, ref hdrs[i], Marshal.SizeOf(hdrs[i]));
            handles[i].Free();
        }
        waveInClose(hWaveIn);

        byte[] pcm = captured.ToArray();
        Console.WriteLine("Captured " + pcm.Length + " bytes = " + ((double)pcm.Length / bytesPerSec).ToString("F2") + "s");
        WriteWav(outFile, pcm, sampleRate, channels, bits);
        Console.WriteLine("Saved to " + outFile);
    }

    static void OnBuffer(IntPtr hWaveIn, int uMsg, IntPtr dwInstance, ref WAVEHDR hdr, IntPtr dwParam2) {
        if (uMsg != WIM_DATA) return;
        if (hdr.dwBytesRecorded > 0) {
            byte[] chunk = new byte[hdr.dwBytesRecorded];
            Marshal.Copy(hdr.lpData, chunk, 0, hdr.dwBytesRecorded);
            lock (captured) captured.Write(chunk, 0, chunk.Length);
        }
        buffersReturned++;
        if (buffersReturned < totalBuffers) {
            hdr.dwFlags = 0;
            hdr.dwBytesRecorded = 0;
            waveInPrepareHeader(hWaveIn, ref hdr, Marshal.SizeOf(hdr));
            waveInAddBuffer(hWaveIn, ref hdr, Marshal.SizeOf(hdr));
        }
    }

    static void WriteWav(string path, byte[] pcm, int sr, int ch, int bits) {
        using var f = new FileStream(path, FileMode.Create);
        using var w = new BinaryWriter(f);
        int byteRate = sr * ch * bits / 8;
        short blockAlign = (short)(ch * bits / 8);
        w.Write(System.Text.Encoding.ASCII.GetBytes("RIFF"));
        w.Write(36 + pcm.Length);
        w.Write(System.Text.Encoding.ASCII.GetBytes("WAVE"));
        w.Write(System.Text.Encoding.ASCII.GetBytes("fmt "));
        w.Write(16); w.Write((short)1); w.Write((short)ch);
        w.Write(sr); w.Write(byteRate); w.Write(blockAlign); w.Write((short)bits);
        w.Write(System.Text.Encoding.ASCII.GetBytes("data"));
        w.Write(pcm.Length);
        w.Write(pcm);
    }
}
