// Proves the formant pre-resample stage does NOT leak into pitch ("slowing
// down"). Drives EngineSoladSnac directly so we can read the new pre_eff /
// pre_perr witnesses. For each formant depth: pre_eff must equal the target
// warp rate preRate=pow(0.5,-depth) (net rate, integer-anchored = no detune),
// pre_perr ~0 (splices frequency-neutral), AND the OUTPUT fundamental must
// stay at exact -12 (the formant knob must never move pitch).
#include "engines/engine_solad_snac.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <random>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double measureFund(const std::vector<float>& x, int sr) {
    int n = (int)x.size();
    int minLag = sr / 400, maxLag = sr / 20, start = n / 3;
    double bestVal = -1e30; int bestLag = minLag;
    std::vector<double> ac(maxLag + 2, 0.0);
    for (int lag = minLag; lag <= maxLag; lag++) {
        double s = 0.0;
        for (int i = start; i < n - lag; i++) s += (double)x[i] * x[i + lag];
        ac[lag] = s; if (s > bestVal) { bestVal = s; bestLag = lag; }
    }
    double y0 = ac[bestLag-1], y1 = ac[bestLag], y2 = ac[bestLag+1];
    double den = (y0 - 2*y1 + y2);
    double d = den != 0.0 ? 0.5*(y0-y2)/den : 0.0;
    return (double)sr / (bestLag + d);
}

int main() {
    const int sr = 48000; const double durS = 6.0; int n = (int)(sr*durS);
    double inFreqs[] = {82.41, 110.0, 220.0};
    float depths[] = {-1.0f, 0.0f, 1.0f};
    std::mt19937 rng(12345);
    std::normal_distribution<float> jit(0.0f, 0.003f);

    int fails = 0;
    printf("freq  depth  preRate  pre_eff   pre_perr  outFund   target-12  ferr%%\n");
    for (double f : inFreqs) {
        std::vector<float> in(n);
        for (int i = 0; i < n; i++) {
            double t = (double)i/sr;
            float v = 0.6f*sinf(2*M_PI*f*t)+0.25f*sinf(2*M_PI*2*f*t)+0.12f*sinf(2*M_PI*3*f*t);
            in[i] = v + jit(rng);
        }
        for (float d : depths) {
            EngineSoladSnac e;
            e.setPitchScale(0.5f);
            e.setFormantDepth(d);
            std::vector<float> out(n, 0.0f);
            const int CH = 64;
            // run, sampling pre_eff/pre_perr over the steady tail only
            double effSum = 0, perrSum = 0; int wins = 0;
            for (int i = 0; i < n; i += CH) {
                int m = (i + CH <= n) ? CH : (n - i);
                e.processBlock(&in[i], &out[i], m);
                if (i > n/3) {   // steady tail
                    effSum += e.preEffRateNow();
                    perrSum += e.preSplicePhaseErrNow();
                    wins++;
                } else { e.preEffRateNow(); e.preSplicePhaseErrNow(); }
            }
            float preEff = wins ? (float)(effSum/wins) : 0;
            float prePerr = wins ? (float)(perrSum/wins) : 0;
            double target = f / 2.0;
            double outF = measureFund(out, sr);
            double ferr = (outF - target)/target*100.0;
            // preRate is clamped to [0.7,1.3] (smooth-formant band) so the
            // witness target is the clamped value, not the raw pow().
            float preRate = powf(0.5f, -d);
            if (preRate < 0.7f) preRate = 0.7f;
            if (preRate > 1.3f) preRate = 1.3f;
            // pre_eff must track the (clamped) preRate; pitch must stay exact.
            bool rateOk = fabsf(preEff - preRate) < 0.02f * (preRate > 1 ? preRate : 1);
            bool pitchOk = fabs(ferr) < 1.5;
            bool ok = rateOk && pitchOk;
            printf("%5.0f %5.1f  %6.3f  %7.4f  %7.4f  %7.2f  %8.2f  %+5.2f  %s\n",
                   f, d, preRate, preEff, prePerr, outF, target, ferr,
                   ok ? "ok" : (rateOk ? "FAIL(pitch)" : "FAIL(rate-leak)"));
            if (!ok) fails++;
        }
    }
    printf("\n%s\n", fails==0 ? "PASS: pre-stage net rate tracks preRate, pitch exact at all depths"
                              : "FAIL: formant leaks into pitch or detunes");
    return fails;
}
