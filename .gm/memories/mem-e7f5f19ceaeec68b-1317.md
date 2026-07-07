---
key: mem-e7f5f19ceaeec68b-1317
ns: default
created: 1782066212964
updated: 1782066212964
---

## Looper live pitch shifting + formant (MIDI-driven, RubberBandWrapper.h)

pLivePitchWrapper allocated unconditionally; loopMachine::update bypasses wrapper when liveEngaged==false (zero latency). ONE algo EngineSoladSnac (patches/soladSnacOctaver.h): pitch-only PSOLA, solad single-delay-line variable-speed read + phase-coherent splices + McLeod SNAC pitch detection. Splices MUST be frequency-neutral (load-bearing): triggerSpliceByPeriod snaps net jump to whole number of periods (nWhole=round(rawJump/per)) so read rate stays exactly m_scale (0.5 at -12). Quiet-input coast: m_envSlow<0.004 repositions readers silently (no click at next note). Latency ~4ms budget: m_respliceFrac=1 (reader gap offset+<=1 period), INITIAL_READ_OFFSET_DEFAULT=64 (1.3ms). Telemetry audio.cpp eng line: eff=0.5000 (read rate, must be exact at -12), perr=0 (splice freq-neutral). Formant depth CC53 [-1,+1] center deadzone via grainFormant.h (pitch=grain emission rate, formant=grain playback speed fm=2^depth; Hann 2-period OLA; streaming gap-bound resplices whole periods). MIDI: CC1 modwheel +/-12 (deadzone 59-69), CC52 linear +/-12, ch1 note-on (0x91) toggle+pitch=note-60, ch2 note-on (0x92) always engage. CC100/101/102/103 = engine tuning (UDP-inject). Host+Pi validated -12 exact E2-E4. See [[looper-audio-architecture]].
