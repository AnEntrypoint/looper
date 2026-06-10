# AGENTS.md

Project notes for agents working on Lanmower's Looper. Supersedes CLAUDE.md — CLAUDE.md is now just a pointer here.

## Timing: latency-backdated record + continuous buffer + Link train-on-first-loop (commit 1cbd670)

- **`continuousBuffer.{h,cpp}` is the single staging area.** A 180s always-on stereo rolling ring (`g_cbBuffer`, ~16MB, `CB_RING_BLOCKS = 180 * INTEGRAL_BLOCKS_PER_SECOND`). `cbWriteBlock(m_input_buffer)` is called EVERY audio block from `loopMachine::update` **AFTER the live-pitch + effects stages** (was: before) so loops record the WET signal the musician hears — pitch shift and filter/sends baked in. Monotonic `g_cbWriteBlock`. All loopers copy their clip out of this; nothing records the live input directly anymore. **Backdate compensates the extra processing latency dynamically:** `g_cbExtraLagSamples` is published by `loopMachine::update` right before the write = the live-pitch engine's read-offset (`RubberBandWrapper::latencySamples()`, ~192 @ 48k, CC100-tunable) when transpose is engaged, **0 when transpose is off** (engine bypassed, zero added latency). `cbBackdatedBlock` adds it to `CB_FIXED_LAG_SAMPLES`. Effects (delay/reverb) are additive tails, not a timing shift, so they need no lag compensation. Surfaced as `extraLag=<samp>` in the `:4445 TIME` verb.
- **Press is timestamped at the MIDI ISR.** `apcKey25::_queueCmd` (runs inside `handleMidi`) stamps `press_ticks = CTimer::GetClockTicks()` into the SPSC cmd-ring slot. The Core-2 drain publishes it to `g_pendingPressTicks` immediately before `pTheLooper->command()` (single-threaded, no race). This is the anchor for ALL backdating — action latency is compensated regardless of the Core-2 poll cadence.
- **`cbBackdatedBlock(press_ticks)`** = `g_cbWriteBlock - backBlocks`, where `backBlocks = (CB_FIXED_LAG_SAMPLES=96 + wrap-safe (now-press_ticks)us→samples) / AUDIO_BLOCK_SAMPLES`, clamped to `cbBlocksAvailable()-1` (sets `g_cbLastBackdateClamped`). `press_ticks==0` ⇒ fixed-lag only.
- **Clip records by copy-from-rolling:** `loopClipUpdate.cpp` `memcpy`s `cbBlockPtr(m_recStartBlock + m_record_block)` into the clip block (was `*rp++=*ip++`). `m_recStartBlock = cbBackdatedBlock(...)` set in `_startRecording`, so clip block 0 == the press instant. `m_recordStartPhaseOffset` is backdated (`masterPhase - backBlocks`) so phrase alignment is sample-true to the press. Stop length = `_backdatedRecordLength()` (backdated-stop − backdated-start) when master==0, so loop length == press-to-press interval, latency-independent.
- **Ableton Link train-on-first-loop** (`abletonLink.cpp`): `linkStart(originTicks)` on first rec-on (clear bank, not synced) anchors the timeline origin at the backdated press = beat 0; `sendAlive` tmln TLV now broadcasts `beatOrigin`/`timeOrigin` (were hardcoded 0). `linkEnd(clip_seconds)` on first-loop finalize calls `linkDeriveQuant` → nearest-120 musical beat-count {0.25..16} within [80,160] → sets `s_bpm`+`s_quantBeats`, marks ended, broadcasts. Quant SOURCE = first loop when clear; Link grid only when synced to peers (`loopMachine` link-quantum block gated on `lp.linkSynced`); first-record stays immediate (no grid wait).
- **Consecutive-record beat-grid snap** (`loopMachine.cpp` pending-latch + `loopClip.cpp::_startRecording`): when a master grid exists (`m_masterLoopBlocks>0`), a deferred record latches on the nearest BEAT grid point, NOT the full-phrase downbeat. `gridStep = (M>=16)? M/16 : M` (M encodes 16 beats per the link-quantum calc). Latch gate `at_beat_grid = (m_masterPhase % gridStep == 0)` (was `% m_masterLoopBlocks`) so a press near a beat starts within ≤1 beat, not ≤1 phrase (the prior "offbeat / one-forward" bug). `_startRecording` SNAPS `m_recordStartPhaseOffset` to the nearest gridStep multiple (floor, +gridStep if `rem*2>=gridStep`) so every consecutive recording slices on a multiple of the quant — offbeats are not multiples. First loop (`M==0`) stays immediate + sample-true (it defines the grid). Witnessed by `scripts/test-grid-snap.cpp` (`ALL PASS`) and `gridStep=`/`latchPhase=` in the `:4445 TIME` verb.
- **Hold = stop+clear, LED always off** (`apcKey25.cpp` hold poll → `loopMachine.cpp` ERASE_TRACK): a long-hold (≥`APC_HOLD_ERASE_MS`) sends `LOOP_COMMAND_ERASE_TRACK_BASE+n`, NOT `CLEAR_LAYER`. `ERASE_TRACK` unconditionally clears the track (per-layer `clearClip`, running-count-correct), sets `m_track_pending[track]=NONE` (cancels any queued deferred-record), drops selection, and NEVER re-arms — so the pad always returns to EMPTY (LED off). `CLEAR_LAYER` (tap-arm) still re-arms a pending RECORD on an empty track when a grid exists; the two intents are now distinct command ids (the shared-id ambiguity left a held pad yellow before).
- **Live timing telemetry — `:4445` `TIME` verb** (kernel_run.cpp): `backdate=<samp> latUs=<us> clamped=<0/1> extraLag=<samp> gridStep=<blocks> latchPhase=<block> cbwr=<block> started=<0/1> ended=<0/1> qbeats100=<beats*100> bpm=<n>`. Capture-free witness, same pattern as the `WLAN`/`GET` verbs.
- **Host tests (no Pi):** `scripts/test-backdate.cpp` (wrap-safe math, clamp, start+stop length invariance) and `scripts/test-link-quant.cpp` (nearest-120 subdivision) both `ALL PASS` via `g++ -O2 -std=c++17`.
- **HARDWARE VALIDATION RUNBOOK (deferred — do when the Pi is reachable):** netboot the staged kernel (1059276B+; in `tftproot/7bec0617/`) or reflash the SD `kernel7l.img`. Then: (1) play a metronomic/transient source into the UCA222; (2) tap a looper pad rec-on on a downbeat, rec-off a known number of beats later; (3) `TIME` on :4445 — expect `backdate` ≈ the real press→process latency in samples (non-zero, `clamped=0`), `started=1` then `ended=1`, `qbeats100`/`bpm` matching the played tempo (~120-ish); (4) `GET`/listen — the recorded loop must START on the transient (not late) and its LENGTH == the tapped interval; (5) stop→replay and let it loop several times — first-play, replay, and every restart must be phase-identical with no jank at the seam. The host tests already prove the math; this confirms it on real input.


## Non-obvious build caveats

