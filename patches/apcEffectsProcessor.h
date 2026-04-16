#ifndef APC_EFFECTS_PROCESSOR_H
#define APC_EFFECTS_PROCESSOR_H

#include <stdint.h>
#include <string.h>

// Effects processor with delay, reverb, and filtering
class apcEffectsProcessor {
  static constexpr size_t MAX_DELAY_SAMPLES = 96000;
  static constexpr size_t REVERB_LINE_LENGTHS[4] = {2473, 2767, 3217, 3571};

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
  float m_lpFilterState[2];
  float m_hpFilterState[2];

  // Reverb line buffers (Freeverb-style)
  float m_reverbLines[4][4096];
  size_t m_reverbPos[4];
  float m_reverbFilter[4];

  size_t m_sampleRate;

  void updateDelayTime(size_t sampleRate) {
    m_delaySamples = (size_t)(m_delayTime * sampleRate);
    if (m_delaySamples > MAX_DELAY_SAMPLES) m_delaySamples = MAX_DELAY_SAMPLES;
  }

  float processOnePoleLowpass(float input, float &state, float cutoff) {
    float coef = cutoff;
    state = state + coef * (input - state);
    return state;
  }

  float processOnePoleHighpass(float input, float &state, float cutoff) {
    float coef = 1.0f - cutoff;
    float output = coef * (state + input - state);
    state = input;
    return output;
  }

public:
  apcEffectsProcessor(size_t sampleRate)
    : m_hpCutoff(0.0f), m_lpCutoff(1.0f), m_filterRes(0.0f),
      m_reverbAmount(0.0f), m_reverbTime(0.5f), m_delayAmount(0.0f), m_delayTime(0.5f),
      m_delayWritePos(0), m_sampleRate(sampleRate)
  {
    memset(m_delayBuffer, 0, sizeof(m_delayBuffer));
    memset(m_lpFilterState, 0, sizeof(m_lpFilterState));
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
      if (m_hpCutoff > 0.001f) {
        l = processOnePoleHighpass(l, m_hpFilterState[0], m_hpCutoff);
        r = processOnePoleHighpass(r, m_hpFilterState[1], m_hpCutoff);
      }

      // Low-pass filter (removes highs)
      if (m_lpCutoff < 0.999f) {
        l = processOnePoleLowpass(l, m_lpFilterState[0], m_lpCutoff);
        r = processOnePoleLowpass(r, m_lpFilterState[1], m_lpCutoff);
      }

      // Delay effect
      float delayL = 0.0f, delayR = 0.0f;
      if (m_delayAmount > 0.001f && m_delaySamples > 0) {
        size_t readPos = (m_delayWritePos - m_delaySamples + MAX_DELAY_SAMPLES) % MAX_DELAY_SAMPLES;
        delayL = m_delayBuffer[0][readPos];
        delayR = m_delayBuffer[1][readPos];
      }

      l = l + delayL * m_delayAmount * 0.5f;
      r = r + delayR * m_delayAmount * 0.5f;

      // Write to delay buffer with feedback
      m_delayBuffer[0][m_delayWritePos] = l * 0.8f;
      m_delayBuffer[1][m_delayWritePos] = r * 0.8f;
      m_delayWritePos = (m_delayWritePos + 1) % MAX_DELAY_SAMPLES;

      // Simple reverb via parallel allpass lines
      if (m_reverbAmount > 0.001f) {
        float revL = 0.0f, revR = 0.0f;

        for (int line = 0; line < 4; line++) {
          float lineOut = m_reverbLines[line][m_reverbPos[line]];
          float feedback = lineOut * (m_reverbTime * 0.9f);
          float input = (l + r) * 0.25f;

          m_reverbLines[line][m_reverbPos[line]] = input + feedback;
          m_reverbFilter[line] = m_reverbFilter[line] * 0.9f + lineOut * 0.1f;
          revL += m_reverbFilter[line];
          revR += m_reverbFilter[line];

          m_reverbPos[line] = (m_reverbPos[line] + 1) % REVERB_LINE_LENGTHS[line];
        }

        l += revL * m_reverbAmount * 0.1f;
        r += revR * m_reverbAmount * 0.1f;
      }

      left[i] = l;
      right[i] = r;
    }
  }
};

#endif
