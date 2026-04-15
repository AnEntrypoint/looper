#ifndef APC_EFFECTS_PROCESSOR_H
#define APC_EFFECTS_PROCESSOR_H

#include <stdint.h>
#include <string.h>
#include <cmath>
#include "signalsmith/dsp/filters.h"

class apcEffectsProcessor {
  static constexpr size_t MAX_DELAY_SAMPLES = 96000;

  // Filter (applied in series to output)
  signalsmith::dsp::BiquadStatic<float> m_hpFilter;
  signalsmith::dsp::BiquadStatic<float> m_lpFilter;

  float m_hpCutoff;
  float m_lpCutoff;
  float m_filterRes;

  // Effects sends (applied to output, not looped back)
  float m_reverbAmount;
  float m_reverbTime;
  float m_delayAmount;
  float m_delayTime;

  // Delay line for delay send
  float m_delayBuffer[2][MAX_DELAY_SAMPLES];
  size_t m_delayWritePos;
  float m_delayFeedback;
  float m_reverbFeedback;

public:
  apcEffectsProcessor(size_t sampleRate)
    : m_hpCutoff(0.0f), m_lpCutoff(1.0f), m_filterRes(0.0f),
      m_reverbAmount(0.0f), m_reverbTime(0.5f), m_delayAmount(0.0f), m_delayTime(0.5f),
      m_delayWritePos(0), m_delayFeedback(0.0f), m_reverbFeedback(0.0f)
  {
    memset(m_delayBuffer, 0, sizeof(m_delayBuffer));
    _updateFilters(sampleRate);
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
    m_reverbFeedback = norm * 0.8f;
  }

  void setReverbTime(float norm) {
    m_reverbTime = norm;
  }

  void setDelayAmount(float norm) {
    m_delayAmount = norm;
    m_delayFeedback = norm * 0.7f;
  }

  void setDelayTime(float norm) {
    m_delayTime = norm;
  }

  void processFilterAndSends(float *left, float *right, size_t samples, size_t sampleRate) {
    _updateFilters(sampleRate);
    size_t delayMaxSamples = (size_t)(m_delayTime * sampleRate * 2.0f);
    if (delayMaxSamples > MAX_DELAY_SAMPLES) delayMaxSamples = MAX_DELAY_SAMPLES;
    if (delayMaxSamples < 1000) delayMaxSamples = 1000;

    for (size_t i = 0; i < samples; i++) {
      float inL = left[i];
      float inR = right[i];

      float filterL = m_hpFilter.processSample(inL);
      float filterR = m_hpFilter.processSample(inR);
      filterL = m_lpFilter.processSample(filterL);
      filterR = m_lpFilter.processSample(filterR);

      float outL = filterL;
      float outR = filterR;

      if (m_delayAmount > 0.001f) {
        size_t delayPos = (size_t)(m_delayTime * delayMaxSamples);
        size_t readPos = (m_delayWritePos + delayMaxSamples - delayPos) % delayMaxSamples;
        float delayedL = m_delayBuffer[0][readPos];
        float delayedR = m_delayBuffer[1][readPos];

        outL += delayedL * m_delayAmount;
        outR += delayedR * m_delayAmount;

        m_delayBuffer[0][m_delayWritePos] = filterL + delayedL * m_delayFeedback;
        m_delayBuffer[1][m_delayWritePos] = filterR + delayedR * m_delayFeedback;
        m_delayWritePos = (m_delayWritePos + 1) % delayMaxSamples;
      }

      if (m_reverbAmount > 0.001f) {
        float reverbMix = (outL + outR) * 0.5f;
        float reverbOut = reverbMix * (1.0f + m_reverbFeedback);
        outL += reverbOut * m_reverbAmount;
        outR += reverbOut * m_reverbAmount;
      }

      left[i] = outL;
      right[i] = outR;
    }
  }

private:
  void _updateFilters(size_t sampleRate) {
    float nyquist = sampleRate * 0.5f;

    if (m_hpCutoff > 0.001f) {
      float hpFreq = m_hpCutoff * (nyquist * 0.5f);
      float Q = 0.707f + m_filterRes * 3.0f;
      m_hpFilter.setHighpass(hpFreq / sampleRate, Q);
    } else {
      m_hpFilter.reset();
    }

    if (m_lpCutoff < 0.999f) {
      float lpFreq = m_lpCutoff * nyquist;
      float Q = 0.707f + m_filterRes * 3.0f;
      m_lpFilter.setLowpass(lpFreq / sampleRate, Q);
    } else {
      m_lpFilter.reset();
    }
  }
};

#endif
