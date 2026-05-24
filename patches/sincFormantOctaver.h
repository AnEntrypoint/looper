#pragma once
// sinc-delay-192 + expressive post-EQ — stateful block-processing class for
// the firmware audio path.
//
// Per-block: feed input samples, emit pitch-shifted samples with optional
// formant coloration. Engine ALWAYS runs at its native 4 ms delay even when
// not pitch-shifting (scale=1.0 = pure delay-line read of dry signal), so
// engaging/disengaging the octaver does not toggle latency on/off — only the
// wet/dry crossfade moves. This eliminates the "the sound jumps in time when
// I turn on -12" artifact.
//
// Smoothing:
//   - m_targetScale → m_scale via 1-pole low-pass (~10 ms time constant).
//     Avoids the read-rate jump click when pitch changes mid-stream.
//   - m_engaged → m_wetMix linear ramp over ~30 ms. Crossfade between the
//     undelayed-but-time-aligned dry tap and the post-EQ wet path.
//
// Runtime knobs:
//   setPitchScale(scale)              — target rate (0.5 = -12). Smoothed.
//   setEngaged(bool)                  — wet/dry crossfade.
//   setFormant(b, r, freqHz)          — see top-level comment block.

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
        m_resonance  = 0.0f;
        m_freq       = 800.0f;
        m_scale       = 1.0f;
        m_targetScale = 1.0f;
        m_wetMix      = 0.0f;
        m_engaged     = false;
        designShelf();
        designPeak();
    }

    void setPitchScale(float s)              { m_targetScale = s; }
    void setEngaged(bool on)                  { m_engaged = on; }

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
            // Write dry.
            m_dl[m_wr & MASK] = in[i];
            m_wr++;

            // Smooth scale toward target (1-pole, ~10 ms).
            const float SCALE_TC = 1.0f / 480.0f;  // 10 ms @ 48k
            m_scale += (m_targetScale - m_scale) * SCALE_TC;

            // Wet read: sinc-interpolated from m_rd.
            double pos = m_rd;
            int base = (int)pos;
            if (pos < 0) base = (int)pos - 1;
            double frac = pos - (double)base;
            float wet = 0.0f;
            for (int k = 0; k < SINC_TAPS; k++) {
                int idx = base + k - SINC_HALF + 1;
                double x = (double)(k - SINC_HALF + 1) - frac;
                double s = (x < 1e-9 && x > -1e-9) ? 1.0
                         : sin(SFP_M_PI * x) / (SFP_M_PI * x);
                double w = 0.5 - 0.5 * cos(2.0 * SFP_M_PI * ((double)k + frac) / (double)(SINC_TAPS - 1));
                wet += m_dl[(uint32_t)idx & MASK] * (float)(s * w);
            }
            // Post-EQ.
            wet = m_shelf.process(wet);
            wet = m_peak.process(wet);

            // Dry tap: read same delay-line at scale=1.0 from a position
            // INITIAL_READ_OFFSET behind write. This is the pure-dry signal
            // delayed by exactly the engine latency so crossfade is phase-
            // coherent (no comb when wetMix is partial).
            uint32_t dryIdx = (m_wr - INITIAL_READ_OFFSET) & MASK;
            float dry = m_dl[dryIdx];

            // Crossfade wetMix toward engaged target (linear, ~30 ms).
            const float MIX_STEP = 1.0f / 1440.0f;  // 30 ms @ 48k
            float target = m_engaged ? 1.0f : 0.0f;
            if (m_wetMix < target) m_wetMix = (m_wetMix + MIX_STEP < target) ? m_wetMix + MIX_STEP : target;
            else if (m_wetMix > target) m_wetMix = (m_wetMix - MIX_STEP > target) ? m_wetMix - MIX_STEP : target;

            out[i] = wet * m_wetMix + dry * (1.0f - m_wetMix);

            // Advance read pointer at the (smoothed) current rate.
            m_rd += (double)m_scale;
            double gap = (double)m_wr - m_rd;
            // Down-shift: read falls behind, snap forward when delay-line full.
            if (gap > (double)(DL - 32)) m_rd = (double)m_wr - (double)INITIAL_READ_OFFSET;
            // Up-shift: read catches write, snap back by one initial-offset.
            if (gap < 16.0) m_rd = (double)m_wr - (double)INITIAL_READ_OFFSET;
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
    static constexpr float TILT_FC = 500.0f;   // pivot for tilt EQ
    static constexpr float PEAK_Q = 2.0f;

    float    m_dl[DL];
    uint32_t m_wr = INITIAL_READ_OFFSET;
    double   m_rd = 0.0;
    float    m_scale       = 1.0f;
    float    m_targetScale = 1.0f;
    float    m_wetMix      = 0.0f;
    bool     m_engaged     = false;
    float    m_brightness  = 0.0f;
    float    m_resonance   = 0.0f;
    float    m_freq        = 800.0f;
    Biquad   m_shelf;
    Biquad   m_peak;

    // Tilt EQ centred at TILT_FC: brightness>0 tilts spectrum brighter
    // (bass down + treble up), brightness<0 tilts darker. Implemented as a
    // single RBJ high-shelf at TILT_FC with gain = brightness * ±18 dB,
    // BUT with the cooked output overall-gain-compensated so unity input
    // gives unity output at TILT_FC regardless of brightness — avoids
    // sudden loudness jumps as the knob sweeps. We also lowered the pivot
    // to 500 Hz so down-octaved low-E (~40-200 Hz dominant) sits squarely
    // BELOW the pivot and gets cut/boosted opposite to the treble.
    void designShelf() {
        float gainDb = m_brightness * 18.0f;
        float A = powf(10.0f, gainDb / 40.0f);
        float w0 = 2.0f * SFP_M_PI * TILT_FC / SR;
        float cos_w0 = cosf(w0);
        float sin_w0 = sinf(w0);
        float alpha = sin_w0 * 0.5f * 1.41421356f;  // S = 1
        float beta  = 2.0f * sqrtf(A) * alpha;
        float A_p1  = A + 1.0f;
        float A_m1  = A - 1.0f;
        float b0 =        A * (A_p1 + A_m1 * cos_w0 + beta);
        float b1 = -2.0f * A * (A_m1 + A_p1 * cos_w0);
        float b2 =        A * (A_p1 + A_m1 * cos_w0 - beta);
        float a0 =             A_p1 - A_m1 * cos_w0 + beta;
        float a1 =      2.0f * (A_m1 - A_p1 * cos_w0);
        float a2 =             A_p1 - A_m1 * cos_w0 - beta;
        // Compensate so |H(TILT_FC)|=1 — divide all b by sqrt(A) (asymptotic
        // shelf midpoint gain).
        float comp = 1.0f / sqrtf(A);
        m_shelf.b0 = b0/a0 * comp; m_shelf.b1 = b1/a0 * comp; m_shelf.b2 = b2/a0 * comp;
        m_shelf.a1 = a1/a0;         m_shelf.a2 = a2/a0;
    }

    void designPeak() {
        float gainDb = m_resonance * 12.0f;
        float A = powf(10.0f, gainDb / 40.0f);
        float w0 = 2.0f * SFP_M_PI * m_freq / SR;
        float alpha = sinf(w0) / (2.0f * PEAK_Q);
        float cos_w0 = cosf(w0);
        float b0 = 1.0f + alpha * A;
        float b1 = -2.0f * cos_w0;
        float b2 = 1.0f - alpha * A;
        float a0 = 1.0f + alpha / A;
        float a1 = -2.0f * cos_w0;
        float a2 = 1.0f - alpha / A;
        m_peak.b0 = b0/a0; m_peak.b1 = b1/a0; m_peak.b2 = b2/a0;
        m_peak.a1 = a1/a0; m_peak.a2 = a2/a0;
    }
};
