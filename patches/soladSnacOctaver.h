#pragma once
// solad-style + McLeod SNAC pitch shifter for monophonic guitar.
//
// References (May 2026):
//   Katja Vetter, "low latency pitch shifting"   katjaas.nl/pitchshiftlowlatency
//   Katja Vetter, "helmholtz finds the pitch"    katjaas.nl/helmholtz
//   McLeod & Wyvill, "A Smarter Way to Find Pitch" (Tartini paper)
//
// Architecture:
//   1. SNAC pitch tracker on 1024-sample sliding window. 2*r[k]/norm[k]
//      where norm[k] = sum(x[n]^2 + x[n-k]^2). Parabolic interp on the
//      peak for sub-sample period accuracy. Fidelity gate.
//   2. solad delay-line shifter: single circular buffer, read at scale rate.
//      When read pointer drifts outside safe band, jump by INTEGER MULTIPLE
//      of detected period (phase-coherent). Crossfade splice length
//      = jump * max(1, scale * 2).
//   3. Transient detector overlay: rectified-sample-derivative > 6σ rolling
//      threshold. On detection, snap read pointer to live audio (force
//      latency briefly to ~0) and trigger immediate splice crossfade.
//
// Latency:
//   - Algorithmic delay = INITIAL_READ_OFFSET = 192 samples (4ms).
//   - SNAC detection lag = 1024 samples (21ms) but applies retrospectively
//     to splice point choice — does not add to audio latency.
//   - Transient snap overrides briefly to near-zero on attack.

#include <math.h>
#include <stdint.h>
#include <vector>

#ifndef SOLAD_M_PI
#define SOLAD_M_PI 3.14159265358979323846f
#endif

class EngineSoladSnac {
public:
    EngineSoladSnac() { reset(); }

    void reset() {
        for (int i = 0; i < DL; i++) m_dl[i] = 0.0f;
        for (int i = 0; i < PRE_DL; i++) m_preBuf[i] = 0.0f;
        m_preWr = PRE_DL / 2;
        m_preRd = 0.0;
        m_preRate = 1.0f;
        m_formantDepth = 0.0f;
        m_wr = (uint32_t)m_initialReadOffset;
        m_rdA = 0.0;                          // primary read pointer
        m_rdB = 0.0;
        m_useA = true;
        m_xfadeRemain = 0;
        m_xfadeLen = 0;
        m_scale = 1.0f;
        m_targetScale = 1.0f;
        m_period = 256;
        m_periodValid = false;
        m_sinceDetect = 0;
        m_warmup = SNAC_WIN;
        m_envSlow = 0.0f;
        m_envFast = 0.0f;
        m_transCool = 0;
        for (int i = 0; i < SNAC_WIN; i++) m_snacBuf[i] = 0.0f;
        m_snacWr = 0;
    }

    void setPitchScale(float s) { m_targetScale = s; }

    // Live-tunable runtime params (sweep-friendly for empirical tuning).
    // initialReadOffset = engine algorithmic delay (samples). Smaller =
    //   lower latency, higher chance of read catching write under drift.
    void setInitialReadOffset(int samples) {
        if (samples < 32) samples = 32;
        if (samples > DL - 64) samples = DL - 64;
        m_initialReadOffset = samples;
    }
    int  getInitialReadOffset() const { return m_initialReadOffset; }

    // Splice crossfade scale factor over the per-period default.
    //   1.0 = 2*period (default), 0.5 = 1*period, 2.0 = 4*period.
    void setXfadeScale(float s) {
        if (s < 0.25f) s = 0.25f;
        if (s > 4.0f) s = 4.0f;
        m_xfadeScale = s;
    }

    // SNAC fidelity gate ∈ [0.3, 0.95]. Lower = tracks weaker signals,
    // higher = only confident lock. Default 0.7.
    void setFidelityThresh(float f) {
        if (f < 0.30f) f = 0.30f;
        if (f > 0.95f) f = 0.95f;
        m_fidelityThresh = f;
    }

    // Formant depth ∈ [-1, +1]:
    //   d = 0  : natural pitch shift, formants slide with pitch (deep/slow)
    //   d = 1  : formants fully preserved at original pitch (vocal-octave)
    //   d > 1  : formants shift OPPOSITE direction (extreme exaggeration)
    //   d < 0  : formants doubled-down with pitch (huge/monster)
    // Implemented as a pre-resample stage feeding the delay line: pre-rate
    // = pow(pitchScale, -depth) → engine resamples back by pitchScale, net
    // formant shift = pow(pitchScale, 1-depth).
    void setFormantDepth(float d) { m_formantDepth = d; }