- `circle-prh/audio/bcm_pcm.cpp` includes `"BCM_PCM.h"` (uppercase) but the file is `bcm_pcm.h` (lowercase). Linux CI requires a symlink: `ln -sf bcm_pcm.h circle/_prh/audio/BCM_PCM.h` before building.
- `circle-prh` miniuart uses `ARM_IRQ_AUX` and `ARM_GPIO_GPPUD` which are absent in phorton1/circle for RASPPI=4 (circle uses GIC-400 for rPi4). Fixed via `patches/miniuart.cpp` with `#if RASPPI < 4` guards.
- `phorton1/circle` does not support `AARCH=64` — `alloc.cpp` casts pointers to `u32`. Use `AARCH=32` for RASPPI=4; produces `kernel7l.img`.
- Default build (no defines) uses CS42448 octo audio path. `LOOPER_USB_AUDIO=1` requires `AudioInputUSB`/`AudioOutputUSB` patches.
- On Linux CI, include paths use forward slashes: `<audio/Audio.h>` not `<audio\\Audio.h>`.
- CI produces `kernel7l.img` for RASPPI=4 AARCH=32. Naming per circle Rules.mk: RASPPI=2→kernel7, RASPPI=3 32-bit→kernel8-32, RASPPI=4 32-bit→kernel7l.
- `CUSBCDCGadget` exposes no `GetSerial()` — access via `CDeviceNameService::Get()->GetDevice("utty1", FALSE)` after `Initialize()` returns. Device is `nullptr` until USB host enumerates.
- `CUSBCDCGadget` lives in `lib/usb/gadget/` as a separate library (`libusbgadget.a`). Build `make -C circle/lib/usb/gadget` and link `libusbgadget.a` before `libusb.a`.
- On rPi4, `CUSBHCIDevice` is `CXHCIDevice` (xHCI, USB-A). `CUSBCDCGadget` uses `CDWUSBGadget` (DWC OTG, USB-C). Separate controllers — can coexist.
- APC Key 25 USB audio input only supports 48000Hz. With `LOOPER_USB_AUDIO=1` alone, `InCompletion` receives zero-length URBs and the audio chain stalls. Fix: `loop()` drives `AudioSystem::startUpdate()` via timer at block rate when UCA222 IN absent.
- `CUSBAudioDevice::Configure()` seeds both In/Out requests. `OutCompletion` must always call `StartOutRequest()`; send silence when no handler registered (`patches/usbaudiodevice.cpp`).
- `tftp-server.js` is the single all-in-one dev-host process: TFTP (69), DHCP (67), syslog (514), GitHub release auto-update. Run `node dev-server.js` as admin.
- **Local Windows build (CI fallback)**: `scripts\build-local.ps1` mirrors `.github/workflows/build.yml` end-to-end on Windows. Requires `scoop install gcc-arm-none-eabi`, `python`, `git`. Robocopies the repo into `$env:USERPROFILE\.looper-build\circle\_prh\_apps\Looper`, clones rsta2/circle + phorton1/circle-prh, applies all patches, builds libs once (cached), produces `dist\looper-sd.zip`. First build ~10-30min, incremental rebuilds seconds. Use this for the measure-tune-measure loop instead of waiting on GitHub Actions. Falls back to CI if local fails.
  - **CRITICAL build gotcha — `CHECK_DEPS=0` disables header-dependency tracking.** The firmware make (and `build-local.ps1`) builds with `CHECK_DEPS=0`, so editing a header (`soladSnacOctaver.h`, `grainFormant.h`, `RubberBandWrapper.h`, etc.) does **NOT** recompile the `.o` files that `#include` it — make only rebuilds a `.o` when its own `.cpp` is newer. After ANY header edit you MUST `rm` the dependent objects (e.g. `loopMachine.o audio.o`, or just all `*.o`) before `make`, otherwise you deploy a kernel with **stale engine code while the source looks correct**. This caused a long stretch of "host tests pass but the Pi behaves differently" — the tell was live telemetry showing a `.cpp`-reached field updating (`fdepth`) while a sibling field set inline in the same header function (`gTgt`/`gMix`) did not. Note: after `rm *.o` you must also regenerate `wlan_firmware.o` (`arm-none-eabi-as` from `$TEMP/wlan_firmware.S`) — `rm` deletes it and there is no make rule to rebuild it. A clean-rebuild kernel-size change confirms the engine objects actually recompiled.
- **Host-side loopback measurement**: `scripts\measure-latency.ps1` plays test signals through host default audio out (wired into UCA222 IN via Focusrite) and captures from default audio in (UCA222 OUT). Modes: `impulse` (round-trip latency via onset cross-correlation), `chirp` (frequency sweep), `sine -Freq <Hz>` (THD via Goertzel at fundamental + harmonics 2-5), `silence` (noise floor), `full` (all of the above + baseline 1kHz + low-E 82.4Hz + low-E -12 41.2Hz sustained). Outputs `scripts\measure-results\<timestamp>\report.json` plus capture WAVs. **Run this against a baseline firmware build, save the JSON, then run again after any DSP/buffer change — diff to confirm "lowest achievable latency" empirically rather than asserting it.**

## Audio architecture

- **USB audio runs at 48000Hz** (UCA222 native). `AUDIO_SAMPLE_RATE=44100` in AudioTypes.h is the internal system rate.
- **AUDIO_BLOCK_SAMPLES=64** for low latency (~1.3ms at 48kHz).
- **Ring buffers decouple USB from audio chain.** USB IN writes to a 512-sample SPSC ring (`patches/input_usb.cpp`). USB OUT to a 2048-sample SPSC ring (`patches/output_usb.cpp`).
- **Drift correction: Q16 fractional read + linear interpolation.** Both IN and OTG-tap read positions are fractional (u32 int + u16 frac). Rate step = `FRAC_ONE + (band_dev * FRAC_ONE) / RATE_GAIN`. IN target=96, DB=48 (`patches/input_usb.cpp:17-18`) — derived from physics: UCA222 IN delivers 48 samples per 1ms USB SOF, so target = 1 packet (48) + 1-packet safety margin (48) = 96; DB = ±1 packet protects against single-SOF jitter without triggering rate adjust. OTG target=768, DB=192 — derived: OTG isochronous DMA needs deeper margin to absorb DWC2 frame-parity slips. Inaudible on tonal content. Catastrophic-deviation clause resets read position. Rate clamped to ±256/16384 (≈1.5%).
- **Underrun fallback repeats last sample**, not zero. Eliminates clicks on brief starvation.
- **Watchdog**: if USB IN hasn't delivered in >5ms, `loop()` (Core 2 control plane) force-fires `AudioSystem::startUpdate()` which enqueues a `DISPATCH_AUDIO` job onto Core 1.
- **`startUpdate()` is driven by USB IN ring position** (push from Core 0 ISR), not a timer. Under `ARM_ALLOW_MULTI_CORE` it pushes onto `coreDispatch` + SEV; the legacy inline `doUpdate()` branch only runs in single-core builds.
- **4-core partition (rPi4, ARM_ALLOW_MULTI_CORE=1)** — Core 0: hard-RT dispatch (USB IN/OUT completion ISRs via GIC, `AudioSystem::startUpdate` push, reboot socket poll, WFE). Core 1: DSP worker (WFE-blocked drain of `coreDispatch` audio jobs → `AudioSystem::doUpdate` → entire audio graph including loopMachine, RubberBand feed/retrieve, signalsmith/granular octaver, apcEffectsProcessor; `pLivePitchWrapper->setPitchScale` runs here so signalsmith stays single-writer). Core 2: control plane (USB plug-and-play poll, Net.Process, usbMidiProcess, `audio.cpp::loop` telemetry+watchdog, `pTheAPC->update`, linkProcess, WiFi DHCP, Scheduler.Yield). Core 3: reserved idle (WFE forever, claimable later).
- **IPC primitives** — `patches/coreDispatch.{h,cpp}`: 64-slot SPSC ring + DSB+SEV/WFE wakeup, ISR-safe push, no mutexes in audio path; drops counted in `g_dispatchDropped` and surfaced as `TELEM_DISPATCH_FULL`. `patches/paramSnapshot.{h,cpp}`: double-buffered atomic-swap publish for control→DSP shared params (`liveEngaged`, `livePitchSemitones`, `formantNorm`, `linkSynced`, `linkBPM`, `masterLoopBlocks`); single writer (Core 2), multi-reader, never torn.
- **Per-core busy/idle accounting** — `patches/coreBusy.{h,cpp}`: `g_coreBusyTicks[4]` / `g_coreIdleTicks[4]` updated cooperatively by each core's loop (only own slot written, lock-free). `audio.cpp` 2Hz stat block emits `cores c1=NN%% c2=active c3=NN%%` so the per-core partition can be verified live: Core 3 should read ~0%, Core 1 should idle low between bursts. Confirms (a) Core 1 not saturated, (b) Core 3 truly idle (free for future hand-off — pitch worker candidate), (c) DSP work isn't leaking to Core 2.
- **Audio readiness handshake**: `g_coreAudioReady` + WFE/SEV. Cores 1 and 2 spin in WFE during Core 0's hardware init, released after `setup()` returns from `CKernel::Run`.
- **OTG gadget handler direction**: `TAudioInHandler (s16 *pL, s16 *pR, unsigned nSamples)` fills arrays (Pi→host USB IN). `TAudioOutHandler (const s16 *pL, const s16 *pR, ...)` receives read-only (host→Pi USB OUT). DMA buffer: interleaved s16 LE, 192 bytes = 48 samples at 48kHz stereo.
- **OTG is side-channel** alongside UCA222 when both USB_AUDIO and OTG_AUDIO defined. UCA222 drives the AudioSystem chain; OTG taps/injects without `AudioConnection`.
- **Never do blocking I/O in USB completion handlers.** `CLogger::Write` (syslog UDP) in completion causes periodic audio gaps.
- **Isochronous USB requests require `AddIsoPacket(maxPacketSize)`** before `SubmitAsyncRequest` on xHCI (host side).
- **DWC2 OTG isochronous frame parity: use software toggle, never DSTS SOFFN.** Reading DSTS races with SOF advancement and causes half-rate transmission. Both IN and OUT endpoints toggle `m_bIsoOddFrame` on every `BeginTransfer`.
- **UCA222 IN and OUT are on separate USB device nodes.** `s_pThis` = IN, `s_pOut` = OUT.
- **Factory intercepts only `int1-2-0`** (audio streaming), not `int1-1-0` (audio control).

