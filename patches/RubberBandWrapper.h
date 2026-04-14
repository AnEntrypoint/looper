#ifndef RUBBERBAND_WRAPPER_H
#define RUBBERBAND_WRAPPER_H

#include <stdint.h>
#include <string.h>
#include <rubberband/RubberBandStretcher.h>

class RubberBandWrapper {
  static constexpr size_t INPUT_RING_SIZE = 4096;
  static constexpr size_t OUTPUT_RING_SIZE = 8192;
  static constexpr size_t RING_MASK_IN = INPUT_RING_SIZE - 1;
  static constexpr size_t RING_MASK_OUT = OUTPUT_RING_SIZE - 1;

  static constexpr size_t MAX_BLOCK = 256;

  RubberBand::RubberBandStretcher *m_rb;
  float m_in_ring[INPUT_RING_SIZE * 2];
  float m_out_ring[OUTPUT_RING_SIZE * 2];
  volatile uint32_t m_in_wr, m_in_rd;
  volatile uint32_t m_out_wr, m_out_rd;
  volatile float m_tempoRatio;
  volatile float m_pitchScale;
  size_t m_sampleRate;
  uint32_t m_processedFrames;
  uint32_t m_retrievedFrames;
  float m_feed_L[MAX_BLOCK];
  float m_feed_R[MAX_BLOCK];
  float m_retr_L[MAX_BLOCK];
  float m_retr_R[MAX_BLOCK];

public:
  RubberBandWrapper(size_t sampleRate, size_t channels)
    : m_sampleRate(sampleRate), m_in_wr(0), m_in_rd(0),
      m_out_wr(0), m_out_rd(0), m_tempoRatio(1.0f),
      m_pitchScale(1.0f), m_processedFrames(0), m_retrievedFrames(0)
  {
    using namespace RubberBand;
    int opts = RubberBandStretcher::OptionProcessRealTime |
               RubberBandStretcher::OptionEngineFaster |
               RubberBandStretcher::OptionWindowShort;
    m_rb = new RubberBandStretcher(sampleRate, channels, opts, 1.0, 1.0);
    m_rb->setMaxProcessSize(524288);
    memset(m_in_ring, 0, sizeof(m_in_ring));
    memset(m_out_ring, 0, sizeof(m_out_ring));
  }

  ~RubberBandWrapper() { delete m_rb; }

  void feedAudio(const int16_t *left, const int16_t *right, size_t samples) {
    for (size_t i = 0; i < samples; i++) {
      m_feed_L[i] = (float)left[i] / 32768.0f;
      m_feed_R[i] = (float)right[i] / 32768.0f;
    }
    const float *ptrs[2] = { m_feed_L, m_feed_R };
    m_rb->process(ptrs, samples, false);
    m_processedFrames += samples;
  }

  size_t retrieveAudio(int16_t *left, int16_t *right, size_t maxSamples) {
    float *out[2] = { m_retr_L, m_retr_R };
    size_t avail = m_rb->retrieve(out, maxSamples);
    for (size_t i = 0; i < avail; i++) {
      float l = m_retr_L[i] * 32768.0f;
      float r = m_retr_R[i] * 32768.0f;
      left[i]  = (int16_t)(l > 32767.0f ? 32767 : (l < -32768.0f ? -32768 : (int16_t)l));
      right[i] = (int16_t)(r > 32767.0f ? 32767 : (r < -32768.0f ? -32768 : (int16_t)r));
    }
    m_retrievedFrames += avail;
    return avail;
  }

  void setTempoRatio(float ratio) { m_tempoRatio = ratio; }
  void setPitchScale(float scale) { m_pitchScale = scale; }
  void updateRatios() {
    float tempo = m_tempoRatio;
    float pitch = m_pitchScale;
    m_rb->setTimeRatio(tempo);
    m_rb->setPitchScale(pitch);
  }

  size_t getSamplesRequired() const { return m_rb->getSamplesRequired(); }
  int available() const { return m_rb->available(); }
  size_t getStartDelay() const { return m_rb->getStartDelay(); }

  struct DebugState {
    float tempoRatio;
    float pitchScale;
    uint32_t processedFrames;
    uint32_t retrievedFrames;
    size_t samplesRequired;
    int availableOutput;
    int64_t estimatedLatencyMs;
  };

  DebugState getDebugState() const {
    return {
      m_tempoRatio,
      m_pitchScale,
      m_processedFrames,
      m_retrievedFrames,
      m_rb->getSamplesRequired(),
      m_rb->available(),
      (int64_t)m_rb->getStartDelay() * 1000 / m_sampleRate
    };
  }
};

#endif