    void processBlock(const float* in, float* out, int n) {
        for (int i = 0; i < n; i++) {
            // ---- 0. Pre-resample input for formant depth control ----
            // Write raw input into the formant-prewarp buffer at full rate;
            // read at preRate to produce formant-warped samples for the
            // delay line. Continuous-rate sinc-interpolated read.
            m_preBuf[m_preWr & PRE_MASK] = in[i];
            m_preWr++;
            // preRate = pow(pitchScale, -depth). depth=0 → preRate=1 → no
            // formant warp (natural pitch shift). depth=1 → preRate=1/scale
            // (formants preserved). Smooth onto target.
            float scale = m_scale;
            if (scale < 0.01f) scale = 0.01f;
            float targetPreRate = powf(scale, -m_formantDepth);
            m_preRate += (targetPreRate - m_preRate) * (1.0f / 480.0f);

            // Read pre-buffer at preRate; how many delay-line writes per
            // input sample = 1/preRate average. Maintain m_preRd, only
            // commit to delay line when m_preRd ≤ current write position
            // minus a small safety.
            // Walk preRd at fixed step of 1 output (delay-line) sample per
            // engine sample; preRd reads pre-buffer at variable rate.
            float xWarped = 0.0f;
            {
                double pos = m_preRd;
                int base = (int)pos;
                if (pos < 0) base = (int)pos - 1;
                double frac = pos - (double)base;
                // 8-tap sinc — cheaper than the 16-tap delay-line read.
                const int TAPS = 8, HALF = TAPS/2;
                for (int k = 0; k < TAPS; k++) {
                    int idx = base + k - HALF + 1;
                    double dx = (double)(k - HALF + 1) - frac;
                    double s = (dx < 1e-9 && dx > -1e-9) ? 1.0
                             : sin(SOLAD_M_PI * dx) / (SOLAD_M_PI * dx);
                    double w = 0.5 - 0.5 * cos(2.0 * SOLAD_M_PI * ((double)k + frac)
                                               / (double)(TAPS - 1));
                    xWarped += m_preBuf[(uint32_t)idx & PRE_MASK] * (float)(s * w);
                }
                m_preRd += (double)m_preRate;
                // Drift management on pre-buffer. Hard-snap only when we
                // genuinely cannot recover (gap < 4 or > PRE_DL-16) —
                // those are emergency cases where read would alias write.
                // Within recoverable band, nudge the read rate slightly
                // toward the target gap (= PRE_DL/2) over the next ~10ms
                // so we glide back instead of clicking.
                double pgap = (double)m_preWr - m_preRd;
                const double pgapTarget = (double)(PRE_DL / 2);
                if (pgap < 4.0 || pgap > (double)(PRE_DL - 16)) {
                    m_preRd = (double)m_preWr - pgapTarget;
                } else if (pgap < pgapTarget * 0.5 || pgap > pgapTarget * 1.5) {
                    // Drift bias — preRate slightly off target so gap walks
                    // back toward pgapTarget. Inaudible <0.1% rate shift.
                    float bias = (pgap < pgapTarget) ? -1e-4f : +1e-4f;
                    m_preRate += bias;
                }
            }

            // ---- 1. Ingest the pre-warped sample ----
            float x = xWarped;
            m_dl[m_wr & MASK] = x;
            m_wr++;

            // Mirror into SNAC ring.
            m_snacBuf[m_snacWr] = x;
            m_snacWr = (m_snacWr + 1) % SNAC_WIN;

            // ---- 2. Pitch detect periodically ----
            if (++m_sinceDetect >= SNAC_HOP) {
                m_sinceDetect = 0;
                detectPitch();
            }

            // ---- 3. Transient detector ----
            // Fire only on RISING envelope edge: envFast must (a) exceed
            // envSlow by a wide margin AND (b) be increasing relative to its
            // own previous value. Without (b) the detector latched on
            // sustained energy and fired on every refractory expiry,
            // splicing every ~100 ms (audible click train).
            float ax = fabsf(x);
            m_envSlow += (ax - m_envSlow) * ENV_SLOW_TC;
            m_envFast += (ax - m_envFast) * ENV_FAST_TC;
            float envDeriv = m_envFast - m_envFastPrev;
            m_envFastPrev = m_envFast;
            bool transient = false;
            if (m_transCool > 0) m_transCool--;
            else if (m_envFast > m_envSlow * 3.0f
                     && m_envFast > 0.05f
                     && envDeriv > 0.005f) {
                transient = true;
                m_transCool = TRANS_REFRACTORY;
            }

            // ---- 4. Smooth scale ----
            m_scale += (m_targetScale - m_scale) * (1.0f / 480.0f);

            // ---- 5. Manage read pointers ----
            if (m_warmup > 0) {
                m_warmup--;
                out[i] = 0.0f;
                if (m_useA) m_rdA = (double)(m_wr - m_initialReadOffset);
                else        m_rdB = (double)(m_wr - m_initialReadOffset);
                continue;
            }

            double &rdActive  = m_useA ? m_rdA : m_rdB;
            double &rdPassive = m_useA ? m_rdB : m_rdA;

            // Transient detector currently DISABLED — host A/B showed the
            // snap-to-live splice introduces a 150σ spike at attacks
            // (worse than the smearing it tried to fix). Re-enable once
            // we have a proper short-time spectral-flux detector and a
            // bridge crossfade (not a hard splice) for attack handling.
            // if (transient && m_envSlow > 0.01f) {
            //     triggerSplice(/*toLive=*/true);
            // }
            (void)transient;
            // Drift management — splice if outside safe band. Defer when
            // period detector is unsure (silence / noise tail) — the
            // integer-period snap would land at a phase-incorrect position
            // and click; better to let the gap stretch until we re-lock.
            double gap = (double)m_wr - rdActive;
            bool driftOOB = (gap > (double)(DL - 256) || gap < 8.0);
            if (driftOOB && m_periodValid && m_envSlow > 0.005f) {
                triggerSplice(/*toLive=*/false);
            }
            // Hard escape — if we genuinely cannot recover (reader wrapping
            // off the end of the buffer), splice anyway. Better a click than
            // garbage memory reads.
            if (gap > (double)(DL - 16) || gap < 0.0) {
                triggerSplice(/*toLive=*/false);
            }

            // ---- 6. Read + crossfade ----
            float yA = readSinc(m_rdA);
            float yB = readSinc(m_rdB);
            float w  = (m_xfadeRemain > 0 && m_xfadeLen > 0)
                       ? (float)m_xfadeRemain / (float)m_xfadeLen
                       : 0.0f;
            // w=1 → 100% passive (old reader); w=0 → 100% active (new reader)
            float wActive  = 1.0f - w;
            float wPassive = w;
            // Use cosine fade for smoother amplitude (sum-of-squares = 1)
            wActive  = 0.5f * (1.0f - cosf(SOLAD_M_PI * wActive));
            wPassive = 1.0f - wActive;

            float y = m_useA ? (yA * wActive + yB * wPassive)
                             : (yB * wActive + yA * wPassive);
            out[i] = y;

            // ---- 7. Advance pointers ----
            m_rdA += (double)m_scale;
            m_rdB += (double)m_scale;
            if (m_xfadeRemain > 0) m_xfadeRemain--;
        }
    }