## USB MIDI (APC Key 25)

- **Host-side MIDI OUT must be async.** `CUSBMIDIHostDevice::SendEventsHandler` was originally `GetHost()->Transfer()` — synchronous, blocking the main loop on any endpoint stall. After 5-10 min of steady LED traffic this reliably caused the main loop to block, which starved `pTheAPC->update()` — LEDs froze and queued button commands never executed, while inline CC handlers (running from the IN completion ISR, independent endpoint) kept working. Fix: `patches/usbmidihost.cpp` uses preallocated DMA buffer slots + `SubmitAsyncRequest` with a completion callback; drops frames if all slots busy (`g_midiOutDropped`).
- **LED updates coalesce**: `apcKey25Transpose.cpp::_updateGridLeds` uses `sendLedCoalesced()` — sends NoteOn only when the LED value changed. Reduces send rate 10-50× in steady state.
- **MIDI IN runs in ISR context.** Packet handler `packetHandler` (in `usbMidi.cpp`) called from URB completion. CC handlers in `apcKey25::handleMidi` fire inline (filter/effect changes take effect immediately). Note/button handlers use `_queueCmd` which is drained by `pTheAPC->update()` from the main loop.

## MIDI mapping is data — `midiMap.h` atomic controller profile

- **Every in/out MIDI control is an atomic, controller-agnostic record in `midiMap.h`.** `struct MidiInputMap` = one input control keyed on the full `(statusType, channel, data1Lo..data1Hi)` triple → a logical `MidiAction` + `valueMode` + `param`; `struct MidiOutputMap` = one logical `MidiFeedbackState` → a MIDI emission (note + velocity). The looper's logic speaks logical actions/states (`MA_PAD`, `MA_LIVE_PITCH_NOTE_CH1`, `MFS_LOOPER_RECORDING`, …); the controller's physical addresses and LED colour scheme live ONLY in the default profile. Swap `g_activeProfile` (a `MidiControllerProfile` bundling both tables + VU thresholds + mod-deadzone) and a non-APC controller drives the same looper without code changes.
- **`g_apc25Profile` is the default and is byte-identical to the former hard-coded behavior** — this is the load-bearing no-operational-change invariant ("we must not change any of the operational controls"). The primary target is the APC Key 25. `scripts/test-midi-config-parity.cpp` drives the FULL event matrix (note-on + CC, all 16 channels, every data1) through `midiMapResolveInput` and asserts the SAME logical action the legacy `handleMidi`/`_onButton`/`handleFilterCC`/`handleEffectsCC` branches take, and asserts every feedback state resolves to the SAME velocity legacy `_updateGridLeds` sent (`ALL PASS`).
- **Output side is LIVE config-driven.** `apcKey25Transpose.cpp::_updateGridLeds` classifies each looper/preset into a `MidiFeedbackState` and resolves the velocity via `_ledVel()`→`midiMapResolveOutput(g_activeProfile, …)`; VU buckets key off `g_activeProfile->vuMid/vuHigh`. The live-engage LED (`apcKey25.cpp::update`) resolves its note (`APC25_LIVE_LED_NOTE`) + on/off velocity the same way. A profile that omits a state resolves to `0`/OFF — silent, never a stuck LED (graceful degrade).
- **Controller nuance is first-class.** `MidiValueMode` = `{MV_ABSOLUTE, MV_RELATIVE_TWOS, MV_RELATIVE_SIGNBIT, MV_TRIGGER}` so endless/relative encoders are supported without forcing the APC's absolute 0-127 style (`midiMapDecodeValue`). `channel` is part of the atomic key so the APC's ch0/ch1/ch2 pitch scheme is encoded explicitly and a single-channel controller can avoid it. `data1Lo..data1Hi` lets one row cover a contiguous block (the 40 grid pads) so a different grid size is just a bounds change.
- **Storage: compiled-in default, optional SD override.** The profile is a compiled-in table (zero new deps — no JSON/parser bloat, per the maintenance-surface rule). FatFs is mounted (`patches/kernel.cpp` `f_mount`, `patches/std_kernel.cpp`), so a future `SD:/firmware/midimap.txt` can replace `g_activeProfile` at init without touching any caller — the `g_activeProfile` indirection that makes that possible already exists.

## Rubber Band integration (clip time-stretch only)

- Each `loopClip` owns a `RubberBandWrapper`. Separate from live pitch (uses signalsmith-stretch).
- `loopClip::update()` calls `feedAudio` then `retrieveAudio` per block. s16↔float conversion inline (÷×32768).
- Tempo sync: `atomic<float> m_tempoRatio`. Link handler writes atomically. `setTimeRatio()` is RT-safe.
- Memory: ~5.1 MB/wrapper (pre-alloc `setMaxProcessSize(524288)`). 5 clips × 5.1 MB = 25.5 MB.

## Clip state machine

