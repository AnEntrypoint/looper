---
key: mem-f7d2cf25e9eb36be-467
ns: default
created: 1784182205244
updated: 1784182205244
---

## Resolved mutable: glitch-microrepeat-leaves-playpos-offset

microRepeat is a SEPARATE post-mix DSP block (patches/microRepeat.h, applied loopMachine.cpp:824 on the whole mix via lp.microRepeatDiv), not a per-clip read; it never writes m_playPos. Mechanism: it freezes/repeats OUTPUT while masterPhase advances; on release the phase target moved but free-running m_playPos didn't -> offset nothing chases back. Same root cause as varispeed-playpos-not-phase-chased.
