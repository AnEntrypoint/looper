#ifndef RUBBERBAND_WRAPPER_H
#define RUBBERBAND_WRAPPER_H

#include <stdint.h>
#include <string.h>
#include <math.h>
#include "signalsmith/signalsmith-stretch.h"
#include "yinPsolaOctaver.h"
#include "sincFormantOctaver.h"
#include "soladSnacOctaver.h"

class RubberBandWrapper {
  signalsmith::stretch::SignalsmithStretch<float> m_stretch;
  EngineYinPsola m_psolaL;
  EngineYinPsola m_psolaR;
  SincFormantOctaver m_sincL;
  SincFormantOctaver m_sincR;
  EngineSoladSnac m_soladL;
  EngineSoladSnac m_soladR;
  float m_pitchScale;
  float m_formant;
  size_t m_channels;
  uint32_t m_processedFrames;
  uint32_t m_retrievedFrames;
  float m_formantRes = 0.0f;
  float m_formantFreq = 800.0f;
  bool  m_engaged = false;

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
    // Optimal-quality config in the 3-8ms latency budget.
    // Empirically chosen via scripts/quality-harness.cpp + run-all-engines.ps1
    // sweep over (64..256, 16..96) at scale=0.5 on 8-signal corpus (pure
    // sines, harmonic-rich synthetic guitar tones, plucks at E2/A2/D3).
    // Winner: blockSamples=128, intervalSamples=48.
    // Per scripts/quality-results/all.jsonl: composite score 78.6 (best in
    // budget without splitComputation, which the in-tree signalsmith stub
    // doesn't expose), pluck_lat 2.58ms (envelope-onset), sust_fund_err
    // 3.20Hz, pluck_thd 61% (vs prior 64/32 at 221% pluck_thd — 3.6× cleaner).
    int blockSamples = 128;
    int intervalSamples = 48;
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
    // ONE algo for every pitch ratio: EngineSoladSnac.
    //
    // Pitch-only (no time-stretch) shifter. Single delay line + variable
    // read pointer with phase-coherent splices at integer-period offsets,
    // pitch detection via McLeod SNAC. Host A/B confirmed pitch lock
    // <0.3 Hz across E2-E4 AND preserved transient timing (vs prior
    // sinc-delay engine which time-stretched output → played slower).
    //
    // Formant-depth knob (m_formantDepth ∈ [-1, +1]) drives a pre-resample
    // stage feeding the delay line:
    //   d = 0  : natural pitch shift, formants slide with pitch
    //   d = 1  : formants preserved at original pitch (vocal-octave)
    //   d > 1  : formants exaggerated opposite to pitch (extreme)
    //   d < 0  : formants doubled-down with pitch (huge/monster)
    // Implementation: preRate = pow(scale, -depth). Engine resamples by
    // scale, net formant shift = pow(scale, 1-depth).
    //
    // Wet/dry crossfade (m_engaged): currently routed via the caller's
    // gating in loopMachine; the engine always runs at its native
    // ~4 ms read offset so engage/disengage doesn't introduce a latency
    // step. signalsmith + sinc + PSOLA still linked (used by loop-clip
    // RubberBand path) but not in the live pitch path.
    m_soladL.processBlock(m_feed_L, m_retr_L, (int)samples);
    m_soladR.processBlock(m_feed_R, m_retr_R, (int)samples);

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
    m_psolaL.configure(48000.0f, scale);
    m_psolaR.configure(48000.0f, scale);
    m_sincL.setPitchScale(scale);
    m_sincR.setPitchScale(scale);
    m_soladL.setPitchScale(scale);
    m_soladR.setPitchScale(scale);
  }

  // Wet/dry crossfade (presently no-op for solad which always pitches —
  // engage/disengage handled in loopMachine by gating the call into the
  // wrapper). Kept for API compatibility.
  void setEngaged(bool on) {
    m_engaged = on;
    m_sincL.setEngaged(on);
    m_sincR.setEngaged(on);
  }
  bool isEngaged() const { return m_engaged; }

  // Single-knob formant depth control, ∈ [-1, +1].
  // Drives the solad-snac pre-resample stage. Also pushes the value into
  // the legacy signalsmith formant factor for any caller that still uses
  // signalsmith on a non-live code path.
  void setFormant(float depth) {
    m_formant = depth * 0.12f;
    m_stretch.setTransposeFactor(m_pitchScale, m_formant);
    m_soladL.setFormantDepth(depth);
    m_soladR.setFormantDepth(depth);
  }

  // Legacy three-knob API: brightness still drives formant depth, the
  // peaking/freq knobs are ignored on solad (they belonged to the old
  // SincFormantOctaver post-EQ).
  void setFormantEq(float brightness, float /*resonance*/, float /*freqHz*/) {
    m_soladL.setFormantDepth(brightness);
    m_soladR.setFormantDepth(brightness);
  }

  // Live-tunable solad-snac runtime params (CC100-107 via UDP-MIDI inject).
  // Lets us sweep latency / splice / fidelity / bypasses in-place without
  // rebuilding to find the param combination that eliminates periodic
  // misalignment glitches.
  void setEngineReadOffset(int samples) {
    m_soladL.setInitialReadOffset(samples);
    m_soladR.setInitialReadOffset(samples);
  }
  void setEngineXfadeScale(float s) {
    m_soladL.setXfadeScale(s);
    m_soladR.setXfadeScale(s);
  }
  void setEngineFidelity(float f) {
    m_soladL.setFidelityThresh(f);
    m_soladR.setFidelityThresh(f);
  }
  void setEnginePreBypass(bool on) {
    m_soladL.setPreResampleBypass(on);
    m_soladR.setPreResampleBypass(on);
  }
  void setEngineSpliceSnap(bool on) {
    m_soladL.setSpliceSnap(on);
    m_soladR.setSpliceSnap(on);
  }
  void setEngineSpliceMatch(bool on) {
    m_soladL.setSpliceMatch(on);
    m_soladR.setSpliceMatch(on);
  }
  void setEngineDriftLow(int s) {
    m_soladL.setDriftLowBand(s);
    m_soladR.setDriftLowBand(s);
  }
  void setEngineDriftHigh(int s) {
    m_soladL.setDriftHighHead(s);
    m_soladR.setDriftHighHead(s);
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
