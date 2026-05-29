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

## Rubber Band integration (clip time-stretch only)

- Each `loopClip` owns a `RubberBandWrapper`. Separate from live pitch (uses signalsmith-stretch).
- `loopClip::update()` calls `feedAudio` then `retrieveAudio` per block. s16↔float conversion inline (÷×32768).
- Tempo sync: `atomic<float> m_tempoRatio`. Link handler writes atomically. `setTimeRatio()` is RT-safe.
- Memory: ~5.1 MB/wrapper (pre-alloc `setMaxProcessSize(524288)`). 5 clips × 5.1 MB = 25.5 MB.

## Clip state machine

- **`ClipState` enum** (9 mutually-exclusive values) replaces former 7 `CLIP_STATE_*` bitmask defines.
- **`CS_RECORDING_TAIL` vs `CS_FINISHING`** encode whether clip auto-plays after recording ends.
- **`CS_LOOPING`** encodes former `PLAY_MAIN|PLAY_END`.
- **Phase alignment**: `_startPlaying()` hard-locks `m_play_block = ((masterPhase - recordStartPhaseOffset) % numBlocks + numBlocks) % numBlocks`. Guarantees `play_block=0` at every phrase boundary.
- **`loopClip.cpp` split** into three files: `loopClip.cpp` (init/transitions), `loopClipUpdate.cpp` (per-buffer audio), `loopClipState.cpp` (state/quantize).
- **Quantize**: `_calcQuantizeTarget()` uses fixed 7-candidate array {M/8, M/4, M/2, M, 2M, 4M, 8M}.

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
  - **Latency**: ~4 ms (192-sample initial read offset). SNAC detection adds 0 latency to audio (runs retrospectively for splice point picking).
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
- playing tap → `LOOP_COMMAND_STOP_TRACK_BASE + n` (pause-at-cycle)
- paused/stopped tap → `LOOP_COMMAND_TRACK_BASE + n` (resume play)
- long-hold ≥ `APC_HOLD_ERASE_MS` (1000 ms) → `LOOP_COMMAND_CLEAR_LAYER_BASE + n` (full erase)

**Preset pad gestures**:
- tap → `_applyPreset(p)`: for each looper, play if bit set in stored mask, pause if not. Empty loopers ignored. No-op if preset never captured.
- long-hold → `_capturePreset(p)`: snapshot 32-bit `m_presetMask[p]` of which loopers are currently playing or pending-play. Sets `m_presetUsed[p] = true`.

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