    static void run(const std::vector<float>& in, std::vector<float>& out,
                    int sr, float scale, float formantDepth = 0.0f) {
        EngineSoladSnac e;
        e.setPitchScale(scale);
        e.setFormantDepth(formantDepth);
        out.assign(in.size(), 0.0f);
        const int CHUNK = 64;
        for (size_t i = 0; i < in.size(); i += CHUNK) {
            size_t left = in.size() - i;
            int n = (int)(left < (size_t)CHUNK ? left : (size_t)CHUNK);
            e.processBlock(&in[i], &out[i], n);
        }
    }

private:
    static const int DL = 131072;  // 2.7s at 48k — drift splice fires every
                                   // ~5.5s at -12 (vs 1.35s at DL=32768)
    static const int PRE_DL = 8192;        // 170 ms — formant pre-resample buffer
    static const int PRE_MASK = PRE_DL - 1;
    static const int MASK = DL - 1;
    static const int SINC_TAPS = 16;
    static const int SINC_HALF = SINC_TAPS / 2;
    // 64 samples = 1.33 ms @ 48 kHz. Empirically validated via CC100 sweep
    // on OTG↔OTG loopback (scripts/measure-results/sweep-cc100-*): 0.13%
    // THD at this offset vs 0.32% at 192. The smallest stable offset that
    // doesn't trigger splice resonance on low-fundamental input.
    static const int INITIAL_READ_OFFSET_DEFAULT = 64;
    static const int SNAC_WIN = 1024;
    static const int SNAC_HOP = 128;
    static const int MIN_PERIOD = 48;     // 1 kHz
    static const int MAX_PERIOD = 800;    // 60 Hz
    static const int TRANS_REFRACTORY = 14400;  // 300 ms — keeps transient splice rare
    static constexpr float ENV_SLOW_TC = 1.0f / 4800.0f;  // 100 ms
    static constexpr float ENV_FAST_TC = 1.0f / 48.0f;    // 1 ms
    static constexpr float FIDELITY_THRESH_DEFAULT = 0.7f;  // relaxed from 0.95 for guitar

