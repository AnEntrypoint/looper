// Three-loopback measurement CLI — replaces fragile PowerShell P/Invoke.
//
// Plays a sine through a chosen Win32 wave-out device, captures from
// a chosen wave-in device, computes Goertzel @ fund + harmonics 2-5 → THD.
//
// Build:
//   g++ -std=c++17 -O2 -o scripts/loopback-cli.exe scripts/loopback-cli.cpp -lwinmm
//
// Use:
//   loopback-cli.exe --list
//   loopback-cli.exe --out "Focusrite" --in "Focusrite" --freq 1000 --sec 2
//   loopback-cli.exe --out "Focusrite" --in "Looper"    --freq 1000 --sec 2
//   loopback-cli.exe --out "Looper"    --in "Looper"    --freq 1000 --sec 2

#include <windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

static int findOut(const char* needle) {
    UINT n = waveOutGetNumDevs();
    for (UINT i = 0; i < n; i++) {
        WAVEOUTCAPSA caps{};
        if (waveOutGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            if (strstr(caps.szPname, needle)) return (int)i;
        }
    }
    return -1;
}
static int findIn(const char* needle) {
    UINT n = waveInGetNumDevs();
    for (UINT i = 0; i < n; i++) {
        WAVEINCAPSA caps{};
        if (waveInGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            if (strstr(caps.szPname, needle)) return (int)i;
        }
    }
    return -1;
}
static void listDevices() {
    UINT no = waveOutGetNumDevs();
    printf("OUT devices:\n");
    for (UINT i = 0; i < no; i++) {
        WAVEOUTCAPSA c{};
        waveOutGetDevCapsA(i, &c, sizeof(c));
        printf("  [%u] %s\n", i, c.szPname);
    }
    UINT ni = waveInGetNumDevs();
    printf("IN devices:\n");
    for (UINT i = 0; i < ni; i++) {
        WAVEINCAPSA c{};
        waveInGetDevCapsA(i, &c, sizeof(c));
        printf("  [%u] %s\n", i, c.szPname);
    }
}
static double goertzel(const int16_t* x, int N, int sr, double f) {
    double w = 2.0 * 3.14159265358979 * f / sr;
    double coeff = 2.0 * std::cos(w);
    double q1 = 0, q2 = 0;
    for (int i = 0; i < N; i++) {
        double q0 = coeff * q1 - q2 + (x[i] / 32768.0);
        q2 = q1; q1 = q0;
    }
    double mag = std::sqrt(q1*q1 + q2*q2 - q1*q2*coeff) / (N / 2.0);
    return mag;
}
static void writeWav(const char* path, const std::vector<int16_t>& s, int sr) {
    FILE* f = fopen(path, "wb"); if (!f) return;
    uint32_t bytes = (uint32_t)(s.size() * 2);
    fwrite("RIFF", 4, 1, f);
    uint32_t sz = 36 + bytes; fwrite(&sz, 4, 1, f);
    fwrite("WAVEfmt ", 8, 1, f);
    uint32_t fs = 16; fwrite(&fs, 4, 1, f);
    uint16_t fmt = 1, ch = 1, bps = 16, ba = 2; uint32_t br = sr * 2;
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    uint32_t srOut = (uint32_t)sr; fwrite(&srOut, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&ba, 2, 1, f); fwrite(&bps, 2, 1, f);
    fwrite("data", 4, 1, f); fwrite(&bytes, 4, 1, f);
    fwrite(s.data(), 2, s.size(), f);
    fclose(f);
}
int main(int argc, char** argv) {
    bool list = false;
    const char* outName = "";
    const char* inName  = "";
    int freq = 1000;
    double sec = 2.0;
    const char* outDir = "";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) list = true;
        else if (!strcmp(argv[i], "--out") && i+1 < argc) outName = argv[++i];
        else if (!strcmp(argv[i], "--in")  && i+1 < argc) inName  = argv[++i];
        else if (!strcmp(argv[i], "--freq") && i+1 < argc) freq = std::atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sec") && i+1 < argc) sec = std::atof(argv[++i]);
        else if (!strcmp(argv[i], "--outdir") && i+1 < argc) outDir = argv[++i];
    }
    if (list) { listDevices(); return 0; }
    int outDev = strlen(outName) ? findOut(outName) : WAVE_MAPPER;
    int inDev  = strlen(inName)  ? findIn(inName)   : WAVE_MAPPER;
    if (outDev < 0) { fprintf(stderr, "OUT device not found: %s\n", outName); return 1; }
    if (inDev  < 0) { fprintf(stderr, "IN device not found: %s\n", inName);   return 1; }

    const int sr = 48000;
    int N = (int)(sr * sec);
    std::vector<int16_t> play(N);
    std::vector<int16_t> cap(N, 0);
    for (int i = 0; i < N; i++)
        play[i] = (int16_t)(std::sin(2.0 * 3.14159265358979 * freq * i / sr) * 24576);

    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = sr;
    fmt.nAvgBytesPerSec = sr * 2;
    fmt.nBlockAlign = 2;
    fmt.wBitsPerSample = 16;

    HWAVEOUT hOut = nullptr;
    HWAVEIN  hIn  = nullptr;
    if (waveOutOpen(&hOut, outDev, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        fprintf(stderr, "waveOutOpen failed\n"); return 2;
    }
    if (waveInOpen(&hIn, inDev, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        fprintf(stderr, "waveInOpen failed\n"); waveOutClose(hOut); return 2;
    }

    WAVEHDR hp{}, hi{};
    hp.lpData = (LPSTR)play.data(); hp.dwBufferLength = (DWORD)(N * 2);
    hi.lpData = (LPSTR)cap.data();  hi.dwBufferLength = (DWORD)(N * 2);
    waveOutPrepareHeader(hOut, &hp, sizeof(hp));
    waveInPrepareHeader(hIn, &hi, sizeof(hi));
    waveInAddBuffer(hIn, &hi, sizeof(hi));
    waveInStart(hIn);
    waveOutWrite(hOut, &hp, sizeof(hp));

    Sleep((DWORD)(sec * 1000 + 300));

    waveInStop(hIn);
    while (!(hp.dwFlags & WHDR_DONE)) Sleep(5);
    while (!(hi.dwFlags & WHDR_DONE)) Sleep(5);
    waveOutUnprepareHeader(hOut, &hp, sizeof(hp));
    waveInUnprepareHeader(hIn, &hi, sizeof(hi));
    waveOutClose(hOut);
    waveInClose(hIn);

    double f1 = goertzel(cap.data(), N, sr, freq);
    double h2 = goertzel(cap.data(), N, sr, freq * 2);
    double h3 = goertzel(cap.data(), N, sr, freq * 3);
    double h4 = goertzel(cap.data(), N, sr, freq * 4);
    double h5 = goertzel(cap.data(), N, sr, freq * 5);
    double thd = (f1 > 1e-9) ? 100.0 * std::sqrt(h2*h2 + h3*h3 + h4*h4 + h5*h5) / f1 : 0.0;

    int peak = 0; double sumSq = 0;
    for (int v : cap) { int a = v < 0 ? -v : v; if (a > peak) peak = a; sumSq += (double)v * v; }
    double rms = std::sqrt(sumSq / N) / 32768.0;
    double peakDb = peak > 0 ? 20.0 * std::log10(peak / 32767.0) : -999.0;

    printf("{\"out\":\"%s\",\"in\":\"%s\",\"freq\":%d,\"sec\":%.2f,"
           "\"fund\":%.5f,\"thd_pct\":%.2f,\"peak\":%d,\"peak_db\":%.1f,\"rms\":%.5f}\n",
           outName, inName, freq, sec, f1, thd, peak, peakDb, rms);

    if (strlen(outDir) > 0) {
        char path[1024];
        snprintf(path, sizeof(path), "%s\\cap.wav", outDir);
        CreateDirectoryA(outDir, nullptr);
        writeWav(path, cap, sr);
        fprintf(stderr, "saved %s\n", path);
    }
    return 0;
}
