## 2026-04-17b — USB audio sync: fractional Q16 read + linear interp (eliminate drift-correction crackle/smear)

- feat: patches/input_usb.cpp — replace integer skip/repeat drift correction with Q16 fractional read position + 2-sample linear interpolation. Wider deadband (target=256, DB=128) + 16k rate gain means corrections spread smoothly across many blocks instead of producing 1-sample discontinuities. Rate clamped to ±256/16384 (~1.5%) to bound the worst case.
- feat: patches/output_usb.cpp — same fractional/interp scheme on OTG tap (target=768, DB=192). Eliminates OTG-side micro-clicks on tonal content.
- feat: audio.cpp — replace skip/repeat counters with rate-step deviation in PPM for smooth observability of drift tracking.
- test: test.js — rewritten to 9 cases covering interp correctness (midpoint=150, zero-frac=lower), bit-exact steady-state output, ramp monotonicity, avail convergence under drift, and 5000-iter stability at ±0.1% producer drift.

## 2026-04-17 — USB audio sync stability: underrun-repeat, drift deadband, watchdog, OTG OUT parity fix

- fix: patches/input_usb.cpp — repeat-last-sample on underrun (no more zero clicks), deadband skip/repeat drift correction (target=192, DB=64), catastrophic resync, IN_RING_SIZE 256→512, g_inLastTicks timestamp for watchdog, underrun/resync/skip/repeat counters
- fix: patches/output_usb.cpp — repeat-last-sample on outHandler underrun, recenter on catastrophic avail, expose out-avail and underrun counter
- fix: patches/dwusbgadgetendpoint.cpp — OTG OUT frame parity: replace DSTS SOFFN race with software m_bIsoOddFrame toggle, mirroring the prior IN fix
- feat: audio.cpp — USB IN watchdog in loop() forces startUpdate() after >5ms idle (bridges USB stall → faster recovery from grain events); 1 Hz ring-stats log when anomalies observed
- test: test.js — 8-case ring simulation: steady-state no-correction, underrun-repeat, overrun-resync, drift skip/repeat, 1000-iter matched + ±0.1% drift stability



- feat: audio.cpp — always allocate pLivePitchWrapper; wire input→wrapper→looper unconditionally; remove LOOPER_LIVE_PITCH guards
- feat: apcKey25.h — add m_liveEngaged, m_livePitchSemitones fields; _applyLivePitch() helper; expose both in DebugState
- feat: apcKey25.cpp — CC1 mod wheel: deadzone 59-69 disengages (pitch=1.0), outside sets semitones ±6 from center; CC52: maps 0-127 to ±6 semitones on live input; ch1 note-on toggles m_liveEngaged + sets pitch from distance-to-C60; ch2 note-on sets pitch = pow(2, (note-60)/12) and engages
- feat: apcKey25Transpose.cpp — _applyLivePitch() calls pLivePitchWrapper->setPitchScale(pow(2, semitones/12)) or 1.0 when disengaged

## 2026-04-13 — Fix CI build: add .PHONY cstdint target for RubberBandWrapper dependency

- fix: Makefile — add `.PHONY: cstdint` target to suppress "No rule to make target cstdint" error when Circle's build system includes system headers in .d dependency files; cstdint is a standard header included by patches/RubberBandWrapper.h

## 2026-04-13 — Add transpose state observability: DebugState struct + getDebugState()

- feat: apcKey25.h — add DebugState struct (transposeLocked, transposePitch, pitchWheelOffset, driftTarget, computedRatio) and getDebugState() method declaration to expose transpose lock state for troubleshooting
- feat: apcKey25Transpose.cpp — implement getDebugState() to return current transpose state as immutable struct; enables live monitoring of lock/unlock, pitch changes, drift behavior without performance impact

## 2026-04-13 — Fix build errors: kernel.cpp SetLogLevel, Makefile include path, add RubberBandWrapper header

- fix: patches/kernel.cpp — remove line 83 `m_Logger.SetLogLevel(LogPanic, &m_Screen)` which does not exist in Circle's CLogger class; log level is already set in constructor to LogDebug
- fix: Makefile — add `-I .` to INCLUDE path so compiler can find `patches/RubberBandWrapper.h` when included from audio.cpp
- feat: patches/RubberBandWrapper.h — add header for tempo/pitch stretching wrapper around RubberBand::RubberBandStretcher; required for integrate-clip-stretch feature

## 2026-04-12 — Fix multicore: call CMultiCoreSupport::Initialize() to start secondary cores

- fix: patches/kernel.cpp — add `m_CoreTask.Initialize()` call in `CKernel::Initialize()` after timer init, under ARM_ALLOW_MULTI_CORE guard; without this call, secondary cores never start, IPIHandler never registers, and SendIPI hangs core 0
- fix: tftp-server.js — add serial-subfolder fallback in handleRRQ: if `tftproot/<serial>/<file>` not found, try `tftproot/<file>` (standard Pi netboot behavior)

