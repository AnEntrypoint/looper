// Tremolo / amplitude-modulation witness for the solad-snac octaver.
// Feeds a sustained sine through the engine at -12, extracts the output
// envelope, and reports modulation depth + dominant modulation rate.
//
// Tremolo manifests as a periodic envelope ripple. A clean octaver holds a
// near-flat envelope on a sustained tone, so modulation depth -> ~0.
//
//   build: g++ -std=c++17 -O2 -I scripts/engines -o scripts/test-tremolo.exe scripts/test-tremolo.cpp
//   run:   ./scripts/test-tremolo.exe
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include "engine_solad_snac.h"

static const double PI = 3.14159265358979323846;

struct Result { double depth_pct; double rate_hz; double mean_env; };

// Envelope = one-pole-smoothed rectified signal. Then measure ripple.
static Result analyze(const std::vector<float>& out, int sr, double skipSec) {
    int skip = (int)(skipSec * sr);
    // One-pole envelope follower, ~15ms attack/release (slow enough to ignore
    // the carrier, fast enough to catch a sub-30Hz tremolo).
    double tc = 1.0 / (0.015 * sr);
    std::vector<double> env(out.size(), 0.0);
    double e = 0.0;
    for (size_t i = 0; i < out.size(); i++) {
        double a = std::fabs((double)out[i]);
        e += (a - e) * tc;
        env[i] = e;
    }
    // Steady-state window.
    double mean = 0.0; int n = 0;
    for (size_t i = skip; i < env.size(); i++) { mean += env[i]; n++; }
    if (n == 0) return {0,0,0};
    mean /= n;
    if (mean < 1e-9) return {0,0,mean};
    // Modulation depth = (max-min)/mean over steady-state.
    double mn = 1e30, mx = -1e30;
    for (size_t i = skip; i < env.size(); i++) { mn = std::min(mn, env[i]); mx = std::max(mx, env[i]); }
    double depth = (mx - mn) / mean * 100.0;
    // Dominant modulation rate: Goertzel sweep of the (DC-removed) envelope
    // over 1..30 Hz, pick the strongest bin.
    double bestMag = 0, bestF = 0;
    for (double f = 1.0; f <= 30.0; f += 0.5) {
        double w = 2.0 * PI * f / sr;
        double cc = 2.0 * std::cos(w);
        double q1 = 0, q2 = 0;
        for (size_t i = skip; i < env.size(); i++) {
            double s = (env[i] - mean) + cc * q1 - q2;
            q2 = q1; q1 = s;
        }
        double mag = std::sqrt(q1*q1 + q2*q2 - q1*q2*cc);
        if (mag > bestMag) { bestMag = mag; bestF = f; }
    }
    return { depth, bestF, mean };
}

int main() {
    const int SR = 48000;
    const double DUR = 4.0;
    int N = (int)(SR * DUR);
    struct Case { const char* name; double freq; };
    Case cases[] = { {"sine_82.4", 82.4}, {"sine_110", 110.0}, {"sine_196", 196.0} };
    printf("solad-snac -12 amplitude-modulation (tremolo) witness\n");
    printf("%-12s  depth%%   rate(Hz)  mean_env\n", "case");
    for (auto& c : cases) {
        std::vector<float> in(N), out(N);
        for (int i = 0; i < N; i++) in[i] = 0.5f * (float)std::sin(2.0*PI*c.freq*i/SR);
        EngineSoladSnac e;
        e.setPitchScale(0.5f);
        const int CHUNK = 64;
        for (int i = 0; i < N; i += CHUNK) {
            int n = std::min(CHUNK, N - i);
            e.processBlock(&in[i], &out[i], n);
        }
        Result r = analyze(out, SR, 1.0);  // skip 1s warmup
        printf("%-12s  %6.2f   %6.1f    %.4f\n", c.name, r.depth_pct, r.rate_hz, r.mean_env);
    }
    return 0;
}
