# CHANGELOG

## 2026-04-08

### refactor: replace CLIP_STATE bitmask with ClipState enum

- Defined 9-value mutually-exclusive `ClipState` enum in `Looper.h`, replacing 7 `CLIP_STATE_*` bitmask defines
- `CS_RECORDING_TAIL`/`CS_FINISHING` encode former `m_pendingPlay` boolean into state
- `CS_LOOPING` encodes former `PLAY_MAIN|PLAY_END` combination
- Removed `m_pendingPlay`, `m_pendingUnmute`, `tryUnmute()` from `publicClip`/`loopClip`
- Split `loopClip.cpp` into `loopClip.cpp` + `loopClipUpdate.cpp` + `loopClipState.cpp`
- Updated `loopMachine.cpp`, `loopTrack.cpp`, `uiClip.cpp`, `uiClip.h` to use enum
- Added `loopClipUpdate.o` and `loopClipState.o` to Makefile

### fix: phase-align playback start

- `_startPlaying()` uses `(masterPhase + 1) % numBlocks` to match `update()` tick order
- Eliminates one-beat phase offset when starting a second recording on an offbeat

### fix: keep recording to phrase end on stop

- Recording continues to phrase end (crossfade tail) before stopping
- Playback begins after crossfade tail completes
