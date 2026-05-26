// Guards the formant artifact class that telemetry/click/pitch tests missed:
// the pre-resample REPOSITION (splice) rate is what gurgles/crackles. center
// must be 0 (bypassed), and formant up/down must stay in the smooth band
// (<=~15/s) — a regression that lets preRate drift toward 2.0 spikes this to
// ~66/s = the audible garble. This is the local witness; if it passes, the
// formant is smooth on the Pi too (the rate is material-independent).
#include "engines/engine_solad_snac.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <random>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int main() {
    const int sr = 48000; int n = sr * 6; double f = 110.0;
    std::mt19937 rng(1); std::normal_distribution<float> jit(0.0f, 0.003f);
    std::vector<float> in(n);
    for (int i = 0; i < n; i++) {
        double t = (double)i / sr;
        in[i] = 0.6f*sinf(2*M_PI*f*t) + 0.25f*sinf(2*M_PI*2*f*t)
              + 0.12f*sinf(2*M_PI*3*f*t) + jit(rng);
    }
    float d[] = {-1.0f, 0.0f, 1.0f};
    const char* nm[] = {"down(-1)", "center(0)", "up(+1)"};
    int fails = 0;
    printf("depth       splices/s   limit\n");
    for (int j = 0; j < 3; j++) {
        EngineSoladSnac e; e.setPitchScale(0.5f); e.setFormantDepth(d[j]);
        std::vector<float> out(n, 0.0f); const int CH = 64;
        e.preSpliceCountNow();
        for (int i = 0; i < n; i += CH) {
            int m = (i + CH <= n) ? CH : (n - i);
            e.processBlock(&in[i], &out[i], m);
        }
        double rate = e.preSpliceCountNow() / 6.0;
        double limit = (d[j] == 0.0f) ? 0.5 : 15.0;   // center must be ~0
        bool ok = rate <= limit;
        printf("%-11s %8.1f    %5.1f  %s\n", nm[j], rate, limit, ok ? "ok" : "FAIL(garble)");
        if (!ok) fails++;
    }
    printf("\n%s\n", fails == 0 ? "PASS: pre-stage splice rate in smooth band at all depths"
                                : "FAIL: splice rate spiked = formant will gurgle");
    return fails;
}
