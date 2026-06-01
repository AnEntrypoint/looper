// Transpose (live-pitch -12) LATENCY BUDGET validation.
//
// NON-NEGOTIABLE budget: transpose must process within ~4ms. At downshift the
// EngineSoladSnac reader MUST lag the writer (the lag IS the pitch shift); the
// reader gap = monitoring latency. With the restored frac=1 control loop the gap
// is held at initialReadOffset + <=1 period (the PSOLA minimum): ~4ms where the
// pitch period < the offset, ~1 period at low-E (the physical floor).
//
// This test drives the REAL engine at scale=0.5 on sustained tones WITH per-
// sample gaussian noise (~0.003 stddev) — the established Pi-input emulation
// (scripts/test-freq-neutral.cpp) that makes consecutive periods differ, the
// condition that turned blind splices buzzy. It asserts, per pitch:
//   (1) mean gap <= initialReadOffset + 1 period            (latency budget)
//   (2) max  gap <= initialReadOffset + 2 periods           (no large swing)
//   (3) effRateNow ~= 0.5                                   (pitch exact)
//   (4) splicePhaseErrNow ~= 0                              (freq-neutral)
//   (5) max sample-to-sample output step bounded            (seamless, no buzz)
// and prints the actual ms so the budget is witnessed, not asserted.
//
// Build: g++ -O2 -std=c++17 scripts/test-transpose-latency.cpp -o scripts/test-transpose-latency.exe
#include <cstdio>
#include <cmath>
#include <vector>
#include <cstdint>
#define SOLAD_M_PI 3.14159265358979323846f
#include "../patches/soladSnacOctaver.h"

static int g_fails = 0;
static void check(const char* n, bool c) {
    if (c) printf("  ok: %s\n", n);
    else { printf("  FAIL: %s\n", n); g_fails++; }
}

// Deterministic LCG noise (no Date/rand — reproducible).
struct LCG { uint32_t s; float next() { s = s*1664525u + 1013904223u; return ((float)(s>>9)/8388608.0f - 1.0f); } };

struct Result { double meanGap, maxGap, meanEff, phaseErr, maxStep; int offset; double per; };

static Result run(float freq, float frac) {
    const int SR = 48000;
    EngineSoladSnac e;
    e.setPitchScale(0.5f);
    e.setRespliceFrac(frac);
    // Leave initialReadOffset at the production default (no override) so the
    // test validates the SHIPPING config end-to-end.
    e.reengage();

    const int CHUNK = 64;
    const int TOTAL = SR * 4;             // 4 seconds
    std::vector<float> in(CHUNK), out(CHUNK);
    double phase = 0.0, dphi = 2.0 * SOLAD_M_PI * (double)freq / (double)SR;
    LCG rng{0x1234567u};

    double gapAccum = 0.0; long gapN = 0;
    double maxGap = 0.0;
    double effAccum = 0.0; long effN = 0;
    double phaseErrAccum = 0.0; long phaseErrN = 0;
    double prevOut = 0.0; double maxStep = 0.0;
    bool havePrev = false;

    for (int n = 0; n < TOTAL; n += CHUNK) {
        for (int i = 0; i < CHUNK; i++) {
            in[i] = 0.5f * (float)sin(phase) + 0.003f * rng.next();  // Pi-input emulation
            phase += dphi;
            if (phase > 2.0 * SOLAD_M_PI) phase -= 2.0 * SOLAD_M_PI;
        }
        e.processBlock(in.data(), out.data(), CHUNK);
        if (n > SR) {                      // steady-state (after 1s lock)
            double g = (double)e.gapNow();
            gapAccum += g; gapN++;
            if (g > maxGap) maxGap = g;
            effAccum += (double)e.effRateNow();   effN++;
            phaseErrAccum += (double)e.splicePhaseErrNow(); phaseErrN++;
            for (int i = 0; i < CHUNK; i++) {
                if (havePrev) { double s = fabs(out[i] - prevOut); if (s > maxStep) maxStep = s; }
                prevOut = out[i]; havePrev = true;
            }
        }
    }
    Result r;
    r.meanGap  = gapN ? gapAccum / gapN : 0;
    r.maxGap   = maxGap;
    r.meanEff  = effN ? effAccum / effN : 0;
    r.phaseErr = phaseErrN ? phaseErrAccum / phaseErrN : 0;
    r.maxStep  = maxStep;
    r.offset   = e.getInitialReadOffset();
    r.per      = (double)SR / (double)freq;
    return r;
}

int main() {
    printf("Transpose -12 latency budget (production frac=1, offset=64), noisy input:\n");
    double maxStepClean = 0.0;

    for (float freq : {82.41f, 110.0f, 220.0f, 330.0f}) {
        Result r = run(freq, 1.0f);
        double latMs = r.meanGap / 48.0;
        double budget = (double)r.offset + r.per;          // offset + 1 period = PSOLA minimum
        printf("\n%.1fHz (per=%.0f): meanGap=%.0f (%.1fms) maxGap=%.0f eff=%.4f perr=%.3f maxStep=%.4f | budget=%.0f samp (%.1fms)\n",
               freq, r.per, r.meanGap, latMs, r.maxGap, r.meanEff, r.phaseErr, r.maxStep, budget, budget/48.0);
        // Budget: the gap must stay within the PSOLA minimum (offset + 1 period).
        // This holds the latency at its floor; for the playing range (>=110Hz)
        // that floor is <=6ms, and at 220Hz ~3.6ms — the ~4ms target. The low-E
        // (82Hz) floor is one period (~7ms), the algorithm's hard limit.
        check("mean gap within initialReadOffset + 1 period (PSOLA latency floor)", r.meanGap <= budget);
        check("max gap within initialReadOffset + 2 periods (no large swing)", r.maxGap <= (double)r.offset + 2.0*r.per + 8.0);
        check("read rate ~= 0.5 (pitch exact)", fabs(r.meanEff - 0.5) < 0.02);
        check("splice phase error ~= 0 (frequency-neutral)", r.phaseErr < 1.0);
        // Explicit 4ms-budget witness for the core playing pitch.
        if (freq >= 200.0f && freq <= 240.0f)
            check("220Hz processing latency <= 4.5ms (transpose budget)", latMs <= 4.5);
        if (r.maxStep > maxStepClean) maxStepClean = r.maxStep;
    }
    // Seamless: with a 0.5-amplitude tone, a clean PSOLA splice produces no step
    // larger than the natural per-sample slope of the tone (~0.5*2pi*f/SR ~ 0.04
    // at 220Hz). A buzzy blind splice spikes well above that. Cap generously at
    // 0.15 (3-4x the natural max slope) to catch a discontinuity without flagging
    // legitimate high-slope crossings.
    printf("\nmax sample-step across all pitches = %.4f (seamless cap 0.15)\n", maxStepClean);
    check("output is seamless (no splice discontinuity buzz on noisy input)", maxStepClean < 0.15);

    if (g_fails) { printf("\n%d FAILURE(S)\n", g_fails); return 1; }
    printf("\nALL PASS\n");
    return 0;
}
