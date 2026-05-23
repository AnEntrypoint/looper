#ifndef RUBBERBAND_WRAPPER_H
#define RUBBERBAND_WRAPPER_H

#include <stdint.h>
#include <string.h>
#include <math.h>
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

  static constexpr size_t OCT_DELAY = 2048;
  static constexpr size_t OCT_GRAIN = 256;  // 5.3ms grain, up-shift only (rate=2.0)
  float m_oct_dl_L[OCT_DELAY];
  float m_oct_dl_R[OCT_DELAY];
  uint32_t m_oct_wr;
  float m_oct_rd_a;
  float m_oct_rd_b;
  float m_oct_fade;

  static inline float sampleLerp(const float *ring, float pos, uint32_t wr) {
    uint32_t mask = OCT_DELAY - 1;
    uint32_t i0 = ((uint32_t)pos) & mask;
    uint32_t i1 = (i0 + 1) & mask;
    float frac = pos - (float)(uint32_t)pos;
    return ring[i0] + (ring[i1] - ring[i0]) * frac;
  }

  // Granular octaver only used for UP-shift (rate=2.0). Down-shift would need
  // PSOLA to avoid inter-head beat artifacts; signalsmith at 192/64 (~3.3ms)
  // is lower latency than the 16ms grain-half this would cost anyway.
  inline bool octaveActive() const {
    float s = m_pitchScale;
    return (s > 1.98f && s < 2.02f);
  }

  inline float octaveRate() const { return m_pitchScale; }

  void processOctave(const float *inL, const float *inR, float *outL, float *outR, size_t n) {
    float rate = octaveRate();
    uint32_t mask = OCT_DELAY - 1;
    float grainHalf = (float)(OCT_GRAIN / 2);
    for (size_t i = 0; i < n; i++) {
      m_oct_dl_L[m_oct_wr & mask] = inL[i];
      m_oct_dl_R[m_oct_wr & mask] = inR[i];
      m_oct_wr++;

      float aL = sampleLerp(m_oct_dl_L, m_oct_rd_a, m_oct_wr);
      float aR = sampleLerp(m_oct_dl_R, m_oct_rd_a, m_oct_wr);
      float bL = sampleLerp(m_oct_dl_L, m_oct_rd_b, m_oct_wr);
      float bR = sampleLerp(m_oct_dl_R, m_oct_rd_b, m_oct_wr);

      float fade = m_oct_fade;
      float wa = 0.5f * (1.0f + cosf(3.14159265f * fade));
      float wb = 1.0f - wa;
      outL[i] = aL * wa + bL * wb;
      outR[i] = aR * wa + bR * wb;

      m_oct_rd_a += rate;
      m_oct_rd_b += rate;
      m_oct_fade += 1.0f / (float)OCT_GRAIN;

      float gap_a = (float)m_oct_wr - m_oct_rd_a;
      float gap_b = (float)m_oct_wr - m_oct_rd_b;
      if (m_oct_fade >= 1.0f) {
        m_oct_fade -= 1.0f;
        m_oct_rd_a = (float)m_oct_wr - grainHalf;
      }
      if (m_oct_fade >= 0.5f && gap_b < 2.0f) {
        m_oct_rd_b = (float)m_oct_wr - grainHalf;
      }
      if (gap_a < 1.0f || gap_a > (float)(OCT_DELAY - 4)) m_oct_rd_a = (float)m_oct_wr - grainHalf;
      if (gap_b < 1.0f || gap_b > (float)(OCT_DELAY - 4)) m_oct_rd_b = (float)m_oct_wr - grainHalf - grainHalf * 0.5f;
    }
  }

public:
  RubberBandWrapper(size_t sampleRate, size_t channels)
    : m_pitchScale(1.0f), m_formant(0.0f), m_channels(channels),
      m_processedFrames(0), m_retrievedFrames(0),
      m_oct_wr(OCT_DELAY), m_oct_rd_a(OCT_DELAY - (float)(OCT_GRAIN / 2)),
      m_oct_rd_b(OCT_DELAY - (float)(OCT_GRAIN / 2) - (float)(OCT_GRAIN / 4)), m_oct_fade(0.0f)
  {
    int blockSamples = 192;
    int intervalSamples = 64;
    m_stretch.configure((int)channels, blockSamples, intervalSamples);
    memset(m_feed_L, 0, sizeof(m_feed_L));
    memset(m_feed_R, 0, sizeof(m_feed_R));
    memset(m_retr_L, 0, sizeof(m_retr_L));
    memset(m_retr_R, 0, sizeof(m_retr_R));
    memset(m_oct_dl_L, 0, sizeof(m_oct_dl_L));
    memset(m_oct_dl_R, 0, sizeof(m_oct_dl_R));
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
    // Zero-latency passthrough when pitch within ±0.1% of unity (mod-wheel deadzone).
    // Skips signalsmith STFT (~4ms) entirely.
    if (m_pitchScale > 0.999f && m_pitchScale < 1.001f) {
      memcpy(m_retr_L, m_feed_L, samples * sizeof(float));
      memcpy(m_retr_R, m_feed_R, samples * sizeof(float));
    } else if (octaveActive()) {
      processOctave(m_feed_L, m_feed_R, m_retr_L, m_retr_R, samples);
    } else {
      const float *in[2]  = { m_feed_L, m_feed_R };
      float       *out[2] = { m_retr_L, m_retr_R };
      m_stretch.process(in, (int)samples, out, (int)samples);
    }
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
    m_formant = norm * 0.12f;
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
