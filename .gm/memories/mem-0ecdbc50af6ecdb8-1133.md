---
key: mem-0ecdbc50af6ecdb8-1133
ns: default
created: 1779785774527
updated: 1779785774527
---

## Resolved mutable: formant-down-slowing-down-detune

REFRAMED by scripts/test-formant-netrate.cpp: pre_eff reads EXACTLY preRate (0.5000/1.0000/1.9999) and pre_perr~0 — so the integer-anchor works and there is NO splice detune leak. The real bug is the FORMANT MATH ITSELF: output fundamental is octave-WRONG. depth=-1 (preRate=0.5): 82->20.58Hz (=82/4, two octaves down, not one). depth=+1 (preRate=2): 82->85.38Hz (=~82, NO downshift). The pre-resample by preRate directly MULTIPLIES the final pitch by preRate because nothing compensates: main stage read rate is fixed at scale=0.5 regardless of preRate. AGENTS.md claims net formant=pow(scale,1-depth) via resample-back-by-scale but there is NO counter-resample stage. So formant-down literally drops pitch (the user hears slowing down) and formant-up cancels the -12. Center (depth=0, preRate=1, bypass) is correct because preRate=1 multiplies pitch by 1. The fix must make the pre-resample formant-only: the main pitch read must be divided by preRate so net output pitch stays scale, while the spectral envelope shifts. Integer-anchor + pre_eff/pre_perr telemetry retained.
