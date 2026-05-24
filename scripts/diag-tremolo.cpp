// Diagnostic: dump the engine's internal gap/period/splice + output envelope
// over time at -12 on a sustained sine, to identify the tremolo mechanism.
// We can't see private members, so we re-derive the envelope and correlate
// against the output sample stream, and we measure the per-period output
// energy to expose the amplitude ripple directly.
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include "engine_solad_snac.h"

static const double PI = 3.14159265358979323846;

int main(int argc, char** argv) {
    const int SR = 48000;
    double freq = (argc > 1) ? atof(argv[1]) : 82.4;
    double DUR = 3.0;
    int N = (int)(SR * DUR);
    std::vector<float> in(N), out(N);
    for (int i = 0; i < N; i++) in[i] = 0.5f * (float)std::sin(2.0*PI*freq*i/SR);
    EngineSoladSnac e;
    e.setPitchScale(0.5f);
    const int CHUNK = 64;
    for (int i = 0; i < N; i += CHUNK) {
        int n = std::min(CHUNK, N - i);
        e.processBlock(&in[i], &out[i], n);
    }
    // Output fundamental is freq/2. Measure RMS in non-overlapping windows of
    // ~2 output cycles each, print the windowed RMS so the ripple is visible.
    double outPer = SR / (freq / 2.0);
    int win = (int)(outPer * 2.0);
    printf("# freq=%.1f outPer=%.1f win=%d\n", freq, outPer, win);
    printf("# t_ms  rms\n");
    int skip = SR; // skip warmup
    for (int i = skip; i + win < N; i += win) {
        double s = 0;
        for (int j = 0; j < win; j++) s += (double)out[i+j]*out[i+j];
        double rms = std::sqrt(s/win);
        printf("%.1f %.4f\n", 1000.0*i/SR, rms);
    }
    return 0;
}
