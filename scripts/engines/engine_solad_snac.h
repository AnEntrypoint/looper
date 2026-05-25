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
        m_preRdB = 0.0;
        m_preUseA = true;
        m_preXfadeRemain = 0;
        m_preXfadeLen = 0;
        m_preEnv = 0.0f;
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
        m_lastGoodPeriodF = 256.0f;
        m_haveGoodPeriod = false;
        m_sinceDetect = 0;
        m_warmup = SNAC_WIN;
        m_envSlow = 0.0f;
        m_envFast = 0.0f;
        m_transCool = 0;
        for (int i = 0; i < SNAC_WIN; i++) m_snacBuf[i] = 0.0f;
        m_snacWr = 0;
        m_snacPhase = 0;   // SNAC_IDLE
        m_snacK = 1;
        m_snacEnergy = 0.0f;
        m_snacMaxTau = 0;
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

                // Cheap rising-transient flag from the input stream — used to
                // DEFER pre-resample wraps during attacks so the wrap's grain-
                // repeat never lands on a transient (was a source of smear).
                float aIn = fabsf(in[i]);
                m_preEnv += (aIn - m_preEnv) * (1.0f / 64.0f);
                bool preTransient = (aIn > m_preEnv * 2.5f && aIn > 0.02f);

                // Period for phase-coherent wraps (reuse cached SNAC period).
                double per = (m_periodValid && m_periodF >= (float)MIN_PERIOD)
                             ? (double)m_periodF : 0.0;
                // Keep the read close behind write (small gap) so any grain
                // repeat on wrap is RECENT — a one-period repeat of the last
                // few ms is inaudible, vs the old 85ms replay that doubled.
                double gapTarget = (per > 0.0) ? per * 4.0 : 1024.0;
                double pgapA = (double)m_preWr - m_preRd;

                // Wrap when the active reader approaches either bound. Jump the
                // PASSIVE reader by an integer number of periods back toward
                // gapTarget (phase-coherent), short equal-gain crossfade, swap.
                bool nearBound = (pgapA < gapTarget * 0.4) || (pgapA > gapTarget * 2.5);
                if (per > 0.0 && nearBound && m_preXfadeRemain == 0 && !preTransient) {
                    double &a = m_preUseA ? m_preRd : m_preRdB;
                    double &b = m_preUseA ? m_preRdB : m_preRd;
                    double target = (double)m_preWr - gapTarget;
                    double diff = target - a;
                    int periods = (int)(diff / per + (diff > 0 ? 0.5 : -0.5));
                    if (periods == 0) periods = (diff > 0 ? 1 : -1);
                    b = a + (double)periods * per;
                    int len = (int)(per * 0.5);
                    if (len < 24) len = 24; if (len > 512) len = 512;
                    m_preXfadeLen = len; m_preXfadeRemain = len;
                    m_preUseA = !m_preUseA;
                } else if (per <= 0.0 && (pgapA < 8.0 || pgapA > (double)(PRE_DL - 16))) {
                    // No period lock (silence/noise) — bare reset, inaudible.
                    m_preRd = (double)m_preWr - gapTarget;
                    m_preRdB = m_preRd;
                }

                float yA = readPre(m_preRd);
                float yB = readPre(m_preRdB);
                float pw = (m_preXfadeRemain > 0 && m_preXfadeLen > 0)
                           ? (float)m_preXfadeRemain / (float)m_preXfadeLen : 0.0f;
                float wAct = 1.0f - pw, wPas = pw;  // equal-gain linear
                xWarped = m_preUseA ? (yA * wAct + yB * wPas)
                                    : (yB * wAct + yA * wPas);
                m_preRd  += (double)m_preRate;
                m_preRdB += (double)m_preRate;
                if (m_preXfadeRemain > 0) m_preXfadeRemain--;
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
            // Incremental SNAC: a small slice of the autocorrelation lag
            // range is computed EACH block so no single processBlock call
            // does the full O(SNAC_WIN*MAX_PERIOD) burst (~820k mul) that
            // overran the Core 1 audio deadline and triggered USB IN-ring
            // resyncs (in_rs) — which fed the engine discontinuous input and
            // destroyed the -12 output on the Pi. detectPitchStep() advances
            // the sweep; a fresh sweep is armed every SNAC_HOP samples.
            if (++m_sinceDetect >= SNAC_HOP && m_snacPhase == SNAC_IDLE) {
                m_sinceDetect = 0;
                snacBegin();
            }
            if (m_snacPhase != SNAC_IDLE) detectPitchStep();

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
            // Only resplice on a VALID period. A fallback fixed jump (e.g.
            // 240) is not phase-aligned to the actual waveform => the
            // 1-period crossfade lands on a discontinuity = click on every
            // fallback splice. When the detector is unsure (silence/noise
            // tail / not-yet-locked) we DEFER: let the gap stretch until
            // SNAC re-locks. Only the emergency hard-escape below may splice
            // without a valid period (buffer-wrap protection).
            // Cache the last confidently-detected period. When SNAC lock
            // momentarily drops (a transient, a brief noisy window) we keep
            // resplicing with this STALE-BUT-RECENT period rather than
            // deferring — deferring let the reader drift unbounded to the
            // buffer wall, firing the emergency hard-escape = one POP, then a
            // splice storm = GURGLE until re-lock (user-reported). A recent
            // period is still phase-coherent enough for a clean 1-period jump.
            if (m_periodValid && m_periodF >= (float)MIN_PERIOD) {
                m_lastGoodPeriodF = m_periodF;
                m_haveGoodPeriod = true;
            }
            if (m_haveGoodPeriod) {
                double per = (double)m_lastGoodPeriodF;
                double trigger = per * m_respliceFrac;
                if (driftFromTarget > trigger && m_envSlow > 0.003f && m_xfadeRemain == 0) {
                    // Jump forward by as many WHOLE periods as needed to bring
                    // the gap back to ~target in ONE phase-coherent splice,
                    // not just one period. A single-period jump left the gap
                    // still above trigger whenever drift exceeded ~1 period,
                    // so it re-fired every block = splice STORM = gurgle (the
                    // 16s-period instability: the read/write gap slow-beats,
                    // and on the high half it stormed). Clearing the whole
                    // drift gives hysteresis: one clean splice, then quiet
                    // until the gap genuinely drifts up a period again.
                    triggerSpliceByPeriod(per, driftFromTarget);
                }
            }
            // Emergency hard-escape (buffer wrap protection only). With the
            // stale-period resplice above keeping the gap bounded, this should
            // never fire in normal sustained use — it's the last-resort
            // backstop for true silence-from-cold (no period ever seen).
            if (gap > (double)(DL - 16) || gap < 16.0) {
                m_emergencyCount++;
                triggerSplice(/*toLive=*/false);
            }
            m_gapBias = 0.0;  // no rate bias — at downshift the reader MUST lag
                              // (the lag IS the pitch shift); biasing it toward
                              // a fixed gap detunes the output. Gap control is
                              // the splice's job, not a rate bias.

            // ---- 6. Read + crossfade ----
            float yA = readSinc(m_rdA);
            float yB = readSinc(m_rdB);
            float w  = (m_xfadeRemain > 0 && m_xfadeLen > 0)
                       ? (float)m_xfadeRemain / (float)m_xfadeLen
                       : 0.0f;
            // w=1 → 100% passive (old reader); w=0 → 100% active (new reader)
            // EQUAL-GAIN linear fade: wActive + wPassive == 1.0 exactly.
            // The two readers are ONE PERIOD apart on a quasi-periodic
            // signal — they are CORRELATED, not independent. An equal-POWER
            // (sum-of-squares=1) cosine fade on correlated grains sums to
            // >1.0 mid-fade (up to +3dB); with per-splice SNAC phase error
            // the bump magnitude varies splice-to-splice => periodic
            // amplitude modulation = audible tremolo. Linear (constant-sum)
            // fade holds the summed amplitude flat when the grains are
            // phase-aligned, which is the PSOLA invariant.
            float wActive  = 1.0f - w;
            float wPassive = w;

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
            // Effective-pitch instrumentation: accumulate the ACTIVE reader's
            // true displacement per output sample INCLUDING splice jumps
            // (recorded in m_spliceJumpAccum). effRate = (cont advance +
            // splice jumps) / samples. If output detunes (52 not 55Hz at -12)
            // this reveals whether the read rate != scale on the Pi.
            m_effContAccum += adv;
            m_effSamples++;
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
    static const int DL = 32768;   // 0.68s at 48k. Big enough that the
                                   // emergency buffer-wrap escape stays rare,
                                   // small enough (128KB/ch, 256KB stereo) to
                                   // keep RubberBandWrapper heap-safe on the Pi
                                   // (the former 131072=512KB/ch bloated the
                                   // single `new` to multi-MB and corrupted on
                                   // AARCH=32). The stale-period resplice keeps
                                   // the gap bounded so wrap rarely matters.
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
    double   m_preRd = 0.0;       // active pre-resample read pointer
    double   m_preRdB = 0.0;      // passive (post-wrap) pre-resample reader
    bool     m_preUseA = true;
    int      m_preXfadeRemain = 0;
    int      m_preXfadeLen = 0;
    float    m_preEnv = 0.0f;     // fast input envelope for wrap-defer on attacks
    float    m_preRate = 1.0f;
    float    m_formantDepth = 0.0f;
    int      m_initialReadOffset = INITIAL_READ_OFFSET_DEFAULT;
    float    m_xfadeScale = 1.0f;
    float    m_respliceFrac = 1.0f;  // resplice after drifting this fraction
                                      // of a period; smaller = finer cadence,
                                      // smaller residual amplitude ripple.
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
    float    m_lastGoodPeriodF = 256.0f;  // survives brief lock loss
    bool     m_haveGoodPeriod = false;
    unsigned m_emergencyCount = 0;        // emergency buffer-wrap escapes
