---
key: mem-73ffddf4c771e0ed-1059
ns: default
created: 1779803138111
updated: 1779803138111
---

## Resolved mutable: formant-via-grain-playback-speed

PROTOTYPE PROVEN on host (scripts/proto-grain-formant.cpp). Discrete-grain PSOLA: output epoch spacing = T/scale (sets -12 pitch), inEpoch advances T per emission, grain content read at rate `fm` (formant). Results at 196Hz->-12: outFund=98.0Hz EXACT at every formant 0.5/0.7/1.0/1.4/2.0 (pitch FULLY independent of formant — no leak), band ratio(2400/600) rises monotonically 0.026->0.081->0.422->0.831->32.8 (real envelope shift), clicks=0 at all settings (clean), constant resample ratio = stable (no wander). This is the correct architecture. INTEGRATION RISK: it is a discrete-grain resynth, different read structure than the engine continuous reader. Must not regress the proven continuous -12 at formant-center. Plan: add a grain-resynth formant path that uses the engine existing SNAC m_periodF + epoch tracking; engage it only when formant != center; keep continuous reader for center (byte-identical -12). Verify host (freq-neutral/clicks/envelope) + Pi (eff/perr/dry-floor) before ear-check.
