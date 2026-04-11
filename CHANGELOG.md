
## [Unreleased]

### Fixed
- play_block phase formula: replace `(delta + 2*numBlocks) % numBlocks` with canonical positive-modulo `((delta % numBlocks) + numBlocks) % numBlocks` in both _startPlaying (loopClip.cpp) and hard-lock (loopClipUpdate.cpp). Correct for all clip lengths relative to phrase.
## 2026-04-11
- fix: replace brute-force _calcQuantizeTarget loops with clean iteration over fixed candidate set {M/8,M/4,M/2,M,2M,4M,8M}, picking nearest by absolute distance; skip candidates < CROSSFADE_BLOCKS*2


- fix: replace CLogger::Get()->Write() in loopClip _startRecording, _startEndingRecording, _startPlaying with LOOPER_LOG(); remove #include <circle/logger.h> from loopClip.cpp and loopClipState.cpp
- fix: replace direct CLogger::Get()->Write() calls in loopMachine::update() and updateState() with LOOPER_LOG() to prevent syslog UDP blocking audio/MIDI threads; drain all queued log messages per frame in uiWindow::updateFrame()

## [unreleased] 2026-04-09
- feat: Ableton Link phrase = 4 bars; masterLoopBlocks = INTEGRAL_BLOCKS_PER_SECOND * 60 * 16 / bpm, rounded to multiple of 8
- fix: recordStartPhaseOffset = masterPhase (removed +CB+1 overcorrection); play_block=0 at all phrase boundaries
- fix: crossfade_start = numBlocks in hard-lock path (tail region, not clip start)
- fix: at_phrase_start / at_loop_point use masterPhase % masterLoopBlocks (monotonic phase)
- test: comprehensive 37-assertion simulation suite; 18+37=55 tests all PASS across 6 BPMs, sub-phrase clips, drift, 2-track, BPM change
- docs: CLAUDE.md updated — INTEGRAL_BLOCKS_PER_SECOND=690, phase alignment formula, 4-bar Link quantize

## [unreleased] 2026-04-08
- fix: Ethernet restored as boot/syslog interface (static 192.168.137.x); WiFi used only for Ableton Link via CBcm4343Device raw frames
- fix: abletonLink rewritten to use SendFrame/ReceiveFrame (no CSocket, no CNetSubSystem dependency)
- fix: WLAN init/join non-fatal; Ethernet always available regardless of WiFi state
- fix: tftp-server.js copies firmware/ from release zip to tftproot/firmware/ for SD card placement
- feat: WiFi via BCM43455 (ticker open network), syslog to 192.168.137.1
- feat: Ableton Link multicast peer (224.76.78.75:20808), BPM sync via tmln TLV
- infra: CI builds circle/addon/wlan, downloads rPi4 WiFi firmware into release zip

## fc7727c — 2026-04-08
- feat: clear button on empty track now acts as record button (sets phrase length on first track)

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
