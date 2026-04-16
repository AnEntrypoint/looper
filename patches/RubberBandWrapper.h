#ifndef RUBBERBAND_WRAPPER_H
#define RUBBERBAND_WRAPPER_H

#include <stdint.h>
#include <string.h>
#include "signalsmith/signalsmith-stretch.h"

class RubberBandWrapper {
  signalsmith::stretch::SignalsmithStretch<float> m_stretch;
  float m_pitchScale;
  float m_formant;
  size_t m_channels;
  uint32_t m_processedFrames;
  uint32_t m_retrievedFrames;

  static constexpr size_t MAX_BLOCK = 512;
  float m_feed_L[MAX_BLOCK];
  float m_feed_R[MAX_BLOCK];
  float m_retr_L[MAX_BLOCK];
  float m_retr_R[MAX_BLOCK];

public:
  RubberBandWrapper(size_t sampleRate, size_t channels)
    : m_pitchScale(1.0f), m_formant(0.0f), m_channels(channels),
      m_processedFrames(0), m_retrievedFrames(0)
  {
    int blockSamples = 512;
    int intervalSamples = 192;
    m_stretch.configure((int)channels, blockSamples, intervalSamples);
    memset(m_feed_L, 0, sizeof(m_feed_L));
    memset(m_feed_R, 0, sizeof(m_feed_R));
    memset(m_retr_L, 0, sizeof(m_retr_L));
    memset(m_retr_R, 0, sizeof(m_retr_R));
  }

  ~RubberBandWrapper() {}

  void feedAudio(const int16_t *left, const int16_t *right, size_t samples) {
    for (size_t i = 0; i < samples; i++) {
      m_feed_L[i] = (float)left[i]  / 32768.0f;
      m_feed_R[i] = (float)right[i] / 32768.0f;
    }
    m_processedFrames += samples;
  }

  size_t retrieveAudio(int16_t *left, int16_t *right, size_t samples) {
    const float *in[2]  = { m_feed_L, m_feed_R };
    float       *out[2] = { m_retr_L, m_retr_R };
    m_stretch.process(in, (int)samples, out, (int)samples);
    for (size_t i = 0; i < samples; i++) {
      float l = m_retr_L[i] * 32768.0f;
      float r = m_retr_R[i] * 32768.0f;
      left[i]  = (int16_t)(l > 32767.0f ? 32767 : (l < -32768.0f ? -32768 : (int16_t)l));
      right[i] = (int16_t)(r > 32767.0f ? 32767 : (r < -32768.0f ? -32768 : (int16_t)r));
    }
    m_retrievedFrames += samples;
    return samples;
  }

  void setPitchScale(float scale) {
    m_pitchScale = scale;
    m_stretch.setTransposeFactor(scale, m_formant);
  }

  void setFormant(float norm) {
    // norm 0-1: maps to tonalityLimit 0-0.08
    // At 48kHz: tonalityLimit 0.08 → freqTonalityLimit ~3.8kHz cutoff
    // Below cutoff: pitch shifts. Above cutoff: passes through (preserves formants)
    // 0 = no preservation (uniform shift), 1 = strong preservation
    m_formant = norm * 0.08f;
    m_stretch.setTransposeFactor(m_pitchScale, m_formant);
  }

  void setTempoRatio(float) {}
  void updateRatios() {}

  struct DebugState {
    float pitchScale;
    uint32_t processedFrames;
    uint32_t retrievedFrames;
  };

  DebugState getDebugState() const {
    return { m_pitchScale, m_processedFrames, m_retrievedFrames };
  }
};

#endif
