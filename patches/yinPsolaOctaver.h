#pragma once
// YIN-tracked PSOLA octaver (-12 specialist) — epoch-based, click-hardened.
//
// Algorithm: YIN detects period T on sliding window. Spawn Hann-windowed
// grain of length 2T every T output samples (50% overlap → sum = 1.0). Input
// epoch advances by T*pitchScale per output epoch (T/2 for -12) so each
// input period is sampled by ~2 output grains → period doubled = pitch halved.
//
// Click hardening (vs prior PSOLA attempts):
//   - Voice locks its grainLen + sourceAbs at spawn; mid-grain T changes
//     never break 50% overlap because the *next* grain uses new T.
//   - 4-voice pool (instead of 2): when T shrinks, old 2T grain may still
//     be alive while 2 new shorter grains spawn.
//   - Sticky lock: hold previous T for HOLD_SAMPLES after YIN drops THRESH,
//     so brief detection dropouts don't flip mode.
//   - Crossfade lock→nolock: transitions fade between PSOLA output and
//     dry-delayed passthrough over FADE samples.
//   - RMS gate: when input is below NOISE_FLOOR, emit silence (no PSOLA
//     on noise).
//   - Grain source safety: voice.sourceAbs clamped to addressable buffer
//     so first grains during warmup don't read garbage.

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <vector>

class EngineYinPsola {
public:
    EngineYinPsola() { reset(); }

    void reset() {
        m_buf.assign(BUF, 0.0f);
        m_writeAbs = 0;
        m_period = 0;
        m_periodConfident = false;
        m_holdSamples = 0;
        m_lockFade = 0.0f;
        m_samplesSinceDetect = 0;
        m_warmup = DETECT_WIN + MAX_PERIOD;
        m_pitchScale = 0.5f;
        for (int v = 0; v < N_VOICES; v++) m_voice[v] = {};
        m_samplesUntilNextEpoch = 0;
        m_nextVoice = 0;
        m_inputEpochAbs = 0;
        m_inputEpochFrac = 0.0;
        m_rmsAcc = 0.0f;
    }

    void configure(float /*sampleRate*/, float pitchScale) {
        m_pitchScale = pitchScale;
    }

    void processBlock(const float* in, float* out, int n) {
        for (int i = 0; i < n; i++) {
            // 1. Ingest dry sample.
            m_buf[(size_t)(m_writeAbs % BUF)] = in[i];
            m_writeAbs++;

            // Running RMS (1-pole, ~10ms).
            m_rmsAcc = 0.999f * m_rmsAcc + 0.001f * in[i] * in[i];
            float rms = sqrtf(m_rmsAcc);

            // 2. Periodically refresh YIN.
            if (++m_samplesSinceDetect >= HOP) {
                m_samplesSinceDetect = 0;
                detectPeriod();
            }

            if (m_warmup > 0) { m_warmup--; out[i] = 0.0f; continue; }

            // 3. Compute desired output via PSOLA + dry-delay; crossfade
            //    between them based on lock state.
            float psola = 0.0f;
            float dry = sampleAt(m_writeAbs - DRY_DELAY);

            bool useLock = m_periodConfident && rms > NOISE_FLOOR;
            if (useLock) {
                m_holdSamples = HOLD_SAMPLES;
                if (m_lockFade < 1.0f) m_lockFade = fminf(1.0f, m_lockFade + FADE_STEP);
            } else if (m_holdSamples > 0) {
                m_holdSamples--;
                useLock = true;  // still riding last good T
            } else {
                if (m_lockFade > 0.0f) m_lockFade = fmaxf(0.0f, m_lockFade - FADE_STEP);
            }

            if (m_lockFade > 0.0f) {
                int T = m_period;
                if (T < MIN_PERIOD) T = MIN_PERIOD;
                if (T > MAX_PERIOD) T = MAX_PERIOD;
                int grainLen = 2 * T;

                // First-time seed of input epoch on lock.
                if (m_inputEpochAbs == 0) {
                    m_inputEpochAbs = m_writeAbs - T;
                    m_samplesUntilNextEpoch = 0;
                }

                if (m_samplesUntilNextEpoch <= 0) {
                    Voice &v = m_voice[m_nextVoice];
                    // Don't stomp an active voice — find a free one.
                    int free = -1;
                    for (int k = 0; k < N_VOICES; k++) {
                        if (!m_voice[(m_nextVoice + k) % N_VOICES].active) {
                            free = (m_nextVoice + k) % N_VOICES;
                            break;
                        }
                    }
                    if (free >= 0) {
                        Voice &nv = m_voice[free];
                        nv.active = true;
                        nv.sampleInGrain = 0;
                        nv.grainLen = grainLen;
                        // Clamp grain start so it's always inside addressable buffer.
                        int64_t startAbs = m_inputEpochAbs - T;
                        int64_t earliest = m_writeAbs - (BUF - 1);
                        if (startAbs < earliest) startAbs = earliest;
                        if (startAbs < 0) startAbs = 0;
                        nv.sourceAbs = startAbs;
                        m_nextVoice = (free + 1) % N_VOICES;
                    }
                    m_samplesUntilNextEpoch = T;  // 50% overlap

                    // Advance input epoch by T*pitchScale.
                    m_inputEpochFrac += (double)T * (double)m_pitchScale;
                    int64_t adv = (int64_t)m_inputEpochFrac;
                    m_inputEpochAbs += adv;
                    m_inputEpochFrac -= (double)adv;
                    int64_t latest = m_writeAbs - T;
                    if (m_inputEpochAbs > latest) m_inputEpochAbs = latest;
                    if (m_writeAbs - m_inputEpochAbs > BUF/2)
                        m_inputEpochAbs = m_writeAbs - T;
                }
                m_samplesUntilNextEpoch--;

                // Sum active voices with Hann window.
                for (int vi = 0; vi < N_VOICES; vi++) {
                    Voice &v = m_voice[vi];
                    if (!v.active) continue;
                    float phase = (float)v.sampleInGrain / (float)v.grainLen;
                    float w = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * phase);
                    psola += w * sampleAt(v.sourceAbs + v.sampleInGrain);
                    v.sampleInGrain++;
                    if (v.sampleInGrain >= v.grainLen) v.active = false;
                }
            }

            // Smooth crossfade between PSOLA and dry.
            out[i] = psola * m_lockFade + dry * (1.0f - m_lockFade);
        }
    }

    static void run(const std::vector<float>& in, std::vector<float>& out,
                    int sr, float scale) {
        EngineYinPsola e;
        e.configure((float)sr, scale);
        out.assign(in.size(), 0.0f);
        const int CHUNK = 64;
        for (size_t i = 0; i < in.size(); i += CHUNK) {
            size_t left = in.size() - i;
            int n = (int)(left < (size_t)CHUNK ? left : (size_t)CHUNK);
            e.processBlock(&in[i], &out[i], n);
        }
    }

