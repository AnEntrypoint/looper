// Generate scripts/quality-corpus/transients.wav — three short plucks at
// 82.4 Hz with attacks spaced 333 ms / 666 ms / 1000 ms apart.
// A pitch-ONLY engine preserves the inter-onset gaps. A pitch+time engine
// (sinc-delay at scale=0.5) doubles the gaps to 666/1333/2000 ms, AND the
// resulting WAV is twice as long as input.
//
// Compile + run:
//   g++ -std=c++17 -O2 -o scripts/gen-transient-test.exe scripts/gen-transient-test.cpp
//   ./scripts/gen-transient-test.exe

#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>

constexpr int SR = 48000;
constexpr double M_PIv = 3.14159265358979323846;

static void writeWav(const char* path, const std::vector<int16_t>& data) {
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "open fail %s\n", path); return; }
    uint32_t bytes = (uint32_t)(data.size() * 2);
    uint32_t fileSize = 36 + bytes;
    uint16_t fmt = 1, ch = 1, bps = 16, ba = 2;
    uint32_t br = SR * ba;
    fwrite("RIFF", 4, 1, f);
    fwrite(&fileSize, 4, 1, f);
    fwrite("WAVEfmt ", 8, 1, f);
    uint32_t fmtSize = 16;
    fwrite(&fmtSize, 4, 1, f);
    fwrite(&fmt, 2, 1, f);
    fwrite(&ch, 2, 1, f);
    uint32_t srOut = SR;
    fwrite(&srOut, 4, 1, f);
    fwrite(&br, 4, 1, f);
    fwrite(&ba, 2, 1, f);
    fwrite(&bps, 2, 1, f);
    fwrite("data", 4, 1, f);
    fwrite(&bytes, 4, 1, f);
    fwrite(data.data(), 2, data.size(), f);
    fclose(f);
}

int main() {
    constexpr double fund = 82.4;
    constexpr double pluckDecayMs = 200;
    constexpr int totalSec = 2;
    int N = SR * totalSec;
    std::vector<float> buf(N, 0.0f);

    // Three plucks at t = 100ms, 433ms, 1100ms (= gaps 333ms, 666ms)
    double pluckTimesSec[] = { 0.10, 0.433, 1.10 };
    int nPlucks = sizeof(pluckTimesSec)/sizeof(pluckTimesSec[0]);

    for (int p = 0; p < nPlucks; p++) {
        int start = (int)(pluckTimesSec[p] * SR);
        // Karplus-like: damped sine with harmonics
        for (int n = 0; n < SR / 2 && start + n < N; n++) {
            double t = (double)n / SR;
            double envMs = t * 1000.0;
            double env = std::exp(-envMs / pluckDecayMs);
            double s = std::sin(2.0 * M_PIv * fund * t) * 0.6
                     + std::sin(2.0 * M_PIv * fund * 2 * t) * 0.3
                     + std::sin(2.0 * M_PIv * fund * 3 * t) * 0.15;
            buf[start + n] += (float)(s * env);
        }
    }

    // Convert to int16 normalised
    float maxAbs = 1e-9f;
    for (float v : buf) if (std::fabs(v) > maxAbs) maxAbs = std::fabs(v);
    std::vector<int16_t> out(N);
    for (int i = 0; i < N; i++) {
        float v = buf[i] / maxAbs * 0.85f * 32767.0f;
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    writeWav("scripts/quality-corpus/transients.wav", out);
    printf("wrote transients.wav: %d samples (%.2fs), 3 plucks at %.0f/%.0f/%.0f ms\n",
           N, (double)N/SR,
           pluckTimesSec[0]*1000, pluckTimesSec[1]*1000, pluckTimesSec[2]*1000);
    return 0;
}
