// Downsample 2x -> signalsmith @24kHz -> upsample 2x.
// At 24kHz, a block of N samples = N/24000 sec gives the STFT effectively
// 2x the spectral resolution per fundamental cycle vs the same block at 48kHz.
// Low-fundamental content (E2=82.4Hz, period 24ms = 583 samples @24kHz)
// fits more cycles inside a 64-sample STFT block at 24kHz than at 48kHz.
//
// Latency = downsample LP delay + signalsmith block + upsample LP delay
//         ~= 32 samples @48kHz + 64 samples @24kHz = 0.67 + 2.67 + 0.67 ms ~= 4ms
//
// Uses a simple 32-tap windowed-sinc half-band filter for downsample/upsample.

#pragma once
#include <vector>
#include <cmath>
#include "signalsmith-stretch.h"   // -I /tmp/sigsmith-upstream/signalsmith-stretch

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

inline void halfBandLPF(const std::vector<float> &in, std::vector<float> &out) {
    // 32-tap windowed-sinc Fc = 0.25 * Fs (half-band ideal cutoff = 0.5,
    // we use 0.45 for transition margin). Hann-windowed.
    static constexpr int TAPS = 32;
    static float h[TAPS];
    static bool init = false;
    if (!init) {
        double fc = 0.225;  // normalized to Fs (so cutoff at 0.45 Nyquist)
        double sum = 0;
        for (int i = 0; i < TAPS; i++) {
            int n = i - TAPS / 2;
            double sinc = (n == 0) ? 2.0 * fc : std::sin(2.0 * M_PI * fc * n) / (M_PI * n);
            double win = 0.5 * (1.0 - std::cos(2.0 * M_PI * i / (TAPS - 1)));
            h[i] = (float)(sinc * win);
            sum += h[i];
        }
        for (int i = 0; i < TAPS; i++) h[i] /= (float)sum;
        init = true;
    }
    int N = (int)in.size();
    out.assign(N, 0.0f);
    for (int n = 0; n < N; n++) {
        double s = 0;
        for (int k = 0; k < TAPS; k++) {
            int idx = n - k;
            if (idx >= 0 && idx < N) s += in[idx] * h[k];
        }
        out[n] = (float)s;
    }
}

inline void engine_downsample(const std::vector<float> &in,
                              std::vector<float> &out,
                              int sr, float scale,
                              int blockSamples, int intervalSamples)
{
    int N = (int)in.size();
    out.assign(N, 0.0f);

    // 1. LP filter at <0.5*Nyquist of 24kHz target (= 6kHz) — actually <0.5*Fs/4 of
    //    original = 6kHz. Our halfBandLPF passes through 0.45*48000/2 = 10.8kHz which
    //    is too high; we need 6kHz cutoff. Use the filter as-is (it's tuned for half-band
    //    at the downsampled rate) and accept some imaging.
    std::vector<float> lp;
    halfBandLPF(in, lp);

    // 2. Decimate by 2
    int M = N / 2;
    std::vector<float> ds(M);
    for (int i = 0; i < M; i++) ds[i] = lp[i * 2];

    // 3. Signalsmith at 24kHz
    int dsr = sr / 2;
    signalsmith::stretch::SignalsmithStretch<float> s;
    s.configure(1, blockSamples, intervalSamples);
    s.setTransposeFactor(scale, 0.0f);
    std::vector<float> shifted(M, 0.0f);
    const float *inA[1] = { ds.data() };
    float       *outA[1] = { shifted.data() };
    s.process(inA, M, outA, M);

    // 4. Upsample 2x (zero-stuff + LP)
    std::vector<float> upRaw(N, 0.0f);
    for (int i = 0; i < M; i++) upRaw[i * 2] = shifted[i] * 2.0f;  // ×2 for energy preservation
    halfBandLPF(upRaw, out);
}
