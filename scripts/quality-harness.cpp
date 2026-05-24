// quality-harness.cpp — pitch-shift engine quality measurement
//
// Loads a WAV (16-bit mono 48kHz), runs it through an engine (selected at
// compile time via -DENGINE=N), computes objective metrics on the OUTPUT
// at the target pitch-shifted frequency, emits one JSON line per test.
//
// Metrics computed:
//   - fund_hz_in / fund_hz_out / fund_err_hz : detected fundamental in/out
//   - thd_pct : sqrt(h2² + h3² + ... + h8²) / fund * 100
//   - centroid_in_hz / centroid_out_hz : spectral centroid (brightness)
//   - att_corr : envelope correlation (input vs output, time-aligned) — how
//                well the attack/decay shape survived
//   - clicks  : count of sample-derivative outliers (|y[n]-y[n-1]| > 3σ)
//   - rms_in / rms_out : level match
//   - lat_ms : measured engine latency (envelope-xcorr method)
//
// Compile (host, all engines built but selected at runtime via argv):
//   g++ -std=c++17 -O2 -I patches -I scripts/engines \
//     scripts/quality-harness.cpp -o scripts/quality-harness.exe
//
// Run:
//   ./scripts/quality-harness.exe <engine_name> <input.wav> <expected_fund_hz>
//
// Output: one JSON line on stdout, easy to aggregate.

#define _USE_MATH_DEFINES
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <complex>
#include <fstream>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---- WAV I/O ----
struct Wav { std::vector<float> samples; int sr = 48000; };

static Wav readWav(const std::string &path) {
    Wav w;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { fprintf(stderr, "wav read fail: %s\n", path.c_str()); std::exit(2); }
    auto sz = f.tellg(); f.seekg(0);
    std::vector<uint8_t> b((size_t)sz); f.read((char*)b.data(), sz);
    int16_t ch = *(int16_t*)(b.data() + 22);
    w.sr = *(int*)(b.data() + 24);
    int16_t bps = *(int16_t*)(b.data() + 34);
    if (bps != 16) { fprintf(stderr, "only 16-bit\n"); std::exit(3); }
    // find data
    size_t off = 12;
    while (off < b.size() - 8) {
        if (std::memcmp(b.data() + off, "data", 4) == 0) { off += 8; break; }
        off += 8 + *(int*)(b.data() + off + 4);
    }
    int n = ((int)b.size() - (int)off) / (ch * 2);
    w.samples.resize(n);
    for (int i = 0; i < n; i++) {
        int acc = 0;
        for (int c = 0; c < ch; c++) acc += *(int16_t*)(b.data() + off + (i * ch + c) * 2);
        w.samples[i] = (float)acc / ch / 32768.0f;
    }
    return w;
}

static void writeWavF(const std::string &path, const std::vector<float> &s, int sr) {
    std::ofstream f(path, std::ios::binary);
    int dataSize = (int)s.size() * 2;
    int rs = 36 + dataSize;
    f.write("RIFF", 4); f.write((char*)&rs, 4); f.write("WAVEfmt ", 8);
    int fs = 16; f.write((char*)&fs, 4);
    int16_t pcm=1, ch=1, ba=2, bps=16;
    int byteRate = sr * 2;
    f.write((char*)&pcm, 2); f.write((char*)&ch, 2); f.write((char*)&sr, 4);
    f.write((char*)&byteRate, 4); f.write((char*)&ba, 2); f.write((char*)&bps, 2);
    f.write("data", 4); f.write((char*)&dataSize, 4);
    for (float v : s) {
        int16_t s16 = (int16_t)std::lround(std::max(-32768.0f, std::min(32767.0f, v * 32767.0f)));
        f.write((char*)&s16, 2);
    }
}

// ---- DFT / metrics ----
static double goertzel(const float *x, int n, double f, int sr) {
    double coef = 2.0 * std::cos(2.0 * M_PI * f / sr);
    double p = 0, p2 = 0;
    for (int i = 0; i < n; i++) {
        double s = (double)x[i] + coef * p - p2;
        p2 = p; p = s;
    }
    return std::sqrt(p2 * p2 + p * p - coef * p * p2);
}