- **`ClipState` enum** (9 mutually-exclusive values) replaces former 7 `CLIP_STATE_*` bitmask defines.
- **`CS_RECORDING_TAIL` vs `CS_FINISHING`** encode whether clip auto-plays after recording ends.
- **`CS_LOOPING`** encodes former `PLAY_MAIN|PLAY_END`.
- **`loopClip.cpp` split** into three files: `loopClip.cpp` (init/transitions), `loopClipUpdate.cpp` (per-buffer audio), `loopClipState.cpp` (state/quantize).
- **Quantize**: `_calcQuantizeTarget()` uses fixed 7-candidate array {M/8, M/4, M/2, M, 2M, 4M, 8M}; M==0 (first loop) → `_backdatedRecordLength()` floored at `CROSSFADE_BLOCKS*2`.
- **FSM lifecycle (rationalized — must stay predictable):** `RECORDING` →(≥CROSSFADE)→ `RECORDING_MAIN` →(stop)→ `RECORDING_TAIL` (willPlay) | `FINISHING` (not) →(`record_block≥max_blocks`)→ `_finishRecording` → `RECORDED` →(willPlay)→ `_startPlaying` → `PLAYING` →(wrap)→ `LOOPING` →(xfade done)→ `PLAYING`. `max_blocks = num_blocks + CROSSFADE_BLOCKS` (the seam tail is overlap, NOT extra loop length).
- **FIRST-LOOP EXACT-REGION INVARIANT (load-bearing):** the first loop must loop its exact recorded region `[m_recStartBlock, +num_blocks)` seamlessly from block 0 every cycle. Three rules enforce it: (1) **TAIL playback drives NO state transition** — the `pp_main` advance has an explicit `CS_RECORDING_TAIL` branch that wraps the monitor head with no crossfade/LOOPING flip; TAIL ends ONLY via `_finishRecording` at `record_block≥max_blocks` (previously TAIL self-advance hit `_startCrossFade` mid-tail, flipped to LOOPING, skipped finish, double-counted running → the "funny place" seam). (2) **`_startCrossFade` resets `m_play_block=0`** (new loop restarts the exact region) while `crossfade_start = num_blocks` (outgoing tail) — previously left play_block at num_blocks so the main head read the tail not block 0. (3) **`_finishRecording` calls `_startPlaying(preservePlayBlock=true)`** so the TAIL→PLAY handoff keeps the monotonic monitor head (no backward jump); a fresh resume uses `preservePlayBlock=false` to re-anchor.
- **Playback-advance rule (single predictable rule, `loopClipUpdate.cpp`):** keyed on state then (has-master, L vs M). `CS_RECORDING_TAIL` → monitor wrap, no state change. Sub-phrase AND first loop (M>0, **L≤M**) → `play_block = (masterPhase − recordStartPhaseOffset) % L` (phase-locked to grid; block i was recorded at phase `offset+i` so `(phase−offset)%L==i` is the TRUE mapping, advancing by 1/block and wrapping at L like a self-advance but re-derivable from masterPhase), wrap→LOOPING gated on `CS_PLAYING`. Else (phrase-or-longer L>M, no-master) → self-advance, wrap→`_startCrossFade` gated on `CS_PLAYING`. The wrap/crossfade transition is ALWAYS `CS_PLAYING`-gated so recording states never trigger it. **PAUSE/RESUME PHASE-SYNC (load-bearing):** the first loop must be in the phase-locked branch (L≤M, was L<M). When L==M self-advanced, `_startPlaying`'s resume re-anchor `(masterPhase−offset)%L` DIVERGED from the self-advancing head whenever `recordStartPhaseOffset≠0` (a first loop recorded mid-phrase), so resume restarted OFFBEAT by up to ~L blocks. Phase-locking L==M makes play and resume use one formula → resume lands on the same grid position a never-paused clip holds (resume must not change phrase sync). First-loop exact-region invariant preserved: starts at block 0 at the recorded downbeat, covers [0,L) in order, seam fires once/cycle. Witnessed `scripts/test-resume-phase.cpp` + `scripts/test-first-loop-region.cpp` (both ALL PASS).
- **PAUSE = MUTE, the play head never stops (load-bearing).** Pausing a recorded loop (per-track `STOP_TRACK` gesture → `LOOP_COMMAND_STOP` on `CS_PLAYING`/`CS_LOOPING`) does NOT reset the head or drop to `CS_RECORDED`. It latches a clip-level `m_paused` that gates ONLY the output, via a click-free per-block `m_pauseGain` ramp (1/16 per block toward 0 paused / 1 playing, per-sample interpolated in `loopClipUpdate.cpp`). The clip STAYS `CS_PLAYING`/`CS_LOOPING`, so the play-head advance (the masterPhase-locked / self-advance block in `loopClipUpdate.cpp`, which runs OUTSIDE the `!m_mute` output guard) keeps advancing every block — position is phase-locked to the master whether paused or not. **Resume (`LOOP_COMMAND_PLAY` on a paused clip) just clears `m_paused`** — NO `_startPlaying` re-anchor — so position never changes and rapid mute/unmute is instant and click-free. **Once a loop is recorded it never stops playing the same way it started:** it enters `[0,L)` at its recorded downbeat and the head is continuous on masterPhase forever; pause only mutes. `getTrackState` (`loopTrack.cpp`) reports a `CS_PLAYING`/`CS_LOOPING` clip with `isPaused()` as `TRACK_STATE_STOPPED` (NOT `PLAYING`) so the pad blinks yellow (`MFS_LOOPER_PAUSED`) and the paused-tap gesture resumes it; per-clip VU reads the gated output so a muted pad shows paused, not active VU. Pause touches NO `incDecRunning` (the clip stays in the running set, advancing). A REAL stop — `stopImmediate` (shift+STOP / abort) and `_startRecording` — clears `m_paused`/`m_pauseGain` (head reset, starts audible); that path is distinct from the mute-pause. `_applyPreset` (arrangement recall) play/pause per looper composes with this: it mutes/unmutes loopers with zero position change. Witnessed by `scripts/test-pause-mute.cpp` (`ALL PASS`: position-identical through pause+resume for L<M, L==M off 0/1234, L>M, 10-phrase pause = zero drift; gain 0/1; click-free <0.005/sample; rapid-toggle position+bounds) and the unchanged `scripts/test-resume-phase.cpp`/`test-first-loop-region.cpp` (still ALL PASS).
- **Start-phase grids (two regimes in `_startRecording`):** **FIRST loop (`m_masterLoopBlocks==0`)** starts IMMEDIATELY on the press (no latch wait) so it backdates: `m_recStartBlock = cbBackdatedBlock(press)`, `m_recordStartPhaseOffset = masterPhase - backBlocks` (sample-true; it defines the grid). **CONSECUTIVE loop (`M>0`)** is fired by the pending-record LATCH in `loopMachine::updateState`, which triggers EXACTLY at a beat downbeat (`m_masterPhase % gridStep == 0`), so it anchors to the latch instant: `m_recStartBlock = cbBackdatedBlock(0)`, `m_recordStartPhaseOffset = m_masterPhase` — **NO press-backdate, NO re-snap** (the phase is the latch beat). The content block uses `cbBackdatedBlock(0)` (fixed-lag-only = `g_cbWriteBlock - CB_FIXED_LAG_SAMPLES/AUDIO_BLOCK_SAMPLES`, a WRITTEN history block), NOT raw `g_cbWriteBlock`: `g_cbWriteBlock` is the NEXT-to-write block (`cbWriteBlock` writes dst THEN increments), and the latch runs AFTER `cbWriteBlock` in `loopMachine::update`, so anchoring at `g_cbWriteBlock` made clip block 0 read the unwritten/stale ring head = SILENCE → the consecutive clip recorded nothing and played no audio. `cbBackdatedBlock(0)` lands on real written audio with the same fixed ring lag the first loop uses, and ignores the stale `g_pendingPressTicks`. Backdating a consecutive loop was wrong: `g_pendingPressTicks` is STALE at latch time (set only in the APC cmd drain when the pending was SET, never refreshed for the later latch in a separate Core-2 call), and the wait-for-grid latch already absorbed the press→grid latency; backdating the stale tick + round-to-nearest re-snap landed the start on a neighboring (later) beat = the "second loop starts a little later / doesn't coincide with the first loop" shift. There is also NO stop-time floor (playback reads the pre-recorded clip buffer, so a stop-time `m_recStartBlock` change is inert and flooring only the phase shifts it). Witnessed by `scripts/test-consec-lock.cpp` (`ALL PASS`: latched offset == latch beat, `play_block==0` at offset coincides with the first-loop start).
- **CONSECUTIVE-LOOP UNIFIED QUANT GRID (505-like, load-bearing for sync):** every non-first loop locks to the first loop's start via ONE grid = powers-of-two of the phrase M, anchored to `masterPhase % M == 0` (phrase 0, since masterPhase is reset to 0 at first-loop define and only `++`/`%M` after). The touchpoints share this grid: (1) latch starts the record (content) on the M/16 beat grid; (2) `_calcQuantizeTarget` quantizes LENGTH to the `{floor=CROSSFADE*2, M/8,M/4,M/2,M,2M,4M,8M}` grid by NEAREST-ON-LOG2 with a safe-DOWN bias — take the largest candidate `<= rec`, round UP to the next only when `rec*2 > c` (strict, past midpoint). A SHORT tap quantizes DOWN to a division `<= rec` so it PLAYS immediately (tight hi-hat loops); a near-phrase tap rounds UP to the phrase multiple (505 play-through); exact/midpoint rounds down. The old nearest-of-all rounded a short tap UP to a larger division (`>rec`), so `updateState(PLAY)` took the deferred branch and the clip sat in `CS_RECORDING_MAIN` waiting for `record_block>=target` forever = "second loop records then silent, never starts". (3) playback `play_block=(masterPhase-offset)%L`. **`recordStartPhaseOffset` is set ONCE, at `_startRecording`, beat-snapped (M/16), tied to `m_recStartBlock` (same snapDelta applied to both) so clip buffer block 0's audio == that phase instant.** There is **NO stop-time floor** of the offset: playback reads the CLIP BUFFER (`getBlock(m_play_block)`), which was filled incrementally during recording from the beat-snapped `m_recStartBlock` — so flooring the offset at `_startEndingRecording` shifted the PHASE while the already-recorded audio stayed put (a `m_recStartBlock` change post-record is INERT — playback uses the buffer, not the ring), making the second loop play OFFBEAT by floorBack. The beat-snapped offset is already coherent with every L (all of `{M/8..8M}` are multiples of the M/16 beat grid), so `play_block` is 0 exactly at the recorded downbeat and at every L-boundary. Sync needs offset==buffer-block-0, which the record-start beat-snap gives. Earlier "off by a quantized amount" came from this stop-time floor (now removed) and from independently snapping content vs phase (now tied at record-start). Req1: the phrase M stays the RAW first-loop length (re-snapping M would shift the first loop off its exact region); `linkDeriveQuant` only sets the broadcast `s_bpm` to the nearest-120 multiple/division. Witnessed by `scripts/test-consec-lock.cpp` (`ALL PASS`: every L in {M/4..4M} phase-locks to phrase 0).
- **`:4445 CLIP[<n>]` verb** (kernel_run.cpp → `loopClipTelemetry`): `track=<n> state=<ClipState> play=<block> rec=<block> num=<blocks> max=<blocks> running=<n>`. Witnesses the seam/wrap live without audio capture.

