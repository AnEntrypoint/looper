// Pure resampling via long delay line + windowed-sinc interpolated read.
// Read pointer advances at scale × write rate (< 1.0 = down-shift).
// Latency = initialReadOffset samples.
// On long-held tones the read drifts behind write; periodic zero-cross-
// aligned snap forward to keep buffer bounded with minimal audible click.

#pragma once
#include <vector>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

inline void engine_sinc_delay(const std::vector<float> &in,
                              std::vector<float> &out,
                              int sr, float scale,
                              int initialReadOffset)   // samples = latency
{
    constexpr int DL = 32768;   // 683ms @ 48kHz — plenty of headroom
    constexpr int MASK = DL - 1;
    constexpr int SINC_TAPS = 16;  // 8 taps each side
    constexpr int SINC_HALF = SINC_TAPS / 2;
    static float sincTab[SINC_TAPS];
    static bool init = false;
    if (!init) {
        // Windowed sinc with cutoff fc = 0.5 * Fs (no anti-image; relies on input
        // being pre-bandlimited by the source DAC).
        double sum = 0;
        for (int i = 0; i < SINC_TAPS; i++) {
            int n = i - SINC_HALF + 1;  // shift so center between taps
            double x = (double)n - 0.5;
            double s = (x == 0) ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
            double win = 0.5 * (1.0 - std::cos(2.0 * M_PI * i / (SINC_TAPS - 1)));
            sincTab[i] = (float)(s * win);
            sum += sincTab[i];
        }
        for (int i = 0; i < SINC_TAPS; i++) sincTab[i] /= (float)sum;
        init = true;
    }

    int N = (int)in.size();
    out.assign(N, 0.0f);

    static float dl[DL];
    for (int i = 0; i < DL; i++) dl[i] = 0;
    uint32_t wr = initialReadOffset;
    // Pre-fill delay line with leading silence so read at wr - initialReadOffset = 0
    // pulls 0 (silence) initially.
    double rd = 0.0;  // read position in floating-point samples

    int n_emitted = 0;
    int snapInterval = sr / 4;   // forced snap every ~250ms if needed
    int lastSnap = 0;

    for (int n = 0; n < N; n++) {
        dl[wr & MASK] = in[n];
        wr++;

        // Sinc-interpolated read
        double pos = rd;
        int base = (int)std::floor(pos);
        double frac = pos - base;
        float v = 0;
        // Use the precomputed sincTab as fixed kernel; for fractional reads we'd
        // ideally have a polyphase bank — for simplicity here use linear interp
        // between two adjacent sinc-tap reads. Acceptable for this experiment.
        // Better: compute sinc directly per fractional position.
        for (int k = 0; k < SINC_TAPS; k++) {
            int idx = base + k - SINC_HALF + 1;
            double x = (k - SINC_HALF + 1) - frac;
            double s = (std::abs(x) < 1e-9) ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
            double win = 0.5 * (1.0 - std::cos(2.0 * M_PI * (k + frac) / (SINC_TAPS - 1)));
            v += dl[(uint32_t)idx & MASK] * (float)(s * win);
        }
        out[n] = v;

        rd += scale;

        // Check read-write gap. wr is integer, rd is float.
        double gap = (double)wr - rd;
        if (gap > DL - 32) {
            // Read fell too far behind, snap forward to one initialReadOffset behind write
            // Try to align to next zero-crossing of input within ±period(of fundamental)
            // For simplicity: snap to integer sample one initialReadOffset behind write
            rd = (double)wr - initialReadOffset;
        }
    }
}
