// gen-corpus.cpp — emit fixed test-signal corpus for pitch-shift quality.
// All output: 48kHz 16-bit mono WAV.
//
// Compile/run:
//   g++ -std=c++17 -O2 scripts/quality-corpus/gen-corpus.cpp -o scripts/quality-corpus/gen-corpus.exe
//   ./scripts/quality-corpus/gen-corpus.exe scripts/quality-corpus/

#define _USE_MATH_DEFINES
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void writeWav(const std::string &path, const std::vector<int16_t> &s, int sr) {
    FILE *f = fopen(path.c_str(), "wb");
    int byteRate = sr * 2;
    int dataSize = (int)s.size() * 2;
    fwrite("RIFF", 1, 4, f);
    int rs = 36 + dataSize; fwrite(&rs, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    int fs = 16; fwrite(&fs, 4, 1, f);
    int16_t pcm = 1; fwrite(&pcm, 2, 1, f);
    int16_t ch = 1;  fwrite(&ch, 2, 1, f);
    fwrite(&sr, 4, 1, f);
    fwrite(&byteRate, 4, 1, f);
    int16_t ba = 2; fwrite(&ba, 2, 1, f);
    int16_t bps = 16; fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&dataSize, 4, 1, f);
    fwrite(s.data(), 2, s.size(), f);
    fclose(f);
}

static int16_t f2s(double v) {
    v *= 32767.0;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)(v < 0 ? v - 0.5 : v + 0.5);
}

// pure sine 2s
static std::vector<int16_t> pureSine(double f, int sr, double dur, double amp = 0.7) {
    int n = (int)(sr * dur);
    std::vector<int16_t> out(n);
    double w = 2.0 * M_PI * f / sr;
    for (int i = 0; i < n; i++) out[i] = f2s(amp * std::sin(w * i));
    return out;
}

// harmonic-rich plucked-string approximation
static std::vector<int16_t> guitarTone(double fund, int sr, double dur, double amp = 0.6) {
    int n = (int)(sr * dur);
    std::vector<int16_t> out(n);
    const int H = 8;
    double B = 0.00012;  // inharmonicity
    double hF[H], hA[H], hPhi[H], hDecRate[H];
    double aSum = 0;
    for (int k = 0; k < H; k++) {
        double kk = k + 1;
        hF[k] = fund * kk * std::sqrt(1.0 + kk * kk * B);
        hA[k] = 1.0 / std::pow(kk, 0.65);
        hPhi[k] = std::fmod(kk * 0.13, 1.0) * 2.0 * M_PI;
        hDecRate[k] = 1.0 / (1.5 * sr / std::sqrt(kk));   // tau secs / sqrt(k)
        aSum += hA[k];
    }
    for (int k = 0; k < H; k++) hA[k] /= aSum;
    int atk = (int)(sr * 0.004);
    for (int i = 0; i < n; i++) {
        double env = (i < atk) ? (double)i / atk : 1.0;
        double s = 0;
        for (int k = 0; k < H; k++) {
            double d = std::exp(-i * hDecRate[k]);
            s += hA[k] * d * std::sin(2.0 * M_PI * hF[k] * i / sr + hPhi[k]);
        }
        out[i] = f2s(amp * env * s);
    }
    return out;
}

// pluck-attack-then-sustain: sharp 1ms attack, 50ms decay to sustain, hold 1s
static std::vector<int16_t> pluckSustain(double fund, int sr, double dur) {
    int n = (int)(sr * dur);
    std::vector<int16_t> out(n);
    int atk = (int)(sr * 0.001);
    double sustain = 0.3;
    double tauDec = sr * 0.05;
    for (int i = 0; i < n; i++) {
        double env;
        if (i < atk) env = (double)i / atk;
        else {
            double settle = std::exp(-(i - atk) / tauDec);
            env = sustain + (1.0 - sustain) * settle;
        }
        double s = 0.6 * std::sin(2.0 * M_PI * fund * i / sr);
        s += 0.3 * std::sin(2.0 * M_PI * fund * 2 * i / sr);
        s += 0.15 * std::sin(2.0 * M_PI * fund * 3 * i / sr);
        out[i] = f2s(env * s);
    }
    return out;
}

// log sine sweep 50Hz -> 1000Hz
static std::vector<int16_t> sineSweep(int sr, double dur, double f0 = 50, double f1 = 1000, double amp = 0.5) {
    int n = (int)(sr * dur);
    std::vector<int16_t> out(n);
    double T = dur;
    double K = T * f0 / std::log(f1 / f0);
    double L = T / std::log(f1 / f0);
    for (int i = 0; i < n; i++) {
        double t = (double)i / sr;
        double phase = 2.0 * M_PI * K * (std::exp(t / L) - 1.0);
        out[i] = f2s(amp * std::sin(phase));
    }
    return out;
}

// gated white noise (250ms ramp-in + 500ms hold + 250ms ramp-out)
static std::vector<int16_t> whiteNoise(int sr, double dur, double amp = 0.4) {
    int n = (int)(sr * dur);
    std::vector<int16_t> out(n);
    unsigned x = 0xCAFEBABEu;
    int rampN = sr / 4;
    for (int i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        double v = ((int)(x >> 9) & 0x7FFFFF) / (double)0x800000 - 0.5;
        double env = 1.0;
        if (i < rampN) env = (double)i / rampN;
        else if (i > n - rampN) env = (double)(n - i) / rampN;
        out[i] = f2s(amp * env * 2.0 * v);
    }
    return out;
}

int main(int argc, char **argv) {
    std::string outDir = argc > 1 ? argv[1] : "scripts/quality-corpus/";
    if (outDir.back() != '/' && outDir.back() != '\\') outDir += '/';
    const int SR = 48000;
    auto save = [&](const std::string &name, const std::vector<int16_t> &s) {
        std::string p = outDir + name;
        writeWav(p, s, SR);
        printf("wrote %s (%d samples)\n", p.c_str(), (int)s.size());
    };

    // Pure sines (steady-state spectral purity test)
    save("sine_82_4.wav", pureSine(82.41, SR, 2.0));
    save("sine_110.wav",  pureSine(110.0, SR, 2.0));
    save("sine_220.wav",  pureSine(220.0, SR, 2.0));
    save("sine_440.wav",  pureSine(440.0, SR, 2.0));

    // Harmonic guitar tones (realistic spectral content)
    save("guitar_E2.wav", guitarTone(82.41, SR, 2.0));
    save("guitar_A2.wav", guitarTone(110.0, SR, 2.0));
    save("guitar_D3.wav", guitarTone(146.83, SR, 2.0));

    // Pluck-attack-sustain (transient handling)
    save("pluck_E2.wav", pluckSustain(82.41, SR, 1.5));
    save("pluck_A2.wav", pluckSustain(110.0, SR, 1.5));

    // Frequency response sweep
    save("sweep_50_1000.wav", sineSweep(SR, 4.0));

    // White noise (artifact test)
    save("noise_gated.wav", whiteNoise(SR, 1.0));

    return 0;
}
