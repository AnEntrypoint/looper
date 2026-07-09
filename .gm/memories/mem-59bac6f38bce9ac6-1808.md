---
key: mem-59bac6f38bce9ac6-1808
ns: default
created: 1783631219475
updated: 1783631219475
---

## Resolved mutable: nominal-rate-rounding-error-at-44100

DECISIVE: codesearch hit patches/AudioSystem.cpp:485-541 (AudioSystem::doUpdate) contains this EXACT comment describing the failure mode being reported: 'A bare threshold hunts: drain->over->stop->refill->drain, OSCILLATING EVERY ~1S with a small resync each toggle. Instead: only ENTER drain mode when avail climbs above DRAIN_HIGH..., then drain all the way down to DRAIN_LOW... The wide [DRAIN_LOW, DRAIN_HIGH] deadband lets avail sweep slowly between ~128 and ~288 -- never near the 64 underrun or 384 overfill resync edges, and no fast toggling.' This hysteresis mechanism was explicitly engineered to prevent a ~1s oscillate-and-resync cycle -- the EXACT symptom now reported (periodic ~1-2s crunch+blips). CONCLUSION: my nominal-rate pktSize fix introduces a systematic per-completion bias in m_nInSubmitBytes (confirmed plausible via the 44100-vs-48000 integer-truncation math: claiming 6 samples/microframe when the true rate at 44100kHz is 5.5125, an ~8.8% constant over-claim) that is large enough to push avail's sweep rate past what the [DRAIN_LOW=128ish, DRAIN_HIGH=288ish] deadband margin (roughly 160 samples of headroom) can absorb before hitting a resync edge (64 underrun / 384 overfill) -- reproducing exactly the oscillation this deadband was built to prevent. FIX DIRECTION: eliminate the SYSTEMATIC bias in m_nInSubmitBytes entirely (not just bound its magnitude) by using fractional sample accumulation across calls (mirroring m_fbAccum's Q16.16 carry on the OUT side) instead of a single per-call truncated integer estimate -- this way the LONG-RUN average claimed matches the true rate exactly (zero steady-state bias), with only sub-sample rounding noise per call, well within what the existing deadband already tolerates.
