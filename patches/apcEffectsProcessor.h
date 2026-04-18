#ifndef APC_EFFECTS_PROCESSOR_H
#define APC_EFFECTS_PROCESSOR_H

#include <stdint.h>
#include <string.h>
#include <math.h>

class apcEffectsProcessor {
  static constexpr size_t MAX_DELAY_SAMPLES = 96000;

  float m_hpCutoff;
  float m_lpCutoff;
  float m_lpRes;
  float m_reverbAmount;
  float m_delayAmount;
  float m_time;

  // Tape delay state
  float m_delayBuffer[2][MAX_DELAY_SAMPLES];
  size_t m_delayWritePos;
  float m_delayReadPos;       // fractional read position for smooth tape scrub
  float m_delayTargetSamples; // target delay length in samples

  // 2-pole SVF lowpass state (per channel)
  float m_lpIc1eq[2];
  float m_lpIc2eq[2];

  // 2-pole SVF highpass state (per channel)
  float m_hpIc1eq[2];
  float m_hpIc2eq[2];

  // Reverb: 8 delay lines for richer sound, Freeverb-style
  static constexpr int REVERB_LINES = 8;
  float m_reverbLines[REVERB_LINES][4096];
  size_t m_reverbPos[REVERB_LINES];
  float m_reverbFilter[REVERB_LINES];
  const size_t m_reverbLens[REVERB_LINES] = {
    2473, 2767, 3217, 3571, 3907, 4057, 2143, 1933
  };

  size_t m_sampleRate;

  // Ableton-style SVF lowpass: cutoff 0-1 (normalized), resonance 0-1
  void svfLowpass(float input, float &ic1eq, float &ic2eq,
                  float cutoff, float res, float &out) {
    // Map cutoff 0-1 to frequency: 20Hz at 0, ~20kHz at 1 (exponential)
    // Clamp frequency below Nyquist/2 to keep tanf prewarp stable (prevents ring-mod IM artifacts)
    float freq = 20.0f * powf(1000.0f, cutoff);
    float maxFreq = (float)m_sampleRate * 0.45f;
    if (freq > maxFreq) freq = maxFreq;
    float g = tanf(3.14159265f * freq / (float)m_sampleRate);
    // Q range 0.5 -> 25 (high resonance available for classic filter sweeps)
    float Q = 0.5f + res * 24.5f;
    float k = 1.0f / Q;
    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1;
    float a3 = g * a2;

    float v3 = input - ic2eq;
    float v1 = a1 * ic1eq + a2 * v3;
    float v2 = ic2eq + a2 * ic1eq + a3 * v3;
    ic1eq = 2.0f * v1 - ic1eq;
    ic2eq = 2.0f * v2 - ic2eq;
    out = v2;
  }

  // Ableton-style SVF highpass: cutoff 0-1 (normalized)
  void svfHighpass(float input, float &ic1eq, float &ic2eq,
                   float cutoff, float &out) {
    float freq = 20.0f * powf(1000.0f, cutoff);
    float g = tanf(3.14159265f * freq / (float)m_sampleRate);
    float k = 1.41421356f; // Q = 0.707 (Butterworth, no resonance)
    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1;
    float a3 = g * a2;

    float v3 = input - ic2eq;
    float v1 = a1 * ic1eq + a2 * v3;
    float v2 = ic2eq + a2 * ic1eq + a3 * v3;
    ic1eq = 2.0f * v1 - ic1eq;
    ic2eq = 2.0f * v2 - ic2eq;
    out = input - k * v1 - v2; // highpass output
  }

  // Linear interpolation for tape delay read
  float readDelayInterp(int ch, float pos) {
    int idx0 = (int)pos;
    int idx1 = (idx0 + 1) % MAX_DELAY_SAMPLES;
    idx0 = idx0 % MAX_DELAY_SAMPLES;
    float frac = pos - (float)(int)pos;
    return m_delayBuffer[ch][idx0] * (1.0f - frac) + m_delayBuffer[ch][idx1] * frac;
  }

public:
  apcEffectsProcessor(size_t sampleRate)
    : m_hpCutoff(0.0f), m_lpCutoff(1.0f), m_lpRes(0.0f),
      m_reverbAmount(0.0f), m_delayAmount(0.0f), m_time(0.5f),
      m_delayWritePos(0), m_delayReadPos(0.0f), m_delayTargetSamples(0.0f),
      m_sampleRate(sampleRate)
  {
    memset(m_delayBuffer, 0, sizeof(m_delayBuffer));
    memset(m_lpIc1eq, 0, sizeof(m_lpIc1eq));
    memset(m_lpIc2eq, 0, sizeof(m_lpIc2eq));
    memset(m_hpIc1eq, 0, sizeof(m_hpIc1eq));
    memset(m_hpIc2eq, 0, sizeof(m_hpIc2eq));
    memset(m_reverbLines, 0, sizeof(m_reverbLines));
    memset(m_reverbPos, 0, sizeof(m_reverbPos));
    memset(m_reverbFilter, 0, sizeof(m_reverbFilter));
    m_delayTargetSamples = m_time * 450.0f * sampleRate / 1000.0f + 50.0f * sampleRate / 1000.0f;
  }

