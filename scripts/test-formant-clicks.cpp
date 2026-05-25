// Exercises EngineSoladSnac's formant pre-resample stage at min/center/max
// depth on real-emulating noisy harmonic input and counts clicks via
// 2nd-difference (jerk) outliers. The fix under test gives the pre-resample
// wrap a value+slope-matched integer-period crossfade (mirroring the main
// splice) so wraps stay click-free on consecutive-periods-differ input.
// Dead-center (depth=0) is the bypass path and is the click-free reference;
// after the fix, depth=-1 and depth=+1 click counts must approach the
// depth=0 floor instead of spiking.
#include "engines/engine_solad_snac.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <random>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int countClicks(const std::vector<float>& out) {
    int n2 = (int)out.size();
    if (n2 < 3) return 0;
    std::vector<double> d2(n2, 0.0);
    double mean = 0;
    for (int i = 2; i < n2; i++) { d2[i] = fabs((double)out[i] - 2.0*out[i-1] + out[i-2]); mean += d2[i]; }
    mean /= (n2 - 2);
    double var = 0; for (int i = 2; i < n2; i++) { double e = d2[i]-mean; var += e*e; }
    double sigma = sqrt(var / (n2 - 2));
    // Count CLUSTERS (perceptual clicks), not raw outlier samples: one click
    // spans several samples, so dedupe outliers within 200 samples (~4ms).
    int clicks = 0, last = -9999;
    for (int i = 2; i < n2; i++) if (d2[i] > mean + 10.0*sigma) {
        if (i - last > 200) clicks++;
        last = i;
    }
    return clicks;
}

int main() {
    const int sr = 48000;
    const double durS = 6.0;
    int n = (int)(sr * durS);
    double inFreqs[] = {82.41, 110.0, 146.83, 220.0};
    float depths[] = {-1.0f, 0.0f, 1.0f};
    std::mt19937 rng(12345);
    std::normal_distribution<float> jit(0.0f, 0.003f);

    printf("note_in(Hz)   depth=-1   depth=0(ref)   depth=+1\n");
    int fails = 0;
    for (double f : inFreqs) {
        std::vector<float> in(n);
        for (int i = 0; i < n; i++) {
            double t = (double)i / sr;
            float v = 0.6f * sinf(2*M_PI*f*t)
                    + 0.25f * sinf(2*M_PI*2*f*t)
                    + 0.12f * sinf(2*M_PI*3*f*t);
            v += jit(rng);
            in[i] = v;
        }
        int c[3];
        for (int d = 0; d < 3; d++) {
            std::vector<float> out;
            EngineSoladSnac::run(in, out, sr, 0.5f, depths[d]);
            c[d] = countClicks(out);
        }
        // The fix passes if extreme-depth clicks are within 4x the center-depth
        // floor (a blind-jump regression spikes them 10-50x). Center floor can
        // be 0; allow a small absolute slack so 0 -> a few is not a failure.
        int ref = c[1];
        int budget = (ref > 0 ? ref * 4 : 0) + 20;
        bool ok = (c[0] <= budget) && (c[2] <= budget);
        printf("%9.2f    %6d     %8d       %6d   %s\n", f, c[0], c[1], c[2],
               ok ? "ok" : "FAIL(extreme clicks spike)");
        if (!ok) fails++;
    }
    printf("\n%s\n", fails == 0 ? "PASS: extreme-depth clicks near center floor"
                                : "FAIL: extreme-depth clicks spike vs center");
    return fails;
}
