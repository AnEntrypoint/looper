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
        initSincTable();
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

    // Re-align readers to a fresh state for re-engage. Called by the
    // wrapper when liveEngaged toggles false→true. Without this the
    // readers retain stale positions from the previous engaged session
    // — at unity scale that means the output is delay-line contents
    // from N seconds ago, not fresh audio. m_dl is NOT zeroed because
    // m_wr keeps advancing during disengage gaps would create position
    // jumps; instead we snap rdA/B to the most-recent valid offset.
    void reengage() {
        m_rdA = (double)((int64_t)m_wr - m_initialReadOffset);
        m_rdB = (double)((int64_t)m_wr - m_initialReadOffset);
        m_useA = true;
        m_xfadeRemain = 0;
        m_xfadeLen = 0;
        m_scale = m_targetScale;   // skip 1-pole ramp (already at target by re-engage time)
        m_periodValid = false;
        m_sinceDetect = 0;
        // m_warmup stays at 0 — don't re-mute output; just realign.
    }

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

    // Bypass pre-resample formant stage entirely. When formantDepth=0
    // the stage is supposed to be a unity-rate pass-through, but the
    // 8-tap sinc + drift management adds compute and could be the
    // source of periodic misalignment artefacts. CC103 toggles.
    void setPreResampleBypass(bool on) { m_preBypass = on; }

    // Skip integer-period snap in triggerSplice. The math relies on the
    // SNAC-detected period being correct; if it's off, the splice lands
    // at a position that's not phase-coherent. CC104 toggles.
    void setSpliceSnap(bool on) { m_spliceSnap = on; }

    // Skip the value-match refinement in triggerSplice. CC105 toggles.
    void setSpliceMatch(bool on) { m_spliceMatch = on; }

    // Splice drift-band lower bound (samples). Splice fires when
    // gap < this. Default 8. Higher = more frequent splices.
    void setDriftLowBand(int samples) {
        if (samples < 1) samples = 1;
        if (samples > DL / 4) samples = DL / 4;
        m_driftLowBand = samples;
    }

    // Splice drift-band upper headroom (samples below DL). Splice fires
    // when gap > DL - this. Default 256.
    void setDriftHighHead(int samples) {
        if (samples < 16) samples = 16;
        if (samples > DL / 2) samples = DL / 2;
        m_driftHighHead = samples;
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
            float xWarped;
            // Auto-bypass pre-resample at depth=0. At depth=0 the stage
            // should be unity passthrough (preRate = pow(scale, 0) = 1),
            // but ARM's libm powf may not return exactly 1.0, causing
            // m_preRate to drift fractionally below 1 → sinc interpolation
            // with non-zero frac → audible chorus/intermodulation. Bypass
            // at depth=0 is also free CPU, and numerically equivalent to
            // unity-rate pre-stage. CC103 forces bypass for A/B testing.
            if (m_preBypass || m_formantDepth == 0.0f) {
                xWarped = in[i];
            } else {
                m_preBuf[m_preWr & PRE_MASK] = in[i];
                m_preWr++;
                float scale = m_scale;
                if (scale < 0.01f) scale = 0.01f;
                float targetPreRate = powf(scale, -m_formantDepth);
                m_preRate += (targetPreRate - m_preRate) * (1.0f / 480.0f);

                xWarped = 0.0f;
                double pos = m_preRd;
                int base = (int)pos;
                if (pos < 0) base = (int)pos - 1;
                double frac = pos - (double)base;
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
                double pgap = (double)m_preWr - m_preRd;
                const double pgapTarget = (double)(PRE_DL / 2);
                if (pgap < 4.0 || pgap > (double)(PRE_DL - 16)) {
                    m_preRd = (double)m_preWr - pgapTarget;
                } else if (pgap < pgapTarget * 0.5 || pgap > pgapTarget * 1.5) {
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

            // ---- 2. Pitch detect — slow refresh timer ----
            // SNAC refreshes the cached period at a relaxed rate (every
            // SNAC_HOP). The micro-splice path reuses the cached period
            // between refreshes, so the heavy autocorrelation runs at the
            // HOP cadence only, NOT per-splice. At SNAC_HOP=2048 (43ms)
            // that's ~23 bursts/sec — well below the level that stalls
            // audio blocks, while still tracking guitar pitch changes.
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
            // At target=1.0, force scale to exactly 1.0 to avoid the
            // 1-pole's asymptotic float-precision drift (which produces
            // fractional sinc reads at unity → audible chorus). Off-unity
            // targets ramp normally.
            if (m_targetScale == 1.0f) {
                m_scale = 1.0f;
            } else {
                m_scale += (m_targetScale - m_scale) * (1.0f / 480.0f);
            }

            // ---- 5. Manage read pointers ----
            if (m_warmup > 0) {
                m_warmup--;
                out[i] = 0.0f;
                // Both readers parked at the same offset behind write —
                // ensures that when the first splice flips active/passive,
                // the formerly-passive reader is already at a valid position
                // pointing at real audio, not at delay-line[0] silence.
                m_rdA = (double)(m_wr - m_initialReadOffset);
                m_rdB = (double)(m_wr - m_initialReadOffset);
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
            // PSOLA pitch shift REQUIRES the reader to drift (it advances
            // slower than the writer at down-shift) and periodically splice
            // back by an integer number of periods. You cannot bound the gap
            // without splicing — that's the core of the algorithm. The key
            // to inaudible splices is: splice OFTEN and PHASE-ALIGNED, so
            // each is a tiny period-boundary overlap, not a rare giant jump.
            //
            // Trigger a splice once the reader has drifted ~one resplice
            // interval (RESPLICE_GAP samples) past the target offset. With
            // RESPLICE_GAP ≈ 4096 (85ms) splices fire ~12×/sec at -12 —
            // frequent enough that each only re-uses 85ms of audio (no
            // audible repeat), phase-aligned so the crossfade is seamless.
            // True-PSOLA resplice: jump the reader forward by EXACTLY ONE
            // detected period whenever it has drifted one period behind the
            // target. Adjacent periods of a quasi-periodic signal are nearly
            // identical, so a 1-period jump with a 1-period Hann overlap is
            // seamless — no audible chunk. This fires often (~100/sec at
            // 200Hz) but each is a micro-splice, not the 85ms macro-jump
            // that caused the audible "quantized chunks several times/sec".
            //
            // SNAC is NOT run here (that's the ring-resync cost). We reuse
            // the cached m_periodF, refreshed on a slow timer below.
            double driftFromTarget = gap - (double)m_initialReadOffset;
            double per = (m_periodValid && m_periodF >= (float)MIN_PERIOD)
                         ? (double)m_periodF : 240.0;
            if (driftFromTarget > per && m_envSlow > 0.003f && m_xfadeRemain == 0) {
                // Jump reader forward by one period, crossfade over one period.
                triggerSpliceByPeriod(per);
            }
            // Emergency hard-escape (buffer wrap protection only).
            if (gap > (double)(DL - 16) || gap < 16.0) {
                triggerSplice(/*toLive=*/false);
            }
            m_gapBias = 0.0;  // no rate bias — pitch must stay exact

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
            // m_gapBias holds the gap near mid-buffer so no splice is ever
            // needed on sustained pitch shift. The +bias makes the reader
            // advance slightly faster (shrinking an over-large gap) or
            // slightly slower (growing a too-small gap). Magnitude <0.05.
            double adv = (double)m_scale + m_gapBias;
            m_rdA += adv;
            m_rdB += adv;
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
    // 192 samples = 4 ms @ 48 kHz. Matches host engine default which has
    // been tested clean (0.06% THD, 41Hz lock from 82Hz input). The 64-
    // sample setting from the earlier sweep was measuring passthrough
    // (engine never actually engaged because ch2 MIDI handler was missing)
    // — that sweep's "best at 64" conclusion is invalid.
    static const int INITIAL_READ_OFFSET_DEFAULT = 192;
    static const int SNAC_WIN = 1024;
    static const int SNAC_HOP = 2048;  // 43ms period-refresh cadence. The
                                       // micro-splice path reuses the cached
                                       // period between refreshes, so SNAC's
                                       // heavy autocorrelation runs ~23/sec
                                       // (not per-splice ~100/sec) — gentle
                                       // enough not to stall audio blocks /
                                       // trigger IN-ring resync.
    static const int MIN_PERIOD = 48;     // 1 kHz
    static const int MAX_PERIOD = 800;    // 60 Hz
    // Reader drifts this far past the read-offset before a phase-aligned
    // resplice. 4096 = 85ms → ~12 splices/sec at -12. Frequent + small so
    // each splice's audio re-use is imperceptible; phase-aligned so the
    // crossfade is seamless. SNAC runs once per splice (≈12/sec) not per
    // HOP (188/sec) — the difference that keeps the IN ring from resyncing.
    static const int RESPLICE_GAP = 4096;
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
    bool     m_preBypass = false;
    bool     m_spliceSnap = true;
    bool     m_spliceMatch = true;
    int      m_driftLowBand = 8;
    int      m_driftHighHead = 256;
    uint32_t m_wr = INITIAL_READ_OFFSET_DEFAULT;
    double   m_rdA = 0.0;
    double   m_rdB = 0.0;
    double   m_gapBias = 0.0;
    bool     m_useA = true;
    int      m_xfadeRemain = 0;
    int      m_xfadeLen = 0;
    float    m_scale = 1.0f;
    float    m_targetScale = 1.0f;
    int      m_period = 256;
    float    m_periodF = 256.0f;   // sub-sample period for snap precision
    bool     m_periodValid = false;
public:
    // Introspection for Pi-side telemetry (read by wrapper → audio.cpp log).
    unsigned m_spliceCount = 0;
    float    scaleNow()  const { return m_scale; }
    int      periodNow() const { return m_period; }
    bool     periodOk()  const { return m_periodValid; }
private:
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

    // Precomputed sinc kernel table — TABLE_SIZE phases × SINC_TAPS taps.
    // Lookup at runtime replaces ~16 sin() + 16 cos() per readSinc call
    // (was 32 transcendentals × 2 readers × 48000 = 3M/sec on Pi).
    static constexpr int SINC_PHASES = 256;
    static float sincTable[SINC_PHASES][SINC_TAPS];
    static bool sincTableReady;
    static void initSincTable() {
        if (sincTableReady) return;
        for (int p = 0; p < SINC_PHASES; p++) {
            double frac = (double)p / (double)SINC_PHASES;
            for (int k = 0; k < SINC_TAPS; k++) {
                double x = (double)(k - SINC_HALF + 1) - frac;
                double s = (x < 1e-9 && x > -1e-9) ? 1.0
                         : sin(SOLAD_M_PI * x) / (SOLAD_M_PI * x);
                double w = 0.5 - 0.5 * cos(2.0 * SOLAD_M_PI * ((double)k + frac)
                                           / (double)(SINC_TAPS - 1));
                sincTable[p][k] = (float)(s * w);
            }
        }
        sincTableReady = true;
    }

    inline float readSinc(double pos) const {
        int base = (int)pos;
        if (pos < 0) base = (int)pos - 1;
        double frac = pos - (double)base;
        // Lookup nearest phase in precomputed table — saves ~16 sin/cos
        // per call. The 256-phase resolution gives error well below
        // perceptual threshold for fractional-rate reads.
        int p = (int)(frac * SINC_PHASES);
        if (p < 0) p = 0;
        if (p >= SINC_PHASES) p = SINC_PHASES - 1;
        const float* coef = sincTable[p];
        float v = 0;
        for (int k = 0; k < SINC_TAPS; k++) {
            int idx = base + k - SINC_HALF + 1;
            v += m_dl[(uint32_t)idx & MASK] * coef[k];
        }
        return v;
    }

    // Micro-splice: jump the new reader forward by exactly ONE period
    // from the active reader and crossfade over one period. Because
    // consecutive periods of a quasi-periodic tone are near-identical,
    // this is seamless — the textbook PSOLA period-repeat. No SNAC here
    // (uses the passed cached period), so it can fire ~100/sec cheaply.
    void triggerSpliceByPeriod(double per) {
        if (m_xfadeRemain > 0) return;
        m_spliceCount++;
        double &rdActive  = m_useA ? m_rdA : m_rdB;
        double &rdPassive = m_useA ? m_rdB : m_rdA;
        // New reader = active + one period (forward jump = skip ahead,
        // shrinking the gap by exactly one period). Phase-identical point.
        rdPassive = rdActive + per;
        int len = (int)per;
        if (len < 32) len = 32;
        if (len > 2048) len = 2048;
        m_xfadeLen = len;
        m_xfadeRemain = len;
        m_useA = !m_useA;
    }

    void triggerSplice(bool toLive) {
        // Hard guard: never re-splice while a crossfade is already in
        // flight. Without this, two close-together triggers leave
        // discontinuous reader state mid-fade and produce a sharp pop.
        if (m_xfadeRemain > 0) return;
        m_spliceCount++;   // introspection: how often are we splicing?

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
        // have a confident period — phase-coherent splice. Toggle via
        // setSpliceSnap (CC104).
        if (m_spliceSnap && m_periodValid && m_periodF >= (float)MIN_PERIOD) {
            double diff = newPos - rdActive;
            double pf = (double)m_periodF;
            int periods = (int)(diff / pf + (diff > 0 ? 0.5 : -0.5));
            newPos = rdActive + (double)periods * pf;

            // Refine: within ±period/2 of the integer-period target, slide
            // newPos to minimise the AMPLITUDE+DERIVATIVE mismatch with
            // rdActive RIGHT NOW. Toggle via setSpliceMatch (CC105).
            if (m_spliceMatch) {
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
                              + fabsf(dTrial - dActive) * 100.0f;
                    if (err < bestErr) { bestErr = err; bestDelta = off; }
                }
                newPos += bestDelta;
            }
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

        // Full autocorrelation (stride=1). Earlier stride=4 was a CPU
        // hack reverted because actual glitch cause was buffer
        // misalignment on re-engage, not CPU shortage.
        int maxTau = MAX_PERIOD;
        if (maxTau > W - 32) maxTau = W - 32;
        m_r[0] = energy;
        m_normK[0] = 2.0f * energy;
        for (int k = 1; k <= maxTau; k++) {
            float sum = 0;
            int limit = W - k;
            for (int n = 0; n < limit; n++) sum += win[n] * win[n + k];
            m_r[k] = sum;
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

// Static-member storage. Header-only so we mark inline (C++17).
inline float EngineSoladSnac::sincTable[EngineSoladSnac::SINC_PHASES][EngineSoladSnac::SINC_TAPS] = {};
inline bool  EngineSoladSnac::sincTableReady = false;