## 2026-04-12 — Fix CORE_FOR_AUDIO_SYSTEM=0: include sysconfig.h in std_kernel_stub.h

- fix: patches/std_kernel_stub.h — add `#include <circle/sysconfig.h>` before ARM_ALLOW_MULTI_CORE guard; sysconfig.h has ARM_ALLOW_MULTI_CORE defined (uncommented by sed patch in build.yml), so CORE_FOR_AUDIO_SYSTEM=1 now activates correctly when building AudioSystem.cpp; previously CORE_FOR_AUDIO_SYSTEM remained 0 because ARM_ALLOW_MULTI_CORE was only a make variable not a preprocessor define in the circle-prh build context
- refactor: build.yml — remove temporary "Inspect circle multicore support" and "Check multicore symbols" diagnostic steps

## 2026-04-12 — Audio on dedicated core 1 via ARM_ALLOW_MULTI_CORE

- feat: patches/multicore.cpp — CCoreTask extends CMultiCoreSupport; Run() halts non-audio cores; IPIHandler() on core 1 calls AudioSystem::doUpdate() on IPI_AUDIO_UPDATE=11
- feat: patches/kernel.h — CCoreTask class declared under ARM_ALLOW_MULTI_CORE guard; CKernel gains m_CoreTask member; includes circle/multicore.h
- feat: patches/kernel.cpp — m_CoreTask(this) added to CKernel constructor initializer list under ARM_ALLOW_MULTI_CORE guard
- feat: patches/std_kernel_stub.h — ARM_ALLOW_MULTI_CORE sets CORE_FOR_AUDIO_SYSTEM=1, IPI_AUDIO_UPDATE=11, and forward-declares CCoreTask::Get()/SendIPI() for AudioSystem.cpp linkage
- feat: Makefile — ARM_ALLOW_MULTI_CORE define added; multicore.o added to OBJS
- feat: build.yml — ARM_ALLOW_MULTI_CORE=1 added to all make commands; multicore.cpp copied in patches step
- refactor: loopMachine.cpp — removed update#N diagnostic counter (was causing LOOPER_LOG→syslog UDP on audio thread)
- refactor: loopClipUpdate.cpp — removed record_block diagnostic log

## 2026-04-12 — Fix audio update loop dying after 5 iterations

- fix: audio.cpp removed duplicate input.start()/output.start() calls after AudioSystem::initialize(); initialize() already calls start() on all AudioStream objects in the graph; the double-call set s_update_responsibility=false (takeUpdateResponsibility returned false on second call), causing inHandler to never call startUpdate() after the initial 5 completions

## 2026-04-12 — OTG USB gadget enumeration fix

- fix: dwusbgadgetendpoint.cpp HandleOutInterrupt SETUP_DONE — skip FinishTransfer; after SETUP_DONE DWC re-arms EP0 OUT XFER_SIZ register for 3 back-to-back SETUP slots (3×40=120), making FinishTransfer see remaining=120 > programmed=8 and return 0; SETUP data is always exactly sizeof(TSetupData), just call InitTransfer+OnControlMessage directly
- fix: dwusbgadgetendpoint.cpp OnUSBReset — remove assert(!(ACTIVE_EP)) for non-EP0 endpoints; on multi-RESET boot sequence each RESET calls OnUSBReset which sets ACTIVE_EP, causing subsequent RESET to assert; iso EP must tolerate being re-initialized while active

## 2026-04-12 — OTG USB gadget enumeration fix (continued)

- fix: dwusbgadget.cpp — pulse soft-disconnect from UpdatePlugAndPlay main loop when SUSPEND fires at StateEnumDone; sets s_bNeedReconnect flag (max 3 attempts) to force Windows re-enumerate after boot SUSPEND absorption
- debug: dwusbgadgetendpoint.cpp FinishTransfer assert replaced with LOGWARN+continue to diagnose nXferSize>nTransferLength without halting kernel
- fix: usbaudiogadgetendpoint.h — remove empty OnUSBReset override; base CDWUSBGadgetEndpoint::OnUSBReset now called on USB reset, clearing stale transfer state via InitTransfer()
- fix: dwusbgadget.cpp Initialize() pulses soft-disconnect for 100ms after InitCore() to force Windows host re-enumeration on Pi boot; without this Windows does not retry after a prior failed enumeration
- fix: dwusbgadget.cpp HandleUSBSuspend also ignores SUSPEND when state==StateEnumDone; debug trace showed SUSPEND fires at StateEnumDone (=3) not StateResetDone — Windows sends SUSPEND between ENUM_DONE and EP0 GET_DESCRIPTOR, killing EP0 before it can respond
- fix: dwusbgadget.cpp patched — HandleUSBSuspend now ignores SUSPEND when state==StateResetDone; prevents UpdatePlugAndPlay from destroying active enumeration window; ENUM_DONE fires normally → EP0 OnActivate() → EP0 armed; root cause: SUSPEND fired between RESET and ENUM_DONE during Windows USB enumeration, causing PnPEventSuspend to kill EP0 before it was ever armed
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
