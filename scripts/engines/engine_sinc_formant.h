#pragma once
// sinc-delay-192 + expressive post-EQ for guitar octaver formant control.
//
// Rationale: LPC source-filter for monophonic guitar (vs voice) introduces
// instability with no real benefit — guitar "formant" character is mostly
// (a) string body resonance (narrow peak ~ 1-3 kHz), (b) low-end roll-off
// when down-shifted (the original 80-200 Hz harmonics become 40-100 Hz),
// (c) overall brightness vs darkness. Three knobs covering those axes give
// expressive control on top of a clean octaver without messing with phase:
//
//   formantBrightness ∈ [-1, +1]
//       Shelving EQ above 800 Hz. -1 = dark/woolly (-12 dB shelf),
//       0 = neutral, +1 = bright/chimey (+12 dB shelf).
//
//   formantResonance ∈ [0, 1]
//       Peaking EQ centered at formantFreq with Q=2. 0 = off, 1 = +12 dB peak.
//       Gives a vocal/wah-like formant character that follows the player.
//
//   formantFreq ∈ [300, 3000] Hz
//       Center frequency of the resonance peak. Sweepable for wah effect.
//
// All three are RT-modulatable per block. Latency = sinc-delay only (4 ms).
// The post-EQ is biquads — instantaneous, no added delay.

#include <vector>
#include <cmath>
#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace sinc_formant_detail {

struct Biquad {
    float b0=1, b1=0, b2=0, a1=0, a2=0;
    float z1=0, z2=0;
    inline float process(float x) {
        float y = b0*x + z1;
        z1 = b1*x - a1*y + z2;
        z2 = b2*x - a2*y;
        return y;
    }
    void reset() { z1 = z2 = 0; }
};

inline void designHighShelf(Biquad& bq, float sr, float fc, float gainDb) {
    float A = std::pow(10.0f, gainDb / 40.0f);
    float w0 = 2.0f * (float)M_PI * fc / sr;
    float S = 1.0f;
    float alpha = std::sin(w0) / 2.0f * std::sqrt((A + 1.0f/A) * (1.0f/S - 1.0f) + 2.0f);
    float cos_w0 = std::cos(w0);
    float beta = 2.0f * std::sqrt(A) * alpha;
    float b0 = A*((A+1) + (A-1)*cos_w0 + beta);
    float b1 = -2*A*((A-1) + (A+1)*cos_w0);
    float b2 = A*((A+1) + (A-1)*cos_w0 - beta);
    float a0 = (A+1) - (A-1)*cos_w0 + beta;
    float a1 = 2*((A-1) - (A+1)*cos_w0);
    float a2 = (A+1) - (A-1)*cos_w0 - beta;
    bq.b0 = b0/a0; bq.b1 = b1/a0; bq.b2 = b2/a0;
    bq.a1 = a1/a0; bq.a2 = a2/a0;
}

inline void designPeaking(Biquad& bq, float sr, float fc, float Q, float gainDb) {
    float A = std::pow(10.0f, gainDb / 40.0f);
    float w0 = 2.0f * (float)M_PI * fc / sr;
    float alpha = std::sin(w0) / (2.0f * Q);
    float cos_w0 = std::cos(w0);
    float b0 = 1 + alpha*A;
    float b1 = -2*cos_w0;
    float b2 = 1 - alpha*A;
    float a0 = 1 + alpha/A;
    float a1 = -2*cos_w0;
    float a2 = 1 - alpha/A;
    bq.b0 = b0/a0; bq.b1 = b1/a0; bq.b2 = b2/a0;
    bq.a1 = a1/a0; bq.a2 = a2/a0;
}

} // namespace sinc_formant_detail

// Main entry. Test helper that runs sinc-delay + post-EQ over a whole
// buffer with fixed knob settings.
inline void engine_sinc_formant(const std::vector<float>& in,
                                std::vector<float>& out,
                                int sr, float scale,
                                int initialReadOffset,
                                float formantBrightness = 0.0f,  // -1..+1
                                float formantResonance  = 0.0f,  //  0..1
                                float formantFreq       = 800.0f)
{
    using namespace sinc_formant_detail;

    constexpr int DL = 32768;
    constexpr int MASK = DL - 1;
    constexpr int SINC_TAPS = 16;
    constexpr int SINC_HALF = SINC_TAPS / 2;

    int N = (int)in.size();
    out.assign(N, 0.0f);

    static float dl[DL];
    for (int i = 0; i < DL; i++) dl[i] = 0;
    uint32_t wr = (uint32_t)initialReadOffset;
    double rd = 0.0;

    // Design post-EQ biquads once (could be re-designed each block at run-time
    // for expressive modulation).
    Biquad shelf, peak;
    designHighShelf(shelf, (float)sr, 800.0f, formantBrightness * 12.0f);
    designPeaking(peak, (float)sr, formantFreq, 2.0f, formantResonance * 12.0f);

    for (int n = 0; n < N; n++) {
        dl[wr & MASK] = in[n];
        wr++;

        double pos = rd;
        int base = (int)std::floor(pos);
        double frac = pos - base;
        float v = 0;
        for (int k = 0; k < SINC_TAPS; k++) {
            int idx = base + k - SINC_HALF + 1;
            double x = (k - SINC_HALF + 1) - frac;
            double s = (std::abs(x) < 1e-9) ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
            double win = 0.5 * (1.0 - std::cos(2.0 * M_PI * (k + frac) / (SINC_TAPS - 1)));
            v += dl[(uint32_t)idx & MASK] * (float)(s * win);
        }

        // Post-EQ chain.
        float y = v;
        y = shelf.process(y);
        y = peak.process(y);
        out[n] = y;

        rd += scale;
        double gap = (double)wr - rd;
        if (gap > DL - 32) rd = (double)wr - initialReadOffset;
    }
}