## Logging

- `LOOPER_LOG(fmt, ...)` macro is **currently a no-op** (`Looper.h` #else branch). Both queued-log and immediate-CLogger paths disabled. The `logString_t` queue, `getNextLogString()`, drain loops in `audio.cpp::loop()` and `uiWindow::updateFrame()` exist but dormant. Do not re-enable the queued path without first making `LogUpdate()` ISR-safe (currently `new logString_t` and `new CString()` are not — will corrupt the heap under audio-ISR log load).
- **Observability now**: ISR-safe lock-free event ring (`patches/audioTelemetry.{h,cpp}`, 256-slot SPSC). ISR sites push `(code, ticks, arg)` triplets — no allocs, no UDP. `audio.cpp::loop()` (Core 2 control plane under ARM_ALLOW_MULTI_CORE) drains up to 32 events per call and emits one `CLogger::Write` per event; producers also bump counters that a 0.5Hz summary line reports as deltas (only when nonzero). Event codes: `IN_UR`, `IN_RS`, `OUT_UR`, `OTG_RS`, `WD`, `LAG`, `DISP_FULL` (cross-core dispatch overflow — Core 1 not draining fast enough). Drops counted in `g_telemDropped`; cross-core dispatch drops in `g_dispatchDropped`.

## WiFi and Ableton Link

- **Ethernet is boot/syslog.** `CNetSubSystem` uses static 192.168.137.100, gateway/DNS 192.168.137.1, syslog target 192.168.137.1. No DHCP wait.
- **WiFi (`CBcm4343Device m_WLAN`) is Link-only.** Raw `SendFrame`/`ReceiveFrame`, no `CNetSubSystem` on WLAN. Init failure is warning.
- **WiFi join / AP fallback**: `JoinOpenNet("ticker")` first; on failure `CreateOpenNet("ticker", 6, false)`. Static AP IP 192.168.4.1. Bare-metal DHCP server in `wlanDHCPServer.cpp` (pool 192.168.4.2-9).
- **WiFi firmware** (`brcmfmac43455-sdio.{bin,txt,clm_blob}`) at `SD:/firmware/`.
- **Ableton Link** in `abletonLink.cpp` builds raw ETH+IP+UDP multicast `224.76.78.75:20808`. Magic `_asdp_v\x01`. Parses `tmln` TLV (microsPerBeat + beatOrigin + timeOrigin big-endian). Alive every 1s.
- **Link-driven quantize**: when synced, `m_masterLoopBlocks = round(690 * 60 * 16 / bpm + 0.5)` rounded to multiple of 8 (4-bar phrase).
- **`linkProcess()`** called in main loop after `loop()`, before `m_Scheduler.Yield()`.
- **IGMP v2** report sent for `224.76.78.75` after DHCP, then every 30s.
- **libwlan.a** built via `make -C circle/addon/wlan RASPPI=4 AARCH=32`.

## Test coverage

- **`test.js`** at project root — pure-JS simulation of ring buffer drift correction / interp / underrun behavior. 9 cases including bit-exact steady-state, linear-interp correctness, ramp monotonicity, 5000-iter ±0.1% drift stability. Runs via `node test.js`.
- **`test/looper-sim.js`** — 48 higher-level scenarios covering phrase quantize, multi-phrase clips, stop-quantize, deferred quantize, per-track latch.

## OTG gadget audio build caveats

When patching Circle's `lib/usb/gadget/` to add `CUSBAudioGadget`:

- **Include path**: `#include "dwusbgadgetendpoint.h"` (quotes) when compiling from `lib/usb/gadget/`.
- **DMA_BUFFER macro unavailable**: replace `DMA_BUFFER(u32, m_DummyBuffer, 1)` with plain `u32 m_DummyBuffer`, use `&m_DummyBuffer` for `void*`.
- **API version**: `FinishTransfer(void)` no args, `OnControlMessage(void)` virtual non-static, `HandleUSBReset(void)` non-static.
- **Visibility**: `AudioInputUSB::start()` / `AudioOutputUSB::start()` must be public in patched headers (protected upstream).
- **Audio Makefile**: `usbaudiogadget.o` and `usbaudiogadgetendpoint.o` in `patches/audio_Makefile` OBJS list.
- **Looper Makefile**: `LOOPER_OTG_AUDIO` needs explicit `ifdef / DEFINE += -DLOOPER_OTG_AUDIO / endif`.
- **UAC1 AC Header size**: with `bInCollection=2`, descriptor is 10 bytes (not 9).

## Live pitch shifting via MIDI

- **`pLivePitchWrapper`** allocated unconditionally in `audio.cpp::setup()`. In `loopMachine::update()`, audio bypasses wrapper when `pTheAPC->getDebugState().liveEngaged == false` (zero latency).
- **Pitch shift path (RubberBandWrapper.h)**: ONE algo — **EngineSoladSnac** (`patches/soladSnacOctaver.h`). Pitch-only (no time-stretch) shifter combining solad-style single-delay-line variable-speed read with phase-coherent splices, and McLeod SNAC pitch detection for splice alignment. Splice triggers: drift OOB (gap > `period * m_respliceFrac`, frac=16), or transient detector (currently disabled — re-enable after spectral-flux upgrade). Value-and-slope matched splice point search within ±period/2 picks the cleanest crossfade boundary.
  - **Splices MUST be frequency-neutral** — this is the load-bearing invariant. The continuous read rate is exactly `m_scale` (0.5 at -12), so the output pitch = exactly half the input ONLY if each splice repeats an EXACT integer number of periods (a splice that repeats a whole period advances the waveform phase by zero). The `bestOff` value/slope match slide (±period/2) and the `n=round(drift/per)` rounding both leave a sub-period residual; on a synthetic sine that is harmless (consecutive periods identical → host renders exact at all freqs) but on REAL input each period differs slightly so the match search acquires a consistent-sign bias → every splice nudges the reader a fraction of a period → the long-term read rate drifts off 0.5. At low notes (large period, rare splices) it does not average out → audibly FLAT (pre-fix: 82→39 not 41.2, 110→53 not 55). **Fix (`triggerSpliceByPeriod`): after `bestOff` picks the matched boundary, snap the net jump to the nearest WHOLE number of periods** (`nWhole = round(rawJump/per)`, `anchored = rdActive + nWhole*per`, gap-safe drop-periods loop). The crossfade still lands on the matched point (<1 period away, inaudible) but the displacement is provably integer*period. Ring untouched — fix lives entirely in the effect.
  - **Quiet-input coast**: when `m_envSlow < 0.004` (silence between notes), the emergency buffer-wrap escape is SUPPRESSED — instead both readers are repositioned to `m_wr - initialReadOffset` silently (no crossfade splice). On silence SNAC otherwise peaks on a spurious long lag and the gap runs to the wrap bound, firing an unaligned reset that lands the reader off-phase → click at the NEXT note-on. Coasting holds the last-good period and keeps the next note clean.
  - **Capture-free verification telemetry** (audio.cpp `eng` line): `eff=N.NNNN` = continuous read rate alone (must read 0.5000 at -12; do NOT add splice jumps — that measures reader-vs-writer catch-up ≈1.0, not pitch). `perr=N` = mean |splice fractional-period error| in 1/100 samples (must read 0 post phase-anchor = splices frequency-neutral on real input). These let you confirm -12 accuracy from HDMI/syslog without the (marginal) loopback audio capture.
  - Host-validated: pitch lock <0.3 Hz across E2-E4, transient timing preserved (vs prior sinc-delay engine which time-stretched), zero clicks on sustained material (E2/C3/pluck after V8 tuning).
  - **Pi-hardware-validated** (build 1047828B): 110Hz→`eff=0.5000 perr=0 period=436 lock=1`; 82Hz low-E→`eff=0.5000 perr=0 period=585 lock=1`; silence tail→`emerg+0` (no inter-note click). Exact -12 at all measured notes after the phase-anchor fix.
  - **Latency (NON-NEGOTIABLE ~4ms budget — restored, host-validated)**: total downshift monitoring latency = `INITIAL_READ_OFFSET_DEFAULT` (fixed algorithmic headroom) + the irreducible ~1-period PSOLA reader lag (at downshift the reader MUST lag the writer — the lag IS the pitch shift). With `m_respliceFrac=1` the reader gap is held at `offset + <=1 period` (the PSOLA minimum); SNAC detection adds 0 latency (retrospective splice picking). Two knobs set the budget: (1) `m_respliceFrac` — was raised 1→8 to cut the splice rate (the ~55/s amplitude-dip buzz on imperfect Pi input), which traded the budget away for a `offset + per*frac/2` swing = 22-53ms (a REGRESSION); restored to **1** now that the integer-period value+slope matched splice (`triggerSpliceByPeriod`, commit 29b131f) makes frequent resplicing seamless even on noisy input. `setRespliceFrac` floor clamp was 2.0 (silently masked sub-1 values) → relaxed to 0.25. frac<1 is rejected: a sub-period drift forced through a ≥1-period forward jump overshoots and lands off-phase (`perr`≈32). (2) `INITIAL_READ_OFFSET_DEFAULT` — was **192** (4ms of REDUNDANT headroom stacked ON TOP of the 1-period lag); trimmed to **64** (1.3ms, well above the 16-tap sinc margin `SINC_HALF+2=10`). **Host-validated** (`scripts/test-transpose-latency.cpp`, scale=0.5, 0.003-stddev gaussian-noise Pi-input emulation, 4s/pitch steady-state, ALL PASS): 220Hz=**3.6ms** (≤4.5ms budget assert), 330Hz=4.3ms, 110Hz=5.9ms, 82Hz low-E=7.4ms — `eff=0.5000` (exact -12), `perr=0` (frequency-neutral), `maxStep≤0.013` (seamless, no buzz), `emerg=0`. The low-E (82Hz) ~7ms floor is one period — the hard PSOLA limit, unbeatable without a different algorithm. The old "64 was invalid (measured passthrough)" note predates the ch2-MIDI-engage fix; this sweep drives the engine engaged so 64 is now valid.
  - Prior `SincFormantOctaver` and `yinPsolaOctaver` engines still in tree but unwired. signalsmith STFT also still linked (used by loop-clip RubberBand path).
- **Formant depth knob** (CC53): single-knob, ∈ [-1, +1] centred at data2=64 with ±4 deadzone.
  - `d = 0`: natural pitch shift, formants slide with pitch (deep/dark)
  - `d = +1`: formants preserved at original pitch (vocal-octave character)
  - `d = +∞` (overdriven): formants exaggerated opposite to pitch (bright/extreme)
  - `d = -1`: formants doubled-down with pitch (huge/monster)
  - **Implemented as a grain-playback-speed formant stage** (`patches/grainFormant.h`, crossfaded against the `EngineSoladSnac` continuous-reader −12). This is the classic PITCH-SHIFTER formant effect (formants ride with pitch ↔ preserved), NOT vocal/LPC envelope shifting. **Prior wrong approaches (do NOT revisit):** (a) pre-resample before the pitch stage — resampling moves pitch+formant together and cancels, centroid didn't move, only doubling; (b) LPC envelope-remap EQ (`lpcFormant.h`, removed) — the per-frame pole estimate wandered = audible envelope/wah, not a stable formant; (c) re-graining the *output* — doubled pitch at factor>1; (d) in-reader sawtooth read-speed warp — clicked once per period. **The correct technique (research: granular DSP; HiFi-Glot arXiv 2409.14823 Feb 2026 confirms TD-PSOLA can't do it natively → neural too heavy for bare-metal):** pitch = grain EMISSION rate, formant = grain PLAYBACK SPEED (a *constant* resample ratio = a STABLE shift). `grainFormant.h` reads the DRY input with three independent rates — output-epoch spacing `Tin/scale` (sets −12 pitch), input-epoch advance `Tin`/emission (consumes at `scale`), grain content read at `fm` (formant) — Hann 2-period OLA. **The streaming gap-bound is load-bearing:** the input epoch lags the writer (= the −12 lag); when it drifts past `targetLag ± 2·Tin` it RESPLICES by a WHOLE number of input periods (phase-preserving, so pitch stays exact — the Hann overlap hides it). `fm = pow(2, depth)`; mix crossfades from the continuous reader (center = byte-identical −12) to the grain path off-center. Witness `scripts/test-formant-grain.cpp`: pitch exact −12 (98Hz) at every depth, envelope band-ratio swings 0.068→14.1, 0–1 clicks over a 10s stream. Hardware-verified (kernel b9dd262): −12 fund 98Hz exact across the sweep, clicks at the rig floor.
  - CC56 (resonance) and CC57 (peak freq) from the old `SincFormantOctaver` post-EQ are no longer mapped — those knobs are silent.
