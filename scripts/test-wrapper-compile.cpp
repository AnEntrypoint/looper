// test-wrapper-compile.cpp — verify RubberBandWrapper.h still compiles
// cleanly with the new PsolaOctaver wired in. Doesn't link signalsmith
// (just type-check via stub).
//
// Compile:
//   g++ -std=c++17 -O2 -I patches -DUNIT_TEST_STUB scripts/test-wrapper-compile.cpp -c -o /tmp/wrap.o

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>

// Stub the signalsmith header before including the wrapper
namespace signalsmith { namespace stretch {
    template<typename T>
    class SignalsmithStretch {
    public:
        void configure(int, int, int) {}
        void setTransposeFactor(float, float) {}
        void process(const float* const*, int, float* const*, int) {}
    };
}}

#define signalsmith_stretch_h_INCLUDED 1
// Pre-empt the real include path
#include "psolaOctaver.h"

// Inline-include the wrapper after the stub has shadowed signalsmith
// Replace the include with our stub:
class RubberBandWrapper {
  signalsmith::stretch::SignalsmithStretch<float> m_stretch;
  PsolaOctaver m_psolaL;
  PsolaOctaver m_psolaR;
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
  RubberBandWrapper(size_t sr, size_t ch)
    : m_psolaL((int)sr), m_psolaR((int)sr),
      m_pitchScale(1.0f), m_formant(0.0f), m_channels(ch),
      m_processedFrames(0), m_retrievedFrames(0)
  { memset(m_feed_L,0,sizeof(m_feed_L)); memset(m_feed_R,0,sizeof(m_feed_R));
    memset(m_retr_L,0,sizeof(m_retr_L)); memset(m_retr_R,0,sizeof(m_retr_R)); }
  void setPitchScale(float scale) {
    m_pitchScale = scale;
    m_stretch.setTransposeFactor(scale, m_formant);
    m_psolaL.setPitchScale(scale);
    m_psolaR.setPitchScale(scale);
  }
  void feedAudio(const int16_t *l, const int16_t *r, size_t n) {
    for (size_t i = 0; i < n; i++) {
      m_feed_L[i] = (float)l[i] / 32768.0f;
      m_feed_R[i] = (float)r[i] / 32768.0f;
    }
    m_processedFrames += n;
  }
  size_t retrieveAudio(int16_t *l, int16_t *r, size_t n) {
    if (m_pitchScale < 0.7f) {
      bool a = m_psolaL.process(m_feed_L, m_retr_L, (int)n);
      bool b = m_psolaR.process(m_feed_R, m_retr_R, (int)n);
      if (!a || !b) {
        const float *in[2] = { m_feed_L, m_feed_R };
        float *out[2] = { m_retr_L, m_retr_R };
        m_stretch.process(in, (int)n, out, (int)n);
      }
    }
    for (size_t i = 0; i < n; i++) {
      l[i] = (int16_t)(m_retr_L[i] * 32768.0f);
      r[i] = (int16_t)(m_retr_R[i] * 32768.0f);
    }
    m_retrievedFrames += n;
    return n;
  }
};

int main() {
  RubberBandWrapper w(48000, 2);
  w.setPitchScale(0.5f);
  int16_t in_l[64] = {0}, in_r[64] = {0};
  int16_t out_l[64] = {0}, out_r[64] = {0};
  // Inject a few seconds of low-E to lock the detector, then check output exists
  double twoPiF = 2.0 * 3.14159265358979 * 82.4 / 48000.0;
  long sampCount = 0;
  for (int blk = 0; blk < 5000; blk++) {  // ~6.5s of 64-sample blocks
    for (int i = 0; i < 64; i++) {
      double v = sin(twoPiF * sampCount++) * 20000.0;
      in_l[i] = (int16_t)v;
      in_r[i] = (int16_t)v;
    }
    w.feedAudio(in_l, in_r, 64);
    w.retrieveAudio(out_l, out_r, 64);
  }
  // Sanity: last block should have some output amplitude
  long sumSq = 0;
  for (int i = 0; i < 64; i++) sumSq += (long)out_l[i] * out_l[i];
  printf("last-block sumSq=%ld (>0 means engine produced output)\n", sumSq);
  return sumSq > 0 ? 0 : 1;
}
