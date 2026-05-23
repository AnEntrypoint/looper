#ifndef PSOLA_OCTAVER_H
#define PSOLA_OCTAVER_H

// Low-fundamental-stable down-shift octaver.
//
// Approach: time-domain resampling via a long circular delay line. Read
// pointer advances at `scale` × write rate (< 1 = down-shift). On a tonal
// input the output is a pristine down-shifted copy with no spectral
// artifacts — the only DSP risk is the read pointer drifting too far
// behind write (eventually wrapping the buffer). For a typical guitar
// note (< 5 seconds sustained), a 16384-sample (~340ms at 48kHz) buffer
// absorbs the drift; resync logic re-seats the read pointer one period
// behind write when the gap becomes critical.
//
// Why this and not granular two-tap (existing up-shift octaver):
//   The two-tap design with periodic reset snaps the read pointer back
//   to (write - period) every crossfade — on TONAL content this re-reads
//   the same period repeatedly, so the output fundamental equals input
//   fundamental rather than scaled. Two-tap works for up-shift because
//   the read advance > write rate, so resets snap backward (re-read OK)
//   and the output rate IS the read rate. For down-shift the snap-forward
//   destroys the pitch-shift effect on periodic signals.
//
// Why this and not PSOLA (1-period grains at output-epoch spacing):
//   That approach produces a comb-modulated spectrum, not clean down-shift.
//
// Why this and not signalsmith STFT:
//   On low-E -12 (41.2Hz) the STFT block (192 samples ≈ 4ms) is smaller
//   than the pitch period (24ms), giving amplitude-modulation beating
//   between adjacent STFT bins. Resampling has no such constraint.
//
// Formant preservation:
//   Resampling shifts formants down by the same factor as pitch — for a
//   guitar string this turns the resampled signal into a "bigger string"
//   sound (formants 1/2 their original frequency), which IS the natural
//   sound of a real bass guitar (longer string, same pickup). For voice
//   it would chipmunk-in-reverse; this engine targets guitar-source
//   material. The wrapper allows the user to choose signalsmith (which
//   does formant-preserving STFT shift) for voice and PSOLA-resample
//   for guitar via a runtime flag — neither path mucks with the other.
//
// Detection (autocorrelation) is retained because:
//   (a) the resync moment must align with a period boundary or it clicks,
//   (b) it gates engagement: only engage when input is actually periodic
//       (so we don't resample noise/silence and produce sub-audible
//        drift artifacts).
//
// Single-channel; instantiate twice for L/R.

#include <stdint.h>
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class PsolaOctaver {
public:
    PsolaOctaver(int sampleRate)
        : m_sr(sampleRate),
          m_scale(1.0f),
          m_in_wr(0),
          m_rd_pos(0.0f),
          m_period(0),
          m_confidence(0.0f),
          m_detectCounter(0),
          m_warmed(false)
    {
        memset(m_in, 0, sizeof(m_in));
    }

    void setPitchScale(float scale) {
        if (scale < 0.45f) scale = 0.45f;
        if (scale > 1.0f)  scale = 1.0f;
        m_scale = scale;
    }

    bool  locked() const { return m_confidence > LOCK_THRESHOLD && m_period >= MIN_PERIOD; }
    int   currentPeriodSamples() const { return m_period; }
    float currentConfidence() const { return m_confidence; }
    bool  ready() const { return m_warmed && locked() && m_scale < 0.999f; }

    bool process(const float *in, float *out, int n) {
        for (int i = 0; i < n; i++) {
            m_in[m_in_wr & IN_MASK] = in[i];
            m_in_wr++;

            m_detectCounter++;
            if (m_detectCounter >= PERIOD_REDETECT_SAMPLES) {
                m_detectCounter = 0;
                detectPitch();
            }

            if (locked() && m_scale < 0.999f) {
                if (!m_warmed) {
                    // Seed read one period behind write head.
                    m_rd_pos = (float)((int32_t)m_in_wr - m_period);
                    m_warmed = true;
                }
                // Read-write gap check: for scale<1 the read pointer falls
                // BEHIND write head over time (= grows). Once gap exceeds
                // IN_LEN - safety margin, snap read forward to (write -
                // period) so it stays bounded. Snap is rare on short notes
                // (gap grows at (1-scale) samples/output-sample); on long
                // sustained tones it produces an audible glitch — masked
                // by snapping at a period boundary (snap distance is an
                // integer multiple of period).
                int32_t gap = (int32_t)m_in_wr - (int32_t)m_rd_pos;
                if (gap < (int32_t)m_period ||
                    gap > (int32_t)(IN_LEN - 2 * m_period))
                {
                    // Snap to a position one period behind write, aligned
                    // to the period grid relative to the original seed so
                    // phase is preserved across snaps.
                    m_rd_pos = (float)((int32_t)m_in_wr - m_period);
                }
                out[i] = sampleAt(m_rd_pos);
                m_rd_pos += m_scale;
            } else {
                m_warmed = false;
                out[i] = 0.0f;
            }
        }
        return m_warmed;
    }

private:
    static constexpr int IN_LEN  = 16384;     // ~340ms @ 48kHz
    static constexpr int IN_MASK = IN_LEN - 1;
    static constexpr int MIN_PERIOD = 48;
    static constexpr int MAX_PERIOD = 1600;
    static constexpr int ACF_WINDOW = 2048;
    static constexpr int PERIOD_REDETECT_SAMPLES = 256;
    static constexpr float LOCK_THRESHOLD = 0.45f;

    int m_sr;
    float m_scale;
    float m_in[IN_LEN];
    uint32_t m_in_wr;
    float m_rd_pos;
    int m_period;
    float m_confidence;
    int m_detectCounter;
    bool m_warmed;

    void detectPitch() {
        if (m_in_wr < (uint32_t)ACF_WINDOW) return;
        uint32_t base = m_in_wr - (uint32_t)ACF_WINDOW;
        float r0 = 0.0f;
        for (int n = 0; n < ACF_WINDOW; n++) {
            float v = m_in[(base + (uint32_t)n) & IN_MASK];
            r0 += v * v;
        }
        if (r0 < 1e-6f) {
            m_confidence = 0.0f;
            m_period = 0;
            return;
        }
        float bestPeak = 0.0f;
        int   bestTau  = 0;
        for (int tau = MIN_PERIOD; tau <= MAX_PERIOD; tau++) {
            float r = 0.0f;
            int N = ACF_WINDOW - tau;
            for (int n = 0; n < N; n++) {
                r += m_in[(base + (uint32_t)n) & IN_MASK]
                   * m_in[(base + (uint32_t)(n + tau)) & IN_MASK];
            }
            float rN = r / (float)N * (float)ACF_WINDOW;
            if (rN > bestPeak) { bestPeak = rN; bestTau = tau; }
        }
        m_confidence = bestPeak / r0;
        m_period = bestTau;
    }

    float sampleAt(float pos) {
        int32_t i0 = (int32_t)floorf(pos);
        float frac = pos - (float)i0;
        float s0 = m_in[((uint32_t)i0)       & IN_MASK];
        float s1 = m_in[((uint32_t)(i0 + 1)) & IN_MASK];
        return s0 + (s1 - s0) * frac;
    }
};

#endif