- **Live -12 expressive controls** (CC mapping in `apcKey25.cpp::handleEffectsCC` / `apcKey25Filters.cpp::handleEffectsCC`):
  - **CC53** brightness ∈ [-1, +1]: data2=64 ±4 deadzone → 0 (neutral). Outside deadzone linearly maps to high-shelf gain ±12 dB at 800 Hz on the sinc octaver AND signalsmith formant factor.
  - **CC56** resonance ∈ [0, 1]: linear data2/127 → peaking EQ gain 0..+12 dB at `m_formantFreq`.
  - **CC57** peak freq: log-mapped 300..3000 Hz via `300 * 10^(data2/127)`.
  - All three call `RubberBandWrapper::setFormantEq(brightness, resonance, freqHz)` which forwards to both stereo SincFormantOctaver instances. Biquads redesigned only when knob value changes; per-sample cost is two biquad evaluations (~6 mul/add total).
- **Pitch scale**: `_applyLivePitch()` calls `setPitchScale(pow(2, semitones / 12))`.
- **CC1 (mod wheel)**: deadzone 59-69 disengages. Outside: ±12 semitones by `((data2 - 64) * 12 / 63)`.
- **CC52**: linear 0-127 → ±12 semitones (full APC keyboard range).
- **Channel 1 note-on (0x91)**: toggles engage; pitch = note - 60.
- **Channel 2 note-on (0x92)**: pitch = note - 60, always engages.

