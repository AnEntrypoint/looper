#ifndef APC_EFFECTS_PROCESSOR_H
#define APC_EFFECTS_PROCESSOR_H

#include <stdint.h>
#include <string.h>

// Effects processor with delay, reverb, and filtering
class apcEffectsProcessor {
  static constexpr size_t MAX_DELAY_SAMPLES = 96000;

  float m_hpCutoff;
  float m_lpCutoff;
  float m_filterRes;
  float m_reverbAmount;
  float m_reverbTime;
  float m_delayAmount;
  float m_delayTime;

  float m_delayBuffer[2][MAX_DELAY_SAMPLES];
  size_t m_delayWritePos;
  size_t m_delaySamples;

  // Simple one-pole filter state
  float m_hpFilterState[2];

  // Resonant lowpass filter state (state-variable style)
  float m_lpBand[2];      // bandpass state
  float m_lpLow[2];       // lowpass state

  // Reverb line buffers (Freeverb-style)
  float m_reverbLines[4][4096];
  size_t m_reverbPos[4];
  float m_reverbFilter[4];
  const size_t m_reverbLineLengths[4] = {2473, 2767, 3217, 3571};

  size_t m_sampleRate;

  void updateDelayTime(size_t sampleRate) {
    m_delaySamples = (size_t)(m_delayTime * sampleRate);
    if (m_delaySamples > MAX_DELAY_SAMPLES) m_delaySamples = MAX_DELAY_SAMPLES;
  }

  float processOnePoleHighpass(float input, float &state, float cutoff) {
    float coef = 1.0f - cutoff;
    float output = coef * (state + input - state);
    state = input;
    return output;
  }

  float processResonantLowpass(float input, float &band, float &low, float cutoff, float resonance) {
    // State-variable filter with damped resonance
    float f = cutoff;
    float dampedRes = 1.0f - resonance * 0.8f;  // resonance reduces damping: 1.0 at res=0, 0.2 at res=1

    // Highpass: input minus lowpass
    float high = input - low;
    // Bandpass: integrate highpass with damping
    band = band * dampedRes + f * high;
    // Lowpass: integrate bandpass
    low = low + f * band;

    return low;
  }

public:
  apcEffectsProcessor(size_t sampleRate)
    : m_hpCutoff(0.0f), m_lpCutoff(1.0f), m_filterRes(0.0f),
      m_reverbAmount(0.0f), m_reverbTime(0.5f), m_delayAmount(0.0f), m_delayTime(0.5f),
      m_delayWritePos(0), m_sampleRate(sampleRate)
  {
    memset(m_delayBuffer, 0, sizeof(m_delayBuffer));
    memset(m_lpBand, 0, sizeof(m_lpBand));
    memset(m_lpLow, 0, sizeof(m_lpLow));
    memset(m_hpFilterState, 0, sizeof(m_hpFilterState));
    memset(m_reverbLines, 0, sizeof(m_reverbLines));
    memset(m_reverbPos, 0, sizeof(m_reverbPos));
    memset(m_reverbFilter, 0, sizeof(m_reverbFilter));
    updateDelayTime(sampleRate);
  }

  void setHighpassCutoff(float norm) {
    m_hpCutoff = norm;
  }

  void setLowpassCutoff(float norm) {
    m_lpCutoff = norm;
  }

  void setFilterResonance(float norm) {
    m_filterRes = norm;
  }

  void setReverbAmount(float norm) {
    m_reverbAmount = norm;
  }

  void setReverbTime(float norm) {
    m_reverbTime = norm;
  }

  void setDelayAmount(float norm) {
    m_delayAmount = norm;
  }

  void setDelayTime(float norm) {
    m_delayTime = norm;
  }

  void processFilterAndSends(float *left, float *right, size_t samples, size_t sampleRate) {
    updateDelayTime(sampleRate);

    for (size_t i = 0; i < samples; i++) {
      // Apply filters
      float l = left[i];
      float r = right[i];

      // High-pass filter (removes DC and rumble)
      // m_hpCutoff is 0-1, scales the internal highpass cutoff parameter
      // When hpCutoff=0 (knob left), no HP filtering (passes all)
      // When hpCutoff=1 (knob right), strong HP filtering (removes lows)
      float hpCutoff = m_hpCutoff * 0.3f;
      if (hpCutoff > 0.001f) {
        l = processOnePoleHighpass(l, m_hpFilterState[0], hpCutoff);
        r = processOnePoleHighpass(r, m_hpFilterState[1], hpCutoff);
      }

      // Low-pass filter with resonance (removes highs)
      // m_lpCutoff is 0-1 from MIDI: knob left = bright (min filtering), right = dark (max filtering)
      // m_filterRes is 0-1: amount of resonance (peak emphasis at cutoff)
      float lpCoef = m_lpCutoff * 0.1f;
      l = processResonantLowpass(l, m_lpBand[0], m_lpLow[0], lpCoef, m_filterRes);
      r = processResonantLowpass(r, m_lpBand[1], m_lpLow[1], lpCoef, m_filterRes);

      // Delay effect
      // m_delayTime is 0-1, maps to 50ms-500ms
      float delayMs = m_delayTime * 450.0f + 50.0f;
      size_t currentDelaySamples = (size_t)(delayMs * sampleRate / 1000.0f);
      if (currentDelaySamples > MAX_DELAY_SAMPLES) currentDelaySamples = MAX_DELAY_SAMPLES;

      float delayL = 0.0f, delayR = 0.0f;
      if (m_delayAmount > 0.001f && currentDelaySamples > 0) {
        size_t readPos = (m_delayWritePos + MAX_DELAY_SAMPLES - currentDelaySamples) % MAX_DELAY_SAMPLES;
        delayL = m_delayBuffer[0][readPos];
        delayR = m_delayBuffer[1][readPos];
      }

      // Mix: dry + wet * amount, with strong feedback
      l = l + delayL * m_delayAmount * 0.8f;
      r = r + delayR * m_delayAmount * 0.8f;

      // Write to delay buffer: input + strong feedback
      m_delayBuffer[0][m_delayWritePos] = l * 0.3f + delayL * 0.65f;
      m_delayBuffer[1][m_delayWritePos] = r * 0.3f + delayR * 0.65f;
      m_delayWritePos = (m_delayWritePos + 1) % MAX_DELAY_SAMPLES;

      // Simple reverb via parallel delay lines with feedback
      if (m_reverbAmount > 0.001f) {
        float revL = 0.0f, revR = 0.0f;
        float feedbackCoef = 0.5f + m_reverbTime * 0.45f;

        for (int line = 0; line < 4; line++) {
          float lineOut = m_reverbLines[line][m_reverbPos[line]];
          float input = (l + r) * 0.05f;

          m_reverbLines[line][m_reverbPos[line]] = input + lineOut * feedbackCoef;
          m_reverbFilter[line] = m_reverbFilter[line] * 0.8f + lineOut * 0.2f;
          revL += m_reverbFilter[line] * 0.6f;
          revR += m_reverbFilter[line] * 0.6f;

          m_reverbPos[line] = (m_reverbPos[line] + 1) % m_reverbLineLengths[line];
        }

        l += revL * m_reverbAmount * 0.5f;
        r += revR * m_reverbAmount * 0.5f;
      }

      left[i] = l;
      right[i] = r;
    }
  }
};

#endif
