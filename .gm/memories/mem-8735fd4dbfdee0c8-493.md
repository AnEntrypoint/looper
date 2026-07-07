---
key: mem-8735fd4dbfdee0c8-493
ns: default
created: 1780075125450
updated: 1780075125450
---

## Resolved mutable: offbeat-off-by-one-cause

loopMachine.cpp:799 at_phrase_start=(m_masterPhase % m_masterLoopBlocks==0); line 801-802 pending RECORD track_latch=at_phrase_start. Latch fires ONLY at full 16-beat phrase downbeat, never at intermediate beats, so a press near beat N waits to the next phrase wrap and starts on phrase-0 downbeat = the 'one forward'/offbeat. Root = latch gate; _startPlaying (loopClip.cpp) play_block is grid-correct given grid-aligned m_recordStartPhaseOffset.