public:
    // Introspection for Pi-side telemetry (read by wrapper → audio.cpp log).
    unsigned m_spliceCount = 0;
    int      gapNow() const { return (int)((double)m_wr - (m_useA ? m_rdA : m_rdB)); }
    unsigned emergencyCount() const { return m_emergencyCount; }
    double   m_effContAccum = 0.0;
    double   m_spliceJumpAccum = 0.0;
    unsigned m_effSamples = 0;
    // Effective read rate over the window since last read (incl splice jumps).
    // At a correct -12 this must read 0.500; a detuned 52Hz output reads ~0.47.
    float effRateNow() {
        float r = (m_effSamples > 0)
                  ? (float)((m_effContAccum + m_spliceJumpAccum) / (double)m_effSamples)
                  : 0.0f;
        m_effContAccum = 0.0; m_spliceJumpAccum = 0.0; m_effSamples = 0;
        return r;
    }
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
    float    m_snacWin[SNAC_WIN];   // snapshot for incremental sweep
    float    m_snacPre[SNAC_WIN + 1]; // prefix energy for O(1) per-lag norm
    float    m_snacEnergy = 0.0f;
    int      m_snacMaxTau = 0;
    int      m_snacK = 1;           // current lag in the incremental sweep
    int      m_snacPhase = 0;       // SNAC_IDLE / SNAC_SWEEP
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
            double sum = 0.0;
            double tmp[SINC_TAPS];
            for (int k = 0; k < SINC_TAPS; k++) {
                double x = (double)(k - SINC_HALF + 1) - frac;
                double s = (x < 1e-9 && x > -1e-9) ? 1.0
                         : sin(SOLAD_M_PI * x) / (SOLAD_M_PI * x);
                double w = 0.5 - 0.5 * cos(2.0 * SOLAD_M_PI * ((double)k + frac)
                                           / (double)(SINC_TAPS - 1));
                tmp[k] = s * w;
                sum += tmp[k];
            }
            // Per-phase DC-gain normalization. The raw windowed-sinc kernel's
            // coefficient sum varies ~9.9% across fractional phases (0.905 at
            // frac~0.5, up to 1.0 at frac=0). As the read pointer drifts
            // through phases during pitch-shift, that gain ripple modulates
            // the output amplitude => audible TREMOLO (~3-5Hz at -12).
            // Normalizing each phase row to unity DC gain kills the ripple
            // at its source — the dominant tremolo mechanism (host-measured:
            // 20% envelope modulation depth before this fix).
            double inv = (fabs(sum) > 1e-9) ? 1.0 / sum : 1.0;
            for (int k = 0; k < SINC_TAPS; k++)
                sincTable[p][k] = (float)(tmp[k] * inv);
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

    // 8-tap windowed-sinc read of the formant pre-resample buffer with
    // per-call DC-gain normalization (same fix as the main sinc table — an
    // un-normalized kernel ripples ~10% across phases = formant-stage tremolo).
    inline float readPre(double pos) const {
        int base = (int)pos;
        if (pos < 0) base = (int)pos - 1;
        double frac = pos - (double)base;
        const int TAPS = 8, HALF = TAPS / 2;
        double acc = 0.0, sum = 0.0;
        for (int k = 0; k < TAPS; k++) {
            int idx = base + k - HALF + 1;
            double dx = (double)(k - HALF + 1) - frac;
            double s = (dx < 1e-9 && dx > -1e-9) ? 1.0
                     : sin(SOLAD_M_PI * dx) / (SOLAD_M_PI * dx);
            double w = 0.5 - 0.5 * cos(2.0 * SOLAD_M_PI * ((double)k + frac)
                                       / (double)(TAPS - 1));
            double c = s * w;
            acc += (double)m_preBuf[(uint32_t)idx & PRE_MASK] * c;
            sum += c;
        }
        return (float)(sum > 1e-9 ? acc / sum : acc);
    }

    // Micro-splice: jump the new reader forward by exactly ONE period
    // from the active reader and crossfade over one period. Because
    // consecutive periods of a quasi-periodic tone are near-identical,
    // this is seamless — the textbook PSOLA period-repeat. No SNAC here
    // (uses the passed cached period), so it can fire ~100/sec cheaply.
    void triggerSpliceByPeriod(double per, double drift = 0.0) {
        if (m_xfadeRemain > 0) return;
        m_spliceCount++;
        double &rdActive  = m_useA ? m_rdA : m_rdB;
        double &rdPassive = m_useA ? m_rdB : m_rdA;
        // Forward-jump by N whole periods to clear the accumulated drift in
        // one phase-coherent splice (N>=1). Adjacent periods of a
        // quasi-periodic tone are near-identical, so an N-period jump is as
        // seamless as a 1-period one but it fully resets the gap, giving the
        // control loop hysteresis (no per-block re-fire = no splice storm).
        int n = 1;
        if (drift > per) { n = (int)(drift / per + 0.5); if (n < 1) n = 1; if (n > 64) n = 64; }
        double jump = (double)n * per;
        rdPassive = rdActive + jump;
        m_spliceJumpAccum += jump;
        int len = (int)per;          // crossfade still spans ONE period
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

    // ---- Incremental McLeod SNAC ----
    // The full O(SNAC_WIN*MAX_PERIOD) autocorrelation (~820k mul) done in a
    // single block overran the Core 1 audio deadline on the Pi, stalling the
    // chain and triggering USB IN-ring resyncs that destroyed the -12 output.
    // Here the sweep is split across blocks: snacBegin() snapshots the window
    // + energy gate, then detectPitchStep() computes LAGS_PER_STEP lags per
    // processBlock call until the range is swept, then finalizes the peak.
    static const int LAGS_PER_STEP = 48;   // ~48 * SNAC_WIN ≈ 49k mul / block
    enum { SNAC_IDLE = 0, SNAC_SWEEP = 1 };

    void snacBegin() {
        const int W = SNAC_WIN;
        for (int i = 0; i < W; i++) {
            int idx = (m_snacWr + i) % W;
            m_snacWin[i] = m_snacBuf[idx];
        }
        // Prefix energy: m_snacPre[i] = sum_{n<i} win[n]^2. Lets the per-lag
        // norm be computed in O(1) instead of O(W), keeping each step light.
        float acc = 0.0f;
        for (int i = 0; i < W; i++) { m_snacPre[i] = acc; acc += m_snacWin[i] * m_snacWin[i]; }
        m_snacPre[W] = acc;
        float energy = acc;
        m_snacEnergy = energy;
        if (energy < 0.001f) { m_periodValid = false; m_snacPhase = SNAC_IDLE; return; }
        m_snacMaxTau = MAX_PERIOD; if (m_snacMaxTau > W - 32) m_snacMaxTau = W - 32;
        m_r[0] = energy;
        m_normK[0] = 2.0f * energy;
        m_snacK = 1;
        m_snacPhase = SNAC_SWEEP;
    }

    void detectPitchStep() {
        const int W = SNAC_WIN;
        int kEnd = m_snacK + LAGS_PER_STEP;
        if (kEnd > m_snacMaxTau + 1) kEnd = m_snacMaxTau + 1;
        for (int k = m_snacK; k < kEnd; k++) {
            float sum = 0; int limit = W - k;
            for (int n = 0; n < limit; n++) sum += m_snacWin[n] * m_snacWin[n + k];
            m_r[k] = sum;
            // Per-lag norm via prefix sums (O(1)): e1 = sum win[0..limit)^2,
            // e2 = sum win[k..W)^2 = total - prefix[k].
            float e1 = m_snacPre[limit];
            float e2 = m_snacPre[W] - m_snacPre[k];
            float nk = e1 + e2; if (nk < 1e-12f) nk = 1e-12f;
            m_normK[k] = nk;
        }
        m_snacK = kEnd;
        if (m_snacK <= m_snacMaxTau) return;   // more slices to go

        // Sweep complete — peak pick (same logic as before).
        m_snacPhase = SNAC_IDLE;
        int maxTau = m_snacMaxTau;
        int k = 1;
        while (k < maxTau && (2.0f*m_r[k]/m_normK[k]) > (2.0f*m_r[k-1]/m_normK[k-1])) k++;
        float bestVal = -1.0f; int bestTau = -1;
        for (; k < maxTau - 1; k++) {
            if (k < MIN_PERIOD) continue;
            float v  = 2.0f*m_r[k]/m_normK[k];
            float vm = 2.0f*m_r[k-1]/m_normK[k-1];
            float vp = 2.0f*m_r[k+1]/m_normK[k+1];
            if (v > vm && v > vp && v > m_fidelityThresh) {
                if (v > bestVal) { bestVal = v; bestTau = k; }
            }
        }
        if (bestTau < 0) { m_periodValid = false; return; }
        float a = 2.0f*m_r[bestTau-1]/m_normK[bestTau-1];
        float b = 2.0f*m_r[bestTau]  /m_normK[bestTau];
        float c = 2.0f*m_r[bestTau+1]/m_normK[bestTau+1];
        float refined = (float)bestTau;
        float denom = 2.0f*b - a - c;
        if (fabsf(denom) > 1e-9f) refined += (a - c) / denom;
        int np = (int)(refined + 0.5f);
        if (np < MIN_PERIOD) np = MIN_PERIOD;
        if (np > MAX_PERIOD) np = MAX_PERIOD;
        if (refined < (float)MIN_PERIOD) refined = (float)MIN_PERIOD;
        if (refined > (float)MAX_PERIOD) refined = (float)MAX_PERIOD;
        if (m_periodValid) {
            int delta = np - m_period;
            int maxDelta = m_period / 8 + 2;
            if (delta >  maxDelta) { np = m_period + maxDelta; refined = (float)np; }
            if (delta < -maxDelta) { np = m_period - maxDelta; refined = (float)np; }
        }
        m_period = np;
        m_periodF = refined;
        m_periodValid = true;
    }
};

// Static-member storage. Header-only so we mark inline (C++17).
inline float EngineSoladSnac::sincTable[EngineSoladSnac::SINC_PHASES][EngineSoladSnac::SINC_TAPS] = {};
inline bool  EngineSoladSnac::sincTableReady = false;