// Search for max-amp frequency near expected target. Coarse 1Hz grid + 0.05Hz refine.
static double findPeakNear(const float *x, int n, double fLo, double fHi, int sr, double *ampOut = nullptr) {
    double bF = fLo, bA = 0;
    for (double f = fLo; f <= fHi; f += 0.5) {
        double a = goertzel(x, n, f, sr);
        if (a > bA) { bA = a; bF = f; }
    }
    for (double f = bF - 0.5; f <= bF + 0.5; f += 0.05) {
        double a = goertzel(x, n, f, sr);
        if (a > bA) { bA = a; bF = f; }
    }
    if (ampOut) *ampOut = bA;
    return bF;
}

static double spectralCentroid(const float *x, int n, int sr, double fMin = 20, double fMax = 8000) {
    double num = 0, den = 0;
    for (double f = fMin; f <= fMax; f += 5.0) {
        double a = goertzel(x, n, f, sr);
        num += f * a;
        den += a;
    }
    return den > 1e-9 ? num / den : 0;
}

static int countClicks(const std::vector<float> &x) {
    int n = (int)x.size();
    if (n < 2) return 0;
    std::vector<double> d(n - 1);
    double sumSq = 0;
    for (int i = 1; i < n; i++) { d[i - 1] = std::abs((double)x[i] - x[i - 1]); sumSq += d[i - 1] * d[i - 1]; }
    double rms = std::sqrt(sumSq / (n - 1));
    double thr = rms * 6.0;
    int cnt = 0;
    for (auto v : d) if (v > thr) cnt++;
    return cnt;
}

static double envelopeCorrelation(const std::vector<float> &a, const std::vector<float> &b, int sr) {
    int n = (int)std::min(a.size(), b.size());
    double alpha = 1.0 - std::exp(-1.0 / (sr * 0.005));
    std::vector<double> ea(n), eb(n);
    double xa = 0, xb = 0;
    for (int i = 0; i < n; i++) {
        xa += alpha * (std::abs(a[i]) - xa);
        xb += alpha * (std::abs(b[i]) - xb);
        ea[i] = xa; eb[i] = xb;
    }
    // skip first 5ms transient settle
    int s0 = sr / 200;
    double mxa = 0, mxb = 0;
    for (int i = s0; i < n; i++) { mxa += ea[i]; mxb += eb[i]; }
    mxa /= (n - s0); mxb /= (n - s0);
    double num = 0, da = 0, db = 0;
    for (int i = s0; i < n; i++) {
        double pa = ea[i] - mxa, pb = eb[i] - mxb;
        num += pa * pb; da += pa * pa; db += pb * pb;
    }
    return da * db > 1e-12 ? num / std::sqrt(da * db) : 0;
}

static double rms(const std::vector<float> &x, int from = 0, int to = -1) {
    if (to < 0) to = (int)x.size();
    double s = 0;
    for (int i = from; i < to; i++) s += (double)x[i] * x[i];
    int n = to - from;
    return n > 0 ? std::sqrt(s / n) : 0;
}

// Envelope-XCorr latency estimate (input vs output)
static double estLatencyMs(const std::vector<float> &in, const std::vector<float> &out, int sr) {
    int n = (int)std::min(in.size(), out.size());
    double alpha = 1.0 - std::exp(-1.0 / (sr * 0.005));
    std::vector<double> ei(n), eo(n);
    double xi = 0, xo = 0;
    for (int i = 0; i < n; i++) {
        xi += alpha * (std::abs(in[i]) - xi);  ei[i] = xi;
        xo += alpha * (std::abs(out[i]) - xo); eo[i] = xo;
    }
    int maxLag = (int)(sr * 0.040);  // up to 40ms
    int tplLen = std::min(n - maxLag, sr / 4);
    if (tplLen < 1000) return -1;
    double bestC = -1e30; int bestL = 0;
    for (int lag = 0; lag <= maxLag; lag++) {
        double s = 0;
        for (int k = 0; k < tplLen; k += 4) s += ei[k] * eo[lag + k];
        if (s > bestC) { bestC = s; bestL = lag; }
    }
    return bestL * 1000.0 / sr;
}

