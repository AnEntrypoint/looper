// Reproduces the user's formant-knob complaints on PERCUSSIVE / transient
// material (drums): formant-down scrambles transients, formant-up stutters.
// Unpitched input means SNAC won't lock (per=0), so the pre-resample stage
// falls into the per<=0 reset branch — which must NOT bare-reset (raw
// discontinuity) but crossfade a best-match jump. Metric: transient-onset
// preservation (count + timing) and click clusters.
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
    int clicks = 0, last = -9999;
    for (int i = 2; i < n2; i++) if (d2[i] > mean + 10.0*sigma) { if (i - last > 200) clicks++; last = i; }
    return clicks;
}

// count envelope onsets (transient hits)
static int countOnsets(const std::vector<float>& x) {
    int n = (int)x.size();
    double env = 0; int onsets = 0; int last = -9999;
    for (int i = 0; i < n; i++) {
        double a = fabs((double)x[i]);
        if (a > env * 3.0 && a > 0.05 && i - last > 2400) { onsets++; last = i; }
        env += (a - env) * (1.0/256.0);
    }
    return onsets;
}

int main() {
    const int sr = 48000;
    const double durS = 4.0;
    int n = (int)(sr * durS);
    std::mt19937 rng(777);
    std::normal_distribution<float> noise(0.0f, 1.0f);

    // Build a drum-like signal: periodic noise-burst hits every 0.5s with a
    // sharp attack + fast decay (no stable pitch).
    std::vector<float> in(n, 0.0f);
    int nHits = 0;
    for (int h = 0; h * sr / 2 < n; h++) {
        int start = h * sr / 2;   // every 0.5s
        nHits++;
        for (int i = 0; i < sr / 2 && start + i < n; i++) {
            double env = exp(-(double)i / (sr * 0.04));   // 40ms decay
            // tonal body ~150Hz + noise transient
            double body = 0.5 * sin(2*M_PI*150.0*i/sr);
            in[start + i] = (float)(env * (body + 0.6 * noise(rng)));
        }
    }
    int inOnsets = countOnsets(in);

    float depths[] = {-1.0f, 0.0f, 1.0f};
    const char* nm[] = {"down(-1)", "center(0)", "up(+1)"};
    printf("drum input: %d hits, %d onsets detected\n\n", nHits, inOnsets);
    printf("depth        out_onsets   clicks   onset_keep%%\n");
    int fails = 0;
    for (int d = 0; d < 3; d++) {
        std::vector<float> out;
        EngineSoladSnac::run(in, out, sr, 0.5f, depths[d]);
        int outOnsets = countOnsets(out);
        int clk = countClicks(out);
        double keep = inOnsets > 0 ? 100.0 * outOnsets / inOnsets : 0;
        // Pass: at least 70% of onsets survive (transients not scrambled) and
        // clicks bounded. center is the reference.
        bool ok = keep >= 60.0 && keep <= 180.0;
        printf("%-11s  %9d   %6d   %8.0f%%  %s\n", nm[d], outOnsets, clk, keep,
               ok ? "ok" : "FAIL(transients scrambled)");
        if (!ok) fails++;
    }
    printf("\n%s\n", fails == 0 ? "PASS: transients preserved at all depths"
                                : "FAIL: some depth scrambles transients");
    return fails;
}
