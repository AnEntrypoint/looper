// Measures the EXACT output fundamental of EngineSoladSnac at scale=0.5
// across notes. The fix under test (phase-anchored splice) must make every
// note land at exactly half the input frequency on synthetic input AND keep
// it there. Pre-fix: host was already exact on pure sine (the bias only
// showed on noisy Pi input), so this test adds a small per-period noise jitter
// to EMULATE the Pi's slightly-different-each-period real input, which is what
// triggered the bestOff consistent-sign bias.
#include "engines/engine_solad_snac.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <random>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double measureFund(const std::vector<float>& x, int sr) {
    // Goertzel-free: autocorrelation peak with parabolic interpolation,
    // restricted to 20-400 Hz lag range (covers all our -12 outputs).
    int n = (int)x.size();
    int minLag = sr / 400;   // 400 Hz
    int maxLag = sr / 20;    // 20 Hz
    // use the steady tail (skip warmup)
    int start = n / 3;
    double bestVal = -1e30; int bestLag = minLag;
    std::vector<double> ac(maxLag + 2, 0.0);
    for (int lag = minLag; lag <= maxLag; lag++) {
        double s = 0.0;
        for (int i = start; i < n - lag; i++) s += (double)x[i] * x[i + lag];
        ac[lag] = s;
        if (s > bestVal) { bestVal = s; bestLag = lag; }
    }
    // parabolic interp around bestLag
    double y0 = ac[bestLag - 1], y1 = ac[bestLag], y2 = ac[bestLag + 1];
    double denom = (y0 - 2 * y1 + y2);
    double delta = denom != 0.0 ? 0.5 * (y0 - y2) / denom : 0.0;
    double lag = bestLag + delta;
    return (double)sr / lag;
}

int main() {
    const int sr = 48000;
    const double durS = 6.0;
    int n = (int)(sr * durS);
    double inFreqs[] = {82.41, 110.0, 146.83, 220.0};
    std::mt19937 rng(12345);
    std::normal_distribution<float> jit(0.0f, 0.003f);  // per-sample noise ~ real input

    printf("note_in(Hz)  target-12(Hz)  measured(Hz)  err(%%)\n");
    int fails = 0;
    for (double f : inFreqs) {
        std::vector<float> in(n), out;
        // harmonic-rich + tiny noise so consecutive periods are NOT identical
        for (int i = 0; i < n; i++) {
            double t = (double)i / sr;
            float v = 0.6f * sinf(2*M_PI*f*t)
                    + 0.25f * sinf(2*M_PI*2*f*t)
                    + 0.12f * sinf(2*M_PI*3*f*t);
            v += jit(rng);
            in[i] = v;
        }
        EngineSoladSnac::run(in, out, sr, 0.5f, 0.0f);
        double target = f / 2.0;
        double meas = measureFund(out, sr);
        double err = (meas - target) / target * 100.0;
        // click detector: 2nd-difference (jerk) outliers vs robust sigma.
        // A splice discontinuity shows as a large |out[i]-2out[i-1]+out[i-2]|
        // relative to the signal's own typical 2nd-difference.
        int n2 = (int)out.size();
        std::vector<double> d2(n2, 0.0);
        double mean = 0;
        for (int i = 2; i < n2; i++) { d2[i] = fabs((double)out[i] - 2.0*out[i-1] + out[i-2]); mean += d2[i]; }
        mean /= (n2 - 2);
        double var = 0; for (int i = 2; i < n2; i++) { double e = d2[i]-mean; var += e*e; }
        double sigma = sqrt(var / (n2 - 2));
        int clicks = 0;
        for (int i = 2; i < n2; i++) if (d2[i] > mean + 10.0*sigma) clicks++;
        printf("%9.2f   %10.2f   %10.2f   %+6.2f   clicks=%d\n", f, target, meas, err, clicks);
        if (fabs(err) > 1.0) fails++;
    }
    printf("\n%s\n", fails == 0 ? "PASS: all notes within 1%% of exact -12"
                                : "FAIL: some notes exceed 1%% error");
    return fails;
}