  void setHighpassCutoff(float norm) { m_hpCutoff = norm; }
  void setLowpassCutoff(float norm) { m_lpCutoff = norm; }
  void setLowpassResonance(float norm) { m_lpRes = norm; }
  void setReverbAmount(float norm) { m_reverbAmount = norm; }
  void setDelayAmount(float norm) { m_delayAmount = norm; }

  void setTime(float norm) {
    m_time = norm;
    float delayMs = m_time * 990.0f + 10.0f;
    m_delayTargetSamples = delayMs * (float)m_sampleRate / 1000.0f;
    if (m_delayTargetSamples > (float)(MAX_DELAY_SAMPLES - 1))
      m_delayTargetSamples = (float)(MAX_DELAY_SAMPLES - 1);
  }

  void processFilterAndSends(float *left, float *right, size_t samples, size_t sampleRate) {
    m_sampleRate = sampleRate;

    for (size_t i = 0; i < samples; i++) {
      float l = left[i];
      float r = right[i];

      // Highpass filter (2-pole SVF, Butterworth Q, no resonance knob)
      if (m_hpCutoff > 0.01f) {
        svfHighpass(l, m_hpIc1eq[0], m_hpIc2eq[0], m_hpCutoff, l);
        svfHighpass(r, m_hpIc1eq[1], m_hpIc2eq[1], m_hpCutoff, r);
      }

      // Lowpass filter (2-pole SVF with resonance)
      if (m_lpCutoff < 0.99f) {
        svfLowpass(l, m_lpIc1eq[0], m_lpIc2eq[0], m_lpCutoff, m_lpRes, l);
        svfLowpass(r, m_lpIc1eq[1], m_lpIc2eq[1], m_lpCutoff, m_lpRes, r);
      }

      // Tape delay: smoothly interpolate read position toward target
      float currentDelay = (float)m_delayWritePos - m_delayReadPos;
      if (currentDelay < 0) currentDelay += (float)MAX_DELAY_SAMPLES;
      float targetDelay = m_delayTargetSamples;

      // Smooth slew toward target: this creates the tape pitch effect
      // Rate of 0.9999 gives a slow, musical pitch bend when time changes
      float newDelay = currentDelay + (targetDelay - currentDelay) * 0.0001f;
      m_delayReadPos = (float)m_delayWritePos - newDelay;
      if (m_delayReadPos < 0.0f) m_delayReadPos += (float)MAX_DELAY_SAMPLES;

      float delayL = readDelayInterp(0, m_delayReadPos);
      float delayR = readDelayInterp(1, m_delayReadPos);

      // Feedback coefficient: scales with delay amount. Max 1.05 allows self-oscillating runaway for feedback FX.
      float feedback = m_delayAmount * 1.05f;

      // Write to delay buffer: input + feedback
      m_delayBuffer[0][m_delayWritePos] = l + delayL * feedback;
      m_delayBuffer[1][m_delayWritePos] = r + delayR * feedback;
      m_delayWritePos = (m_delayWritePos + 1) % MAX_DELAY_SAMPLES;

      // Mix delay wet signal
      l = l + delayL * m_delayAmount;
      r = r + delayR * m_delayAmount;

      // Reverb: parallel comb filters with lowpass damping
      if (m_reverbAmount > 0.001f) {
        float revL = 0.0f, revR = 0.0f;
        // Time controls reverb decay: maps to feedback 0.7-1.02 (runaway at max for freeze/infinity FX)
        float revFeedback = 0.7f + m_time * 0.32f;

        for (int line = 0; line < REVERB_LINES; line++) {
          float lineOut = m_reverbLines[line][m_reverbPos[line]];

          // Damping lowpass on feedback
          m_reverbFilter[line] = m_reverbFilter[line] * 0.7f + lineOut * 0.3f;
          float dampedOut = m_reverbFilter[line];

          float input = (line < 4) ? l * 0.15f : r * 0.15f;
          m_reverbLines[line][m_reverbPos[line]] = input + dampedOut * revFeedback;

          if (line < 4) revL += dampedOut;
          else          revR += dampedOut;

          m_reverbPos[line] = (m_reverbPos[line] + 1) % m_reverbLens[line];
        }

        l += revL * m_reverbAmount * 0.25f;
        r += revR * m_reverbAmount * 0.25f;
      }

      left[i] = l;
      right[i] = r;
    }
  }
};

#endif