// ---- ENGINES ----
#include "engines/engine_signalsmith.h"
#include "engines/engine_downsample.h"
#include "engines/engine_sinc_delay.h"
#include "engines/engine_yin_psola.h"
#include "engines/engine_sinc_formant.h"
#include "engines/engine_solad_snac.h"

static void runEngine(const std::string &name, const std::vector<float> &in,
                      std::vector<float> &out, int sr, float scale) {
    out.assign(in.size(), 0);
    if      (name == "signalsmith-64-32")   engine_signalsmith(in, out, sr, scale, 64,  32);
    else if (name == "signalsmith-64-16")   engine_signalsmith(in, out, sr, scale, 64,  16);
    else if (name == "signalsmith-96-24")   engine_signalsmith(in, out, sr, scale, 96,  24);
    else if (name == "signalsmith-96-32")   engine_signalsmith(in, out, sr, scale, 96,  32);
    else if (name == "signalsmith-128-32")  engine_signalsmith(in, out, sr, scale, 128, 32);
    else if (name == "signalsmith-128-48")  engine_signalsmith(in, out, sr, scale, 128, 48);
    else if (name == "signalsmith-192-48")  engine_signalsmith(in, out, sr, scale, 192, 48);
    else if (name == "signalsmith-192-64")  engine_signalsmith(in, out, sr, scale, 192, 64);
    else if (name == "signalsmith-160-48")  engine_signalsmith(in, out, sr, scale, 160, 48);
    else if (name == "signalsmith-160-64")  engine_signalsmith(in, out, sr, scale, 160, 64);
    else if (name == "signalsmith-256-64")  engine_signalsmith(in, out, sr, scale, 256, 64);
    else if (name == "signalsmith-256-96")  engine_signalsmith(in, out, sr, scale, 256, 96);
    else if (name == "signalsmith-128-48-split") engine_signalsmith(in, out, sr, scale, 128, 48, true);
    else if (name == "signalsmith-256-96-split") engine_signalsmith(in, out, sr, scale, 256, 96, true);
    else if (name == "downsample-64-32")    engine_downsample(in, out, sr, scale, 64,  32);
    else if (name == "downsample-96-32")    engine_downsample(in, out, sr, scale, 96,  32);
    else if (name == "downsample-128-32")   engine_downsample(in, out, sr, scale, 128, 32);
    else if (name == "sinc-delay-128")      engine_sinc_delay(in, out, sr, scale, 128);
    else if (name == "sinc-delay-192")      engine_sinc_delay(in, out, sr, scale, 192);
    else if (name == "sinc-delay-256")      engine_sinc_delay(in, out, sr, scale, 256);
    else if (name == "sinc-delay-384")      engine_sinc_delay(in, out, sr, scale, 384);
    else if (name == "yin-psola")           EngineYinPsola::run(in, out, sr, scale);
    else if (name == "solad-snac")          EngineSoladSnac::run(in, out, sr, scale);
    else if (name == "solad-fd-0.0")        EngineSoladSnac::run(in, out, sr, scale,  0.0f);
    else if (name == "solad-fd+0.5")        EngineSoladSnac::run(in, out, sr, scale, +0.5f);
    else if (name == "solad-fd+1.0")        EngineSoladSnac::run(in, out, sr, scale, +1.0f);
    else if (name == "solad-fd-0.5")        EngineSoladSnac::run(in, out, sr, scale, -0.5f);
    else if (name == "solad-fd-1.0")        EngineSoladSnac::run(in, out, sr, scale, -1.0f);
    else if (name == "solad-fd+1.5")        EngineSoladSnac::run(in, out, sr, scale, +1.5f);
    else if (name == "solad-fd-1.5")        EngineSoladSnac::run(in, out, sr, scale, -1.5f);
    else if (name == "sinc-formant-neutral")  engine_sinc_formant(in, out, sr, scale, 192, 0.0f, 0.0f, 800.0f);
    else if (name == "sinc-formant-dark")     engine_sinc_formant(in, out, sr, scale, 192, -0.7f, 0.0f, 800.0f);
    else if (name == "sinc-formant-bright")   engine_sinc_formant(in, out, sr, scale, 192, +0.7f, 0.0f, 800.0f);
    else if (name == "sinc-formant-vocal-A")  engine_sinc_formant(in, out, sr, scale, 192, 0.0f, 0.8f, 700.0f);
    else if (name == "sinc-formant-vocal-O")  engine_sinc_formant(in, out, sr, scale, 192, -0.3f, 0.8f, 500.0f);
    else if (name == "sinc-formant-vocal-E")  engine_sinc_formant(in, out, sr, scale, 192, +0.3f, 0.8f, 2000.0f);
    else if (name == "sinc-formant-growl")    engine_sinc_formant(in, out, sr, scale, 192, -0.5f, 1.0f, 300.0f);
    else if (name == "sinc-formant-airy")     engine_sinc_formant(in, out, sr, scale, 192, +0.5f, 0.6f, 1800.0f);
    else { fprintf(stderr, "unknown engine: %s\n", name.c_str()); std::exit(4); }
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <engine_name> <input.wav> <expected_fund_hz> [output.wav]\n", argv[0]);
        return 1;
    }
    std::string engine = argv[1];
    std::string path = argv[2];
    double fund = std::stod(argv[3]);
    std::string outPath = argc > 4 ? argv[4] : "";

    Wav w = readWav(path);
    std::vector<float> outF;
    runEngine(engine, w.samples, outF, w.sr, 0.5f);

    if (!outPath.empty()) writeWavF(outPath, outF, w.sr);

    // Analysis window: skip first 100ms (engine warmup), use middle 70%
    int s0 = std::min((int)outF.size() - 1, w.sr / 10);
    int s1 = (int)outF.size();
    int len = s1 - s0;
    if (len < w.sr / 4) { fprintf(stderr, "output too short for analysis\n"); return 5; }
    const float *xa = &outF[s0];
    int la = len;

    // Detected fundamental on output should be ~ fund/2 (for scale=0.5)
    double targetFund = fund * 0.5;
    double fLo = std::max(20.0, targetFund * 0.85);
    double fHi = targetFund * 1.15;
    double fOutAmp = 0;
    double fOutHz = findPeakNear(xa, la, fLo, fHi, w.sr, &fOutAmp);

    // Also probe the INPUT fund position on output — should be near-zero ideally
    double inLeakAmp = goertzel(xa, la, fund, w.sr);

    // THD: harmonics 2-8 of detected output fundamental
    double h2 = goertzel(xa, la, fOutHz * 2, w.sr);
    double h3 = goertzel(xa, la, fOutHz * 3, w.sr);
    double h4 = goertzel(xa, la, fOutHz * 4, w.sr);
    double h5 = goertzel(xa, la, fOutHz * 5, w.sr);
    double h6 = goertzel(xa, la, fOutHz * 6, w.sr);
    double h7 = goertzel(xa, la, fOutHz * 7, w.sr);
    double h8 = goertzel(xa, la, fOutHz * 8, w.sr);
    double thd = std::sqrt(h2*h2 + h3*h3 + h4*h4 + h5*h5 + h6*h6 + h7*h7 + h8*h8) / std::max(1e-9, fOutAmp) * 100.0;

    double centIn  = spectralCentroid(w.samples.data() + s0, la, w.sr);
    double centOut = spectralCentroid(xa, la, w.sr);
    double rmsIn   = rms(w.samples, s0, s1);
    double rmsOut  = rms(outF, s0, s1);
    int    clicks  = countClicks(outF);
    double attCorr = envelopeCorrelation(w.samples, outF, w.sr);
    double latMs   = estLatencyMs(w.samples, outF, w.sr);

    // Emit one JSON line
    printf("{\"engine\":\"%s\",\"input\":\"%s\",\"fund_in\":%.2f,\"fund_out\":%.2f,"
           "\"fund_err_hz\":%.2f,\"in_leak_amp\":%.5g,\"fund_out_amp\":%.5g,"
           "\"thd_pct\":%.2f,\"centroid_in\":%.1f,\"centroid_out\":%.1f,"
           "\"rms_in\":%.5g,\"rms_out\":%.5g,\"clicks\":%d,\"att_corr\":%.3f,\"lat_ms\":%.2f}\n",
           engine.c_str(), path.c_str(),
           fund, fOutHz, fOutHz - targetFund,
           inLeakAmp, fOutAmp, thd,
           centIn, centOut, rmsIn, rmsOut, clicks, attCorr, latMs);
    return 0;
}
