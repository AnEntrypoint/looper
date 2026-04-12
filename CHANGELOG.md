## 2026-04-12 — OTG USB gadget enumeration fix

- fix: dwusbgadget.cpp patched — set DWHCI_DEV_CTRL_SOFT_DISCONNECT after InitCore() (not before — Reset() wipes all registers including DEV_CTRL), hold through EP creation and interrupt enable, clear after; forces host-visible USB disconnect/reconnect so Windows sends fresh USB RESET → ENUM_DONE → EP0 OnActivate() → EP0 armed
- fix: s_DeviceDescriptor changed from static const to static (non-const) so VID/PID can be written at runtime without const_cast; const_cast into .rodata was silently ignored by MMU on rPi4, causing vid=0000/pid=0000 in device descriptor
- debug: GetDescriptor logs device descriptor fields (len/cls/vid/pid) and config descriptor size

## 2026-04-11 — OTG+USB combined audio mode

- Fixed usbaudiogadgetendpoint: IN path now calls TAudioInHandler to fill samples before transmitting to host; OUT path unpacks DMA buffer and delivers to TAudioOutHandler. Previously both paths were inverted (IN sent silence always, OUT called handler with uninitialized data).
- Corrected TAudioInHandler typedef to non-const (handler writes samples) and TAudioOutHandler to const (handler receives samples). Matching fixes in usbaudiogadget.h, output_otg.h, input_otg.h.
- output_otg.cpp::tapHandler now fills provided pLeft/pRight buffers directly via AudioOutputUSB_tapOTG, completing the Pi→host audio tap path.
- input_otg.cpp::injectHandler now receives const data and injects into AudioInputUSB ring via AudioInputUSB_injectOTG.
- audio.cpp: removed #error mutual-exclusion guard. LOOPER_USB_AUDIO + LOOPER_OTG_AUDIO can now be defined simultaneously. USB audio drives the AudioSystem chain; OTG gadget runs as side-channel tap/inject via otgIn.start()/otgOut.start().
- build.yml: audio and looper build steps now use LOOPER_USB_AUDIO=1 LOOPER_OTG_AUDIO=1 for combined mode.
- Removed unused InRing fields from CUSBAudioGadgetEndpoint.
- fix: dwusbgadgetendpoint.cpp include changed to quoted "dwusbgadgetendpoint.h" so patched header (TypeIsochronous) is resolved instead of original Circle header.
- fix: synced dwusbgadgetendpoint.h to match .cpp: FinishTransfer(void), OnControlMessage virtual, HandleUSBReset non-static, plain u32 m_DummyBuffer (no DMA_BUFFER macro).
- fix: AudioInputUSB::start() and AudioOutputUSB::start() moved to public section so setup() can call them.
- fix: added usbaudiogadget.o and usbaudiogadgetendpoint.o to audio_Makefile OBJS so CUSBAudioGadget links.
- fix: LOOPER_OTG_AUDIO define added to Looper Makefile.
- fix: ACHeader array size corrected to 10 bytes (UAC1 bInCollection=2 adds 2 baInterfaceNr bytes).

## 2026-04-11

### Added
- feat: WiFi AP hotspot fallback — if JoinOpenNet('ticker') fails, Pi starts 'ticker' AP via CreateOpenNet(); wlanDHCPServer.cpp serves DHCP pool 192.168.4.2-9 to connecting clients; Pi uses static IP 192.168.4.1; Ableton Link multicast works over AP interface; heartbeat log shows 'ap' mode
- test: expand simulation coverage — add 30 new scenarios: sub-phrase (M/2), multi-phrase (2M), stop-quantize (willPlay=false→CS_RECORDED), deferred quantize (auto-fires at record_block>=target), per-track latch (recording starts exactly at phrase boundary); add 2 new source integrity checks; total 48 scenarios, all pass

### Refactored
- Remove dead code from loopMachine.cpp: duplicate LOOPER_LOG latch event, dead #if 0 block in LogUpdate, WITH_INT_VOLUMES=0 dead branches and applyGain function; strip all comments from loopMachine.cpp and loopTrack.cpp

### Fixed
- play_block phase formula: replace `(delta + 2*numBlocks) % numBlocks` with canonical positive-modulo `((delta % numBlocks) + numBlocks) % numBlocks` in both _startPlaying (loopClip.cpp) and hard-lock (loopClipUpdate.cpp). Correct for all clip lengths relative to phrase.
- fix: replace brute-force _calcQuantizeTarget loops with clean iteration over fixed candidate set {M/8,M/4,M/2,M,2M,4M,8M}, picking nearest by absolute distance; skip candidates < CROSSFADE_BLOCKS*2
- fix: replace CLogger::Get()->Write() in loopClip _startRecording, _startEndingRecording, _startPlaying with LOOPER_LOG(); remove #include <circle/logger.h> from loopClip.cpp and loopClipState.cpp
- fix: replace direct CLogger::Get()->Write() calls in loopMachine::update() and updateState() with LOOPER_LOG() to prevent syslog UDP blocking audio/MIDI threads; drain all queued log messages per frame in uiWindow::updateFrame()

## 2026-04-09
- feat: Ableton Link phrase = 4 bars; masterLoopBlocks = INTEGRAL_BLOCKS_PER_SECOND * 60 * 16 / bpm, rounded to multiple of 8
- fix: recordStartPhaseOffset = masterPhase (removed +CB+1 overcorrection); play_block=0 at all phrase boundaries
- fix: crossfade_start = numBlocks in hard-lock path (tail region, not clip start)
- fix: at_phrase_start / at_loop_point use masterPhase % masterLoopBlocks (monotonic phase)
- test: comprehensive 37-assertion simulation suite; 18+37=55 tests all PASS across 6 BPMs, sub-phrase clips, drift, 2-track, BPM change
- docs: CLAUDE.md updated — INTEGRAL_BLOCKS_PER_SECOND=690, phase alignment formula, 4-bar Link quantize

## 2026-04-08
- fix: Ethernet restored as boot/syslog interface (static 192.168.137.x); WiFi used only for Ableton Link via CBcm4343Device raw frames
- fix: abletonLink rewritten to use SendFrame/ReceiveFrame (no CSocket, no CNetSubSystem dependency)
- fix: WLAN init/join non-fatal; Ethernet always available regardless of WiFi state
- fix: tftp-server.js copies firmware/ from release zip to tftproot/firmware/ for SD card placement
- feat: WiFi via BCM43455 (ticker open network), syslog to 192.168.137.1
- feat: Ableton Link multicast peer (224.76.78.75:20808), BPM sync via tmln TLV
- infra: CI builds circle/addon/wlan, downloads rPi4 WiFi firmware into release zip
- feat: clear button on empty track now acts as record button (sets phrase length on first track)

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
