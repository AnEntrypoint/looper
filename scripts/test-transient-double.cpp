// Host test: does the solad engine double transients at non-octave ratios?
// Generates a train of plucked-string attacks, runs through EngineSoladSnac at
// a given scale, counts onsets in input vs output. A doubled transient shows as
// MORE output onsets than input onsets (the resplice replayed an attack).
//
// Build: g++ -std=c++17 -O2 -I scripts/engines -o scripts/test-transient-double.exe scripts/test-transient-double.cpp && ./scripts/test-transient-double.exe
#include "engine_solad_snac.h"
#include <vector>
#include <cmath>
#include <cstdio>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// CONTINUOUS sustained tone with periodic re-pluck transients on top — this is
// what real playing looks like and what makes the resplice fire ACROSS an
// attack (the silent-gap version lets the engine coast, hiding the bug).
static std::vector<float> pluckTrain(int sr, float f0, int nPlucks, float gapSec) {
    std::vector<float> x;
    int gapN = (int)(gapSec * sr);
    double ph = 0.0, dph = 2.0*M_PI*f0/sr;
    for (int p = 0; p < nPlucks; p++) {
        for (int n = 0; n < gapN; n++) {
            float t = (float)n / sr;
            float sustain = 0.25f;                       // never goes silent
            float attack  = 0.75f * expf(-t * 8.0f);     // re-pluck transient
            float amp = sustain + attack;
            float s = 0.0f; double hp = ph;
            for (int h = 1; h <= 6; h++) { s += (1.0f/h) * (float)sin(hp); hp += dph; }
            x.push_back(0.5f * amp * s);
            ph += dph;
        }
    }
    return x;
}

// Count onsets: rising envelope edges where fast-env crosses 3x slow-env.
static int countOnsets(const std::vector<float>& y, int sr) {
    float es = 0, ef = 0, efp = 0; int cool = 0, onsets = 0;
    for (float s : y) {
        float a = fabsf(s);
        es += (a - es) * 0.0008f;
        ef += (a - ef) * 0.02f;
        float d = ef - efp; efp = ef;
        if (cool > 0) { cool--; continue; }
        if (ef > es * 3.0f && ef > 0.03f && d > 0.003f) { onsets++; cool = (int)(0.15f*sr); }
    }
    return onsets;
}

static void run(const char* label, float scale) {
    const int sr = 48000;
    std::vector<float> in = pluckTrain(sr, 196.0f, 8, 0.5f);  // G3, 8 plucks 0.5s apart
    std::vector<float> out;
    EngineSoladSnac::run(in, out, sr, scale);
    int inOn  = countOnsets(in, sr);
    int outOn = countOnsets(out, sr);
    printf("%-10s scale=%.3f : in_onsets=%d out_onsets=%d  %s\n",
           label, scale, inOn, outOn,
           outOn > inOn ? "DOUBLED (FAIL)" : (outOn < inOn ? "MISSED" : "clean (PASS)"));
}

int main() {
    run("-12",  0.5f);
    run("-5",   powf(2.0f, -5.0f/12.0f));   // ~0.561
    run("-7",   powf(2.0f, -7.0f/12.0f));   // ~0.667
    run("-3",   powf(2.0f, -3.0f/12.0f));   // ~0.841
    run("0",    1.0f);
    return 0;
}
