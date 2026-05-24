#pragma once
// sinc-delay-192 + expressive post-EQ — stateful block-processing class for
// the firmware audio path. See scripts/engines/engine_sinc_formant.h for the
// host-side single-shot version used in A/B rendering.
//
// Per-block: feed input samples, emit shifted samples. Latency = 192 samples
// (sinc-delay initial read offset) = 4 ms @ 48 kHz.
//
// Runtime knobs (call setFormant before block):
//   brightness ∈ [-1, +1]  — high-shelf gain at 800 Hz (-12 dB .. +12 dB)
//   resonance  ∈ [ 0,  1]  — peaking EQ at formantFreq (Q=2, 0..+12 dB)
//   freq       ∈ [300, 3000] Hz — center of resonance peak
//
// Defaults give bit-identical sinc-delay-192 output (no EQ).

#include <math.h>
#include <stdint.h>

#ifndef SFP_M_PI
#define SFP_M_PI 3.14159265358979323846f
#endif

class SincFormantOctaver {
public:
    SincFormantOctaver() { reset(); }

    void reset() {
        for (int i = 0; i < DL; i++) m_dl[i] = 0.0f;
        m_wr = INITIAL_READ_OFFSET;
        m_rd = 0.0;
        m_shelf.reset();
        m_peak.reset();
        m_brightness = 0.0f;
        m_resonance = 0.0f;
        m_freq = 800.0f;
        designShelf();
        designPeak();
    }

    void setPitchScale(float s) { m_scale = s; }
    void setFormant(float brightness, float resonance, float freq) {
        bool reShelf = (brightness != m_brightness);
        bool rePeak  = (resonance  != m_resonance) || (freq != m_freq);
        m_brightness = brightness;
        m_resonance  = resonance;
        m_freq       = freq;
        if (reShelf) designShelf();
        if (rePeak)  designPeak();
    }

    void processBlock(const float* in, float* out, int n) {
        for (int i = 0; i < n; i++) {
            m_dl[m_wr & MASK] = in[i];
            m_wr++;
            double pos = m_rd;
            int base = (int)pos;
            if (pos < 0) base = (int)pos - 1;
            double frac = pos - (double)base;
            float v = 0;
            for (int k = 0; k < SINC_TAPS; k++) {
                int idx = base + k - SINC_HALF + 1;
                double x = (k - SINC_HALF + 1) - frac;
                double s = (x < 1e-9 && x > -1e-9) ? 1.0
                         : sin(SFP_M_PI * x) / (SFP_M_PI * x);
                double w = 0.5 - 0.5 * cos(2.0 * SFP_M_PI * (k + frac) / (SINC_TAPS - 1));
                v += m_dl[(uint32_t)idx & MASK] * (float)(s * w);
            }
            float y = v;
            y = m_shelf.process(y);
            y = m_peak.process(y);
            out[i] = y;
            m_rd += (double)m_scale;
            double gap = (double)m_wr - m_rd;
            if (gap > (double)(DL - 32)) m_rd = (double)m_wr - INITIAL_READ_OFFSET;
        }
    }

private:
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

    static const int DL = 32768;
    static const int MASK = DL - 1;
    static const int SINC_TAPS = 16;
    static const int SINC_HALF = SINC_TAPS / 2;
    static const int INITIAL_READ_OFFSET = 192;
    static constexpr float SR = 48000.0f;
    static constexpr float SHELF_FC = 800.0f;
    static constexpr float PEAK_Q = 2.0f;

    float m_dl[DL];
    uint32_t m_wr = INITIAL_READ_OFFSET;
    double m_rd = 0.0;
    float m_scale = 1.0f;
    float m_brightness = 0.0f;
    float m_resonance = 0.0f;
    float m_freq = 800.0f;
    Biquad m_shelf;
    Biquad m_peak;

    void designShelf() {
        float gainDb = m_brightness * 12.0f;
        float A = powf(10.0f, gainDb / 40.0f);
        float w0 = 2.0f * SFP_M_PI * SHELF_FC / SR;
        float alpha = sinf(w0) / 2.0f * sqrtf((A + 1.0f/A) * (1.0f/1.0f - 1.0f) + 2.0f);
        float cos_w0 = cosf(w0);
        float beta = 2.0f * sqrtf(A) * alpha;
        float b0 = A*((A+1) + (A-1)*cos_w0 + beta);
        float b1 = -2*A*((A-1) + (A+1)*cos_w0);
        float b2 = A*((A+1) + (A-1)*cos_w0 - beta);
        float a0 = (A+1) - (A-1)*cos_w0 + beta;
        float a1 = 2*((A-1) - (A+1)*cos_w0);
        float a2 = (A+1) - (A-1)*cos_w0 - beta;
        m_shelf.b0 = b0/a0; m_shelf.b1 = b1/a0; m_shelf.b2 = b2/a0;
        m_shelf.a1 = a1/a0; m_shelf.a2 = a2/a0;
    }

    void designPeak() {
        float gainDb = m_resonance * 12.0f;
        float A = powf(10.0f, gainDb / 40.0f);
        float w0 = 2.0f * SFP_M_PI * m_freq / SR;
        float alpha = sinf(w0) / (2.0f * PEAK_Q);
        float cos_w0 = cosf(w0);
        float b0 = 1 + alpha*A;
        float b1 = -2*cos_w0;
        float b2 = 1 - alpha*A;
        float a0 = 1 + alpha/A;
        float a1 = -2*cos_w0;
        float a2 = 1 - alpha/A;
        m_peak.b0 = b0/a0; m_peak.b1 = b1/a0; m_peak.b2 = b2/a0;
        m_peak.a1 = a1/a0; m_peak.a2 = a2/a0;
    }
};
