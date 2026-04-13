#ifndef RUBBERBAND_WRAPPER_H
#define RUBBERBAND_WRAPPER_H

#include <cstdint>
#include <cstring>
#include <atomic>
#include <RubberBandStretcher.h>

class RubberBandWrapper {
  static constexpr size_t INPUT_RING_SIZE = 4096;
  static constexpr size_t OUTPUT_RING_SIZE = 8192;
  static constexpr size_t RING_MASK_IN = INPUT_RING_SIZE - 1;
  static constexpr size_t RING_MASK_OUT = OUTPUT_RING_SIZE - 1;

  RubberBand::RubberBandStretcher *m_rb;
  float m_in_ring[INPUT_RING_SIZE * 2];
  float m_out_ring[OUTPUT_RING_SIZE * 2];
  volatile uint32_t m_in_wr, m_in_rd;
  volatile uint32_t m_out_wr, m_out_rd;
  std::atomic<float> m_tempoRatio;
  std::atomic<float> m_pitchScale;
  size_t m_sampleRate;
  uint32_t m_processedFrames;
  uint32_t m_retrievedFrames;

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
    const float *ptrs[2] = { nullptr, nullptr };
    float tmpL[256], tmpR[256];
    for (size_t i = 0; i < samples; i++) {
      tmpL[i] = (float)left[i] / 32768.0f;
      tmpR[i] = (float)right[i] / 32768.0f;
    }
    ptrs[0] = tmpL; ptrs[1] = tmpR;
    m_rb->process(ptrs, samples, false);
    m_processedFrames += samples;
  }

  size_t retrieveAudio(int16_t *left, int16_t *right, size_t maxSamples) {
    float tmpL[256], tmpR[256];
    float *out[2] = { tmpL, tmpR };
    size_t avail = m_rb->retrieve(out, maxSamples);
    for (size_t i = 0; i < avail; i++) {
      float l = tmpL[i] * 32768.0f;
      float r = tmpR[i] * 32768.0f;
      left[i] = (int16_t)(l > 32767 ? 32767 : (l < -32768 ? -32768 : l));
      right[i] = (int16_t)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
    }
    m_retrievedFrames += avail;
    return avail;
  }

  void setTempoRatio(float ratio) { m_tempoRatio.store(ratio); }
  void setPitchScale(float scale) { m_pitchScale.store(scale); }
  void updateRatios() {
    float tempo = m_tempoRatio.load();
    float pitch = m_pitchScale.load();
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
      m_tempoRatio.load(),
      m_pitchScale.load(),
      m_processedFrames,
      m_retrievedFrames,
      m_rb->getSamplesRequired(),
      m_rb->available(),
      (int64_t)m_rb->getStartDelay() * 1000 / m_sampleRate
    };
  }
};

#endif