    float    m_dl[DL];
    float    m_preBuf[PRE_DL];
    uint32_t m_preWr = PRE_DL / 2;
    double   m_preRd = 0.0;
    float    m_preRate = 1.0f;
    float    m_formantDepth = 0.0f;
    int      m_initialReadOffset = INITIAL_READ_OFFSET_DEFAULT;
    float    m_xfadeScale = 1.0f;
    float    m_fidelityThresh = FIDELITY_THRESH_DEFAULT;
    uint32_t m_wr = INITIAL_READ_OFFSET_DEFAULT;
    double   m_rdA = 0.0;
    double   m_rdB = 0.0;
    bool     m_useA = true;
    int      m_xfadeRemain = 0;
    int      m_xfadeLen = 0;
    float    m_scale = 1.0f;
    float    m_targetScale = 1.0f;
    int      m_period = 256;
    float    m_periodF = 256.0f;   // sub-sample period for snap precision
    bool     m_periodValid = false;
    int      m_sinceDetect = 0;
    int      m_warmup = SNAC_WIN;
    float    m_envSlow = 0.0f;
    float    m_envFast = 0.0f;
    float    m_envFastPrev = 0.0f;
    int      m_transCool = 0;
    float    m_snacBuf[SNAC_WIN];
    int      m_snacWr = 0;
    float    m_r[MAX_PERIOD + 1];
    float    m_normK[MAX_PERIOD + 1];

    inline float readSinc(double pos) const {
        int base = (int)pos;
        if (pos < 0) base = (int)pos - 1;
        double frac = pos - (double)base;
        float v = 0;
        for (int k = 0; k < SINC_TAPS; k++) {
            int idx = base + k - SINC_HALF + 1;
            double x = (double)(k - SINC_HALF + 1) - frac;
            double s = (x < 1e-9 && x > -1e-9) ? 1.0
                     : sin(SOLAD_M_PI * x) / (SOLAD_M_PI * x);
            double w = 0.5 - 0.5 * cos(2.0 * SOLAD_M_PI * ((double)k + frac)
                                       / (double)(SINC_TAPS - 1));
            v += m_dl[(uint32_t)idx & MASK] * (float)(s * w);
        }
        return v;
    }

    void triggerSplice(bool toLive) {
        // Hard guard: never re-splice while a crossfade is already in
        // flight. Without this, two close-together triggers leave
        // discontinuous reader state mid-fade and produce a sharp pop.
        if (m_xfadeRemain > 0) return;

        double &rdActive  = m_useA ? m_rdA : m_rdB;
        double &rdPassive = m_useA ? m_rdB : m_rdA;

        double newPos;
        if (toLive) {
            // Transient: snap forward to write pointer minus tiny safety.
            newPos = (double)m_wr - 32.0;
        } else {
            // Drift correction: target middle of safe band.
            newPos = (double)m_wr - (double)m_initialReadOffset;
        }
        // Snap to integer period offset from current active reader if we
        // have a confident period — phase-coherent splice.
        if (m_periodValid && m_periodF >= (float)MIN_PERIOD) {
            double diff = newPos - rdActive;
            double pf = (double)m_periodF;
            int periods = (int)(diff / pf + (diff > 0 ? 0.5 : -0.5));
            newPos = rdActive + (double)periods * pf;

            // Refine: within ±period/2 of the integer-period target, slide
            // newPos to minimise the AMPLITUDE+DERIVATIVE mismatch with
            // rdActive RIGHT NOW. Matching both value and slope ensures the
            // crossfade is invisible — equivalent to a 1st-order continuous
            // splice through the OLA window.
            float vActive = readSinc(rdActive);
            float vActiveNext = readSinc(rdActive + (double)m_scale);
            float dActive = vActiveNext - vActive;
            double bestDelta = 0.0;
            float  bestErr = 1e9f;
            const int N_TRIAL = 33;
            double maxOff = (double)pf * 0.5;
            for (int t = -N_TRIAL/2; t <= N_TRIAL/2; t++) {
                double off = (double)t * maxOff / (double)(N_TRIAL/2);
                double trialPos = newPos + off;
                if (trialPos < 0.0) continue;
                if (trialPos > (double)m_wr - 1.0) continue;
                float vTrial = readSinc(trialPos);
                float vTrialNext = readSinc(trialPos + (double)m_scale);
                float dTrial = vTrialNext - vTrial;
                float err = fabsf(vTrial - vActive) * 1.0f
                          + fabsf(dTrial - dActive) * 100.0f;  // weight slope heavily
                if (err < bestErr) { bestErr = err; bestDelta = off; }
            }
            newPos += bestDelta;
        }
        rdPassive = newPos;
        // Crossfade length = 2 × period for thorough overlap. Generous
        // window beats a tight one — the splice cooldown caps splice rate
        // so even a long xfade doesn't accumulate.
        int len = m_periodValid
                  ? (int)((float)m_period * 2.0f * m_xfadeScale)
                  : (int)(512.0f * m_xfadeScale);
        if (len < 256)  len = 256;
        if (len > 2048) len = 2048;
        m_xfadeLen = len;
        m_xfadeRemain = len;
        m_useA = !m_useA;
    }

