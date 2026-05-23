// test-psola.cpp — host-side numerical correctness test for psolaOctaver.
//
// Feeds synthesized 82.4Hz sine (guitar low-E) into PsolaOctaver with
// pitchScale=0.5 (target -12 = 41.2Hz). Captures output, measures
// fundamental via Goertzel, computes THD over harmonics 2-5.
//
// Pass criteria:
//   detected output fundamental within ±1Hz of 41.2Hz
//   THD < 10% (PSOLA on a pure sine has tiny window-leakage but no beating)
//
// Compile (Windows mingw):
//   g++ -std=c++17 -O2 -I../patches scripts/test-psola.cpp -o test-psola.exe
//   ./test-psola.exe
//
// Compile (Linux/macOS):
//   g++ -std=c++17 -O2 -I patches scripts/test-psola.cpp -o test-psola
//   ./test-psola
//
// Output:
//   Lines like "RESULT 82.4 -> 41.21Hz  THD 3.8%  PASS"
//   Exit code 0 on all-pass, 1 on any fail.

#define _USE_MATH_DEFINES
#include <cstdio>
#include <cstdlib>
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <vector>
#include "../patches/psolaOctaver.h"

static double goertzel(const float *x, int n, double freq, int sr) {
    double coef = 2.0 * cos(2.0 * M_PI * freq / sr);
    double s_prev = 0.0, s_prev2 = 0.0;
    for (int i = 0; i < n; i++) {
        double s = (double)x[i] + coef * s_prev - s_prev2;
        s_prev2 = s_prev; s_prev = s;
    }
    return sqrt(s_prev2 * s_prev2 + s_prev * s_prev - coef * s_prev * s_prev2);
}

struct TestResult {
    double inputFreq;
    double targetFreq;
    double detectedFreq;
    double thdPct;
    double fundAmp;
    bool pass;
};

static double findPeakFundamental(const float *x, int n, int sr,
                                  double searchLo, double searchHi) {
    // Coarse scan: 0.5Hz resolution
    double bestFreq = 0.0;
    double bestAmp = 0.0;
    for (double f = searchLo; f <= searchHi; f += 0.5) {
        double a = goertzel(x, n, f, sr);
        if (a > bestAmp) { bestAmp = a; bestFreq = f; }
    }
    // Refine: 0.05Hz around peak
    double lo = bestFreq - 0.5, hi = bestFreq + 0.5;
    for (double f = lo; f <= hi; f += 0.05) {
        double a = goertzel(x, n, f, sr);
        if (a > bestAmp) { bestAmp = a; bestFreq = f; }
    }
    return bestFreq;
}

static TestResult runTest(double inputFreq, float scale, int sr, int durMs) {
    int n = sr * durMs / 1000;
    std::vector<float> input(n), output(n);
    double twoPiF = 2.0 * M_PI * inputFreq / sr;
    for (int i = 0; i < n; i++) {
        input[i] = (float)(0.7 * sin(twoPiF * i));
    }

    PsolaOctaver engine(sr);
    engine.setPitchScale(scale);

    // Process in 64-sample blocks (matches firmware AUDIO_BLOCK_SAMPLES)
    constexpr int BLOCK = 64;
    int pos = 0;
    while (pos + BLOCK <= n) {
        engine.process(&input[pos], &output[pos], BLOCK);
        pos += BLOCK;
    }

    // Analyse the last 70% of output (skip warmup)
    int analyseStart = (int)(n * 0.3);
    int analyseN = n - analyseStart;
    double targetFreq = inputFreq * scale;
    // Search ±10Hz around target
    double detected = findPeakFundamental(&output[analyseStart], analyseN, sr,
                                          std::max(20.0, targetFreq - 10.0),
                                          targetFreq + 10.0);
    double fundAmp = goertzel(&output[analyseStart], analyseN, detected, sr);
    double h2 = goertzel(&output[analyseStart], analyseN, detected * 2, sr);
    double h3 = goertzel(&output[analyseStart], analyseN, detected * 3, sr);
    double h4 = goertzel(&output[analyseStart], analyseN, detected * 4, sr);
    double h5 = goertzel(&output[analyseStart], analyseN, detected * 5, sr);
    double thd = (fundAmp > 1e-9)
                 ? sqrt(h2*h2 + h3*h3 + h4*h4 + h5*h5) / fundAmp * 100.0
                 : 999.0;

    bool freqOk = fabs(detected - targetFreq) < 1.0;
    bool thdOk  = thd < 10.0;
    bool ampOk  = fundAmp > 0.01;  // engine actually produced output

    TestResult r{ inputFreq, targetFreq, detected, thd, fundAmp,
                  freqOk && thdOk && ampOk };
    return r;
}

int main() {
    constexpr int SR = 48000;
    constexpr int DUR_MS = 2000;
    int fails = 0;
    struct Case { double f; float scale; const char *label; };
    Case cases[] = {
        { 82.4,  0.5f,  "low-E down -12 (41.2Hz target)" },
        { 110.0, 0.5f,  "A2 down -12 (55Hz target)" },
        { 220.0, 0.5f,  "A3 down -12 (110Hz target)" },
        { 440.0, 0.5f,  "A4 down -12 (220Hz target)" },
    };
    for (auto &c : cases) {
        TestResult r = runTest(c.f, c.scale, SR, DUR_MS);
        printf("[%s] RESULT %.1f -> %.2fHz (target %.1fHz, err=%.2fHz)  "
               "THD %.1f%%  amp=%.3f  %s\n",
               r.pass ? "PASS" : "FAIL", r.inputFreq, r.detectedFreq, r.targetFreq,
               r.detectedFreq - r.targetFreq, r.thdPct, r.fundAmp, c.label);
        if (!r.pass) fails++;
    }
    printf("\n%d/%lu tests passed\n", (int)(sizeof(cases)/sizeof(cases[0])) - fails,
           sizeof(cases)/sizeof(cases[0]));
    return fails > 0 ? 1 : 0;
}