## Looper grid + presets (simplified UI)

The APC25 grid is the only operator surface. 20 flat loopers + 10 preset
slots. NUM_TRACKS=20, NUM_LAYERS=1 (each pad = one independent looper).

**Pad layout:**
- **Cols 0-1** (10 pads, rows 0-4): preset slots. `_presetFromPad(row, col)` returns `row*2 + col` ∈ [0, 10).
- **Cols 2-5** (20 pads, rows 0-4): loopers. `_looperFromPad(row, col)` returns `row*4 + (col-2)` ∈ [0, 20).
- **Cols 6-7**: explicitly blanked each LED tick (`HALVE/DOUBLE_TRACK` commands kept as harmless press handlers; LEDs always OFF).

**Looper pad gestures** (apcKey25Notes.cpp::_onPadRelease):
- empty pad tap → `LOOP_COMMAND_CLEAR_LAYER_BASE + n` (arms record on first clip)
- recording tap → `LOOP_COMMAND_TRACK_BASE + n` (finish + play)
- playing tap → `LOOP_COMMAND_STOP_TRACK_BASE + n` (pause = MUTE; head keeps advancing)
- paused/stopped tap → `LOOP_COMMAND_TRACK_BASE + n` (resume = un-mute; position-identical, instant)
- long-hold ≥ `APC_HOLD_ERASE_MS` (1000 ms) → `LOOP_COMMAND_CLEAR_LAYER_BASE + n` (full erase)

**SHIFT-hold = route the LOOPS INTO the live effects (load-bearing).** While the
APC25 SHIFT button is held, the running loops THEMSELVES become input to the live
pitch+effects chain — so the loops get transposed/effected and can be recorded
through the effects. SHIFT does NOT mute the loops (the earlier "gate loop output
to silence, hear only the live input" behavior was wrong — pressing SHIFT made the
loops go silent). Path: `apcKey25::update` publishes `m_shift` ->
`LiveParams.monitorMode` (paramSnapshot) -> `loopMachine::update`. The order in
`update` is reorganized so the **loop output is computed FIRST**: `updateState()`
+ the per-track audio (into `m_output_buffer`) + the Link-grid block all run
BEFORE the pitch/effects stages (clip playback reads each clip's own buffer and
`m_masterPhase` advances once per block, so the reorder is phase-neutral). Then,
when SHIFT is held, the loop sum is **folded into the effect source**
(`m_input_buffer += m_output_buffer * m_loopFoldGain`) BEFORE the pitch(`pLivePitchWrapper`)
and effects(`pEffectsProcessor`) stages and before `cbWriteBlock` — so the loops
are pitch+effected AND the wet result is what gets recorded. `m_loopFoldGain`
ramps 0->1 when held. The DRY loop contribution to the final mix is suppressed
COMPLEMENTARILY: `m_loopOutputGain = 1 - m_loopFoldGain`, so the loops are heard
ONCE (through the effects, via the processed `ival32`), with no double-sum and no
loudness jump (`dry + fold == 1` at every sample). The live mic input still passes
(loops are ADDED to it). Released, `fold->0`/`dry->1` and loops return to normal
dry output, phase-seamless (clips never stopped advancing). Both ramps are
per-sample-interpolated (1/16 per block = ~21ms, click-free). SHIFT also still
modifies the transport chords (`shift+STOP/RECORD/PLAY`) and CC53 formant range —
monitor mode is a passive read of `m_shift` and does not consume the press.
Surfaced in the `:4445 TIME` verb as `monitor=<0/1> loopGate=<dryGain*100>`
(100 = loops fully dry, 0 = loops fully routed into the effects). Witnessed by
`scripts/test-monitor-route.cpp` (`ALL PASS`: complementary/energy-conserved,
click-free, fold/dry endpoints, rapid-toggle bounded, phase-neutral).

**Microrepeat = synced latch beat-repeat/stutter (notes 82-86, load-bearing).**
`patches/microRepeat.h` is a DSP stage in `loopMachine::update` applied to
`m_input_buffer` AFTER the pitch+effects stages and BEFORE `cbWriteBlock`. While a
latch note is HELD it repeats a beat-fraction slice of the FULL mix (live input +
ALL loops) in sync with the master beat grid: note 82 = 1 beat, 83 = 1/2, 84 =
1/4, 85 = 1/8, 86 = 1/16 beat. `beat = m_masterLoopBlocks/16` blocks; slice =
`beat/div * AUDIO_BLOCK_SAMPLES` samples, clamped to `MR_MAX_SLICE`. The latch is
a member `m_microRepeatDiv` set in `apcKey25::handleMidi` (note-on 82-86, BEFORE
the pad/button dispatch so note 84 OVERRIDES `APC_BTN_FORMAT`; note-off clears it
only if it owns the active div — newer held note wins), published via
`LiveParams.microRepeatDiv` (paramSnapshot) and read on Core 1. **EPHEMERAL /
position-passthrough (load-bearing):** the stage NEVER touches clip `m_play_block`
or `m_masterPhase` — the loops keep advancing underneath while latched, so on
release the signal resumes EXACTLY the position it would have had if the repeat
never happened. Because the loop sum is folded into `m_input_buffer` before the
stage (the not-yet-SHIFT-folded remainder `(1-foldEnd)*wet`, gated complementarily
in the final mix by `(1-mrWet)` so the loop sum counts exactly once), the stutter
contains all loops AND `cbWriteBlock` records it — so under SHIFT the stutter
records INTO a recording loop. Engage/release is click-free (1/16-per-block wet
ramp) and the slice is a whole number of samples (grid-aligned wrap). It is part
of the effects layer (`pMicroRepeat` alongside `pEffectsProcessor`). When `div==0`
and the wet ramp has settled, the stage is fully skipped — the chain is
byte-identical to before (no regression to SHIFT/pause/recording). Surfaced in the
`:4445 TIME` verb as `microRep=<div>`. Witnessed by `scripts/test-microrepeat.cpp`
(`ALL PASS`: slice-per-div, verbatim slice replay, position-passthrough,
click-free, div-change retarget, no-master safe no-op, buffer clamp) and the
unchanged regression suite (`test-monitor-route`/`test-pause-mute`/
`test-resume-phase`/`test-first-loop-region`/`test-midi-config-parity` still ALL
PASS).