    void detectPitch() {
        // McLeod SNAC: r[k] = sum x[n]*x[n-k]  for k=0..MAX_PERIOD
        // norm[k]    = sum x[n]^2 + x[n-k]^2
        // SNAC[k]    = 2*r[k] / norm[k]
        // We read SNAC window with snacWr as the END (most recent sample).
        // For simplicity here: O(W*P) direct loop — fine on host.
        // (Pi build will swap in FFT-based version if CPU-bound.)
        const int W = SNAC_WIN;
        // Build linearized view (most recent W samples in time order).
        float win[SNAC_WIN];
        for (int i = 0; i < W; i++) {
            int idx = (m_snacWr + i) % W;
            win[i] = m_snacBuf[idx];
        }
        // Energy gate.
        float energy = 0.0f;
        for (int i = 0; i < W; i++) energy += win[i] * win[i];
        if (energy < 0.001f) { m_periodValid = false; return; }

        // Direct autocorrelation + norm.
        int maxTau = MAX_PERIOD;
        if (maxTau > W - 32) maxTau = W - 32;
        // r[0] = sum x²
        m_r[0] = energy;
        m_normK[0] = 2.0f * energy;
        for (int k = 1; k <= maxTau; k++) {
            float sum = 0;
            for (int n = 0; n < W - k; n++) sum += win[n] * win[n + k];
            m_r[k] = sum;
            // Incremental norm update.
            float removed = win[W - k] * win[W - k] + win[k - 1] * win[k - 1];
            m_normK[k] = m_normK[k - 1] - removed;
            if (m_normK[k] < 1e-12f) m_normK[k] = 1e-12f;
        }
        // SNAC = 2r/norm. Find first major peak above threshold.
        // Skip first dip down from k=0.
        int k = 1;
        while (k < maxTau && (2.0f * m_r[k] / m_normK[k]) > (2.0f * m_r[k-1] / m_normK[k-1])) k++;
        // Now find local maxima.
        float bestVal = -1.0f;
        int bestTau = -1;
        for (; k < maxTau - 1; k++) {
            if (k < MIN_PERIOD) continue;
            float v  = 2.0f * m_r[k] / m_normK[k];
            float vm = 2.0f * m_r[k-1] / m_normK[k-1];
            float vp = 2.0f * m_r[k+1] / m_normK[k+1];
            if (v > vm && v > vp && v > m_fidelityThresh) {
                // McLeod picks first peak above (max * 0.8) within the bunch.
                if (v > bestVal) { bestVal = v; bestTau = k; }
                // Don't break — keep looking for higher peaks within MAX_PERIOD.
            }
        }
        if (bestTau < 0) { m_periodValid = false; return; }
        // Parabolic interpolation.
        float a = 2.0f * m_r[bestTau-1] / m_normK[bestTau-1];
        float b = 2.0f * m_r[bestTau]   / m_normK[bestTau];
        float c = 2.0f * m_r[bestTau+1] / m_normK[bestTau+1];
        float refined = (float)bestTau;
        float denom = 2.0f * b - a - c;
        if (fabsf(denom) > 1e-9f) refined += (a - c) / denom;
        int np = (int)(refined + 0.5f);
        if (np < MIN_PERIOD) np = MIN_PERIOD;
        if (np > MAX_PERIOD) np = MAX_PERIOD;
        if (refined < (float)MIN_PERIOD) refined = (float)MIN_PERIOD;
        if (refined > (float)MAX_PERIOD) refined = (float)MAX_PERIOD;
        // Light smoothing.
        if (m_periodValid) {
            int delta = np - m_period;
            int maxDelta = m_period / 8 + 2;
            if (delta > maxDelta) {
                np = m_period + maxDelta;
                refined = (float)np;
            }
            if (delta < -maxDelta) {
                np = m_period - maxDelta;
                refined = (float)np;
            }
        }
        m_period = np;
        m_periodF = refined;
        m_periodValid = true;
    }
};