private:
    static const int BUF = 16384;
    static const int DETECT_WIN = 1024;
    static const int HOP = 64;
    static const int MIN_PERIOD = 48;     // 1kHz
    static const int MAX_PERIOD = 1200;   // 40Hz
    static const int N_VOICES = 4;
    static const int DRY_DELAY = 512;     // matches typical grain half for phase coherence
    static const int HOLD_SAMPLES = 2400; // 50ms @ 48k — ride brief YIN dropouts
    static constexpr float FADE_STEP = 1.0f / 480.0f;  // ~10ms cross-fade
    static constexpr float NOISE_FLOOR = 0.002f;       // ~-54 dBFS

    struct Voice {
        bool active = false;
        int sampleInGrain = 0;
        int grainLen = 0;
        int64_t sourceAbs = 0;
    };

    std::vector<float> m_buf;
    int64_t m_writeAbs = 0;
    int m_period = 0;
    bool m_periodConfident = false;
    int m_holdSamples = 0;
    float m_lockFade = 0.0f;
    int m_samplesSinceDetect = 0;
    int m_warmup = 0;
    float m_pitchScale = 0.5f;
    Voice m_voice[N_VOICES];
    int m_samplesUntilNextEpoch = 0;
    int m_nextVoice = 0;
    int64_t m_inputEpochAbs = 0;
    double m_inputEpochFrac = 0.0;
    float m_rmsAcc = 0.0f;

    inline float sampleAt(int64_t absPos) const {
        if (absPos < 0 || absPos >= m_writeAbs) return 0.0f;
        if (m_writeAbs - absPos > BUF - 1) return 0.0f;
        return m_buf[(size_t)((absPos % BUF + BUF) % BUF)];
    }

    void detectPeriod() {
        const int W = DETECT_WIN;
        if (m_writeAbs < W) { m_periodConfident = false; return; }
        std::vector<float> x(W);
        for (int i = 0; i < W; i++) x[i] = sampleAt(m_writeAbs - W + i);

        // Pre-gate on signal energy.
        float energy = 0.0f;
        for (int i = 0; i < W; i++) energy += x[i] * x[i];
        if (energy < (float)W * NOISE_FLOOR * NOISE_FLOOR) {
            m_periodConfident = false;
            return;
        }

        const int maxTau = MAX_PERIOD;
        std::vector<float> d(maxTau + 1, 0.0f);
        std::vector<float> dPrime(maxTau + 1, 1.0f);
        for (int tau = 1; tau <= maxTau; tau++) {
            float sum = 0.0f;
            int limit = W - tau;
            if (limit < 32) { dPrime[tau] = 1.0f; continue; }
            for (int j = 0; j < limit; j++) {
                float diff = x[j] - x[j + tau];
                sum += diff * diff;
            }
            d[tau] = sum;
        }
        dPrime[0] = 1.0f;
        float running = 0.0f;
        for (int tau = 1; tau <= maxTau; tau++) {
            running += d[tau];
            dPrime[tau] = running > 0.0f ? d[tau] * (float)tau / running : 1.0f;
        }
        const float THRESH = 0.15f;
        int chosen = -1;
        for (int tau = MIN_PERIOD; tau <= maxTau; tau++) {
            if (dPrime[tau] < THRESH) {
                while (tau + 1 <= maxTau && dPrime[tau + 1] < dPrime[tau]) tau++;
                chosen = tau;
                break;
            }
        }
        if (chosen < 0) { m_periodConfident = false; return; }
        float refined = (float)chosen;
        if (chosen > 0 && chosen < maxTau) {
            float a = dPrime[chosen - 1];
            float b = dPrime[chosen];
            float c = dPrime[chosen + 1];
            float denom = 2.0f * (2.0f * b - a - c);
            if (fabsf(denom) > 1e-9f) refined = (float)chosen + (a - c) / denom;
        }
        int newPeriod = (int)(refined + 0.5f);
        if (newPeriod < MIN_PERIOD) newPeriod = MIN_PERIOD;
        if (newPeriod > MAX_PERIOD) newPeriod = MAX_PERIOD;

        // Clamp T change rate to prevent grain-length jumps when YIN flips
        // between octaves on noisy onsets.
        if (m_period > 0) {
            int maxDelta = m_period / 16 + 1;  // ±6% per detection
            if (newPeriod > m_period + maxDelta) newPeriod = m_period + maxDelta;
            if (newPeriod < m_period - maxDelta) newPeriod = m_period - maxDelta;
        }
        m_period = newPeriod;
        m_periodConfident = true;
    }
};