**Sampler = independent hold-to-record sampler (buttons 65/66, load-bearing).**
`patches/sampler.h` (`pSampler`, allocated in `audio.cpp` like `pMicroRepeat`) is a
sound SOURCE, NOT a looper. It mixes into `m_input_buffer` at the TOP of
`loopMachine::update` — BEFORE the loop-fold / pitch / effects / microrepeat /
filter chain — so sampler audio gets ALL effects and is recordable into a loop
(under SHIFT it folds into a recording loop). It touches NO loopMachine/loopClip/
`m_masterPhase` state, so the looper keeps working while the sampler works.
**Capture reads the DRY mic snapshot taken BEFORE `renderInto` (load-bearing):**
`pSampler->captureBlock(m_input_buffer,...)` runs before `pSampler->renderInto(...)`
so a recording sample never records itself (no feedback) or the loops — it records
the live input source only. **Two gestures (channel-0 buttons, intercepted in
`apcKey25::handleMidi` BEFORE the pad/button dispatch):** button **65** (`0x41`)
HELD records ONE shared chromatic sample; on release the leading/trailing silence
is auto-clipped and the 25 keyboard keys play it pitched chromatically
(`rate = 2^((note-60)/12)`, middle C = note 60 = original speed), polyphonic.
Button **66** (`0x42`) HELD = drum mode: while held, holding a keyboard key records
into THAT key's own drum slot (`keyIndex = note-48`, 25 slots, auto-clip on key
release); a loaded drum slot plays at ORIGINAL pitch as a one-shot and OVERRIDES
the chromatic sample on that key. **Keyboard routing (load-bearing):** when the
sampler has content (chromatic loaded OR this key's drum loaded) the channel-1
keyboard note-on routes to the sampler (live-pitch keyboard transpose is
SUPPRESSED; live pitch stays reachable via mod-wheel CC1 / CC52). With no content
the key falls through to live-pitch transpose (unchanged). **Cross-core:**
`handleMidi` (ISR/control core) pushes note-on/off + rec-start/stop events into a
lock-free SPSC ring on the sampler (`pushEvent`); Core 1 drains it at the top of
`renderInto`. Buffers (`short` s16, heap-allocated once: 5s chromatic + 25×2s drum
slots, ~10.5MB) are written/read only on Core 1. Voice pool = 16, oldest-voice
steal; per-voice attack/release gain ramp + auto-trim edge fades = click-free;
record overrun clamps at buffer max (`_stopRecord` finalizes on a pending target so
an overrun-then-release still loads). Drum slot overwrite stops voices reading that
slot first. midiMap.h: `MA_SAMPLER_RECORD` (65), `MA_SAMPLER_DRUM_MODE` (66),
`MA_SAMPLER_KEY` (channel-1 keys are a RUNTIME overlay on
`MA_LIVE_PITCH_NOTE_CH1`). Surfaced in `:4445 TIME` as `sampRec`/`drumMode`/
`sampLen`/`drumLoaded`/`voices`. Witnessed by `scripts/test-sampler.cpp`
(`ALL PASS`: auto-trim, chromatic resample ratio, drum one-shot, polyphony,
voice-steal, click-free, overrun clamp, no-content no-op).

**Filters run at the END of the effects chain (after the microrepeat glitch,
load-bearing).** `apcEffectsProcessor` is split into `processFilters` (HP/LP SVF)
and `processSends` (delay/reverb). `loopMachine::update` runs `processSends` in the
old effects slot (after pitch), then the microrepeat stage, then `processFilters`
at the very end on `m_input_buffer` BEFORE `cbWriteBlock` — so the filter acts on
the stuttered/effected signal. `processFilterAndSends` is retained (filters then
sends) for any other caller. At default knobs (HP cutoff 0, LP cutoff 1) both
filter guards skip, so `processFilters` is a byte-exact pass-through and the move is
inaudible until a filter knob moves. Witnessed by `scripts/test-filter-order.cpp`
(`ALL PASS`: default-knob parity + order-observable-when-LP-engaged).

**Preset pad gestures**:
- tap → `_applyPreset(p)`: for each looper, play if bit set in stored mask, pause if not. Empty loopers ignored. No-op if preset never captured.
- long-hold → `_capturePreset(p)`: snapshot 32-bit `m_presetMask[p]` of which loopers are currently playing or pending-play. Sets `m_presetUsed[p] = true`.

**Arrangement memory tracks looper erase (load-bearing).** A preset IS an arrangement: `m_presetMask[p]` is the set of loopers it is made of. When a looper is erased (long-hold ERASE_TRACK at `apcKey25.cpp`), `_forgetLooperFromPresets(n)` (`apcKey25Notes.cpp`) drops bit `n` from EVERY mask; any arrangement whose mask reaches 0 is deleted (`m_presetUsed[p]=false`) so `_updateGridLeds` draws its pad OFF — the arrangement's light goes dark exactly when its last member is gone. `CLEAR_ALL` (shift+PLAY) calls `_forgetAllPresets()` (every arrangement empty → all dark). Erasing an empty/non-member looper is a no-op (only a set bit is cleared, deletion only on mask→0); a deleted slot is immediately reusable by a fresh capture. Witnessed by `scripts/test-arrangement-forget.cpp` (17 checks ALL PASS).

**Looper LED encoding** (apcKey25Transpose.cpp::_updateGridLeds):
- empty + no content: OFF
- RECORDING: solid RED
- PENDING_RECORD/PLAY/STOP: solid YELLOW
- PLAYING + clip peak >8000 (≈-12 dBFS): solid RED (clip-light)
- PLAYING + clip peak >1500: solid YELLOW
- PLAYING + clip peak >200: solid GREEN
- PLAYING + silence: solid GREEN
- has-content + paused: blink YELLOW (visible "loaded but silent" marker)

**Preset LED encoding**:
- never captured: OFF
- captured: solid YELLOW

**Per-clip VU**: `publicClip::m_clipPeakLevel` populated each block in `loopClipUpdate.cpp` from the clip's own tmp_L/tmp_R contribution (not the track sum). Drained on read by `_updateGridLeds`.

**Stuck-LED prevention**: `sendLedCoalesced` only commits to `s_lastLedState` cache when `usbMidiSendNoteOn` returns true. Dropped MIDI OUT frames are retried automatically on the next tick.

**Command-CC base layout** (commonDefines.h) — renumbered to fit 20 slots without overlap:
- `LOOP_COMMAND_TRACK_BASE`        0x20 (20 slots)
- `LOOP_COMMAND_STOP_TRACK_BASE`   0x40
- `LOOP_COMMAND_ERASE_TRACK_BASE`  0x60 (still in `loopMachine`, no longer reached from APC)
- `LOOP_COMMAND_CLEAR_LAYER_BASE`  0xA0
- `LOOP_COMMAND_HALVE_TRACK_BASE`  0xC0
- `LOOP_COMMAND_DOUBLE_TRACK_BASE` 0xE0

## Planned architecture (not yet implemented)

- **3-minute rolling recording buffer**: continuous circular fill, record marks in/out, deep-copies into clip. Eliminates start/stop clicks.
