## 2026-07-08g — Fix buzz+crunch persisting after 2026-07-08f: bias UAC2 IN nominal estimate below the true mean

- fix: patches/usbaudiodevice.cpp StartInRequest's fractional-accumulation fix (2026-07-08f) made the claimed byte count's LONG-RUN average exactly match the true rate, but nominal rate is a MEAN -- roughly half of all real microframes legitimately deliver LESS than the mean (clock-tolerance jitter around it, not free variation), and InCompletion never checked pURB->GetStatus()/any real per-completion delivered-byte signal for UAC2 before trusting the claimed count, so every such below-mean microframe still over-read into stale buffer tail. This is why the user reported buzzing AND the original underrun crunch/blips recurring together on a predictable ~1-2s cycle even after 2026-07-08f: the exact-mean claim didn't fix the structural over-read risk, only its long-run bias. Root-caused by re-reading dwhcixferstagedata.cpp's TransactionComplete: m_nTotalBytesTransfered (the real accumulated transfer count) is genuinely correct, but Circle's vendored USB stack exposes no reliable per-completion actual-vs-nominal delta through GetResultLength() (confirmed broken for multi-packet iso URBs) or any other accessor -- so no correct-by-construction real-data cap is available from this patch layer. Fix: bias nomRate down by a small fixed margin (8134/8192 ≈ 99.29%, ~0.71% low) so the claimed byte count is a soft LOWER bound the overwhelming majority of real microframes clear, converting the residual short-microframe case from an audible over-read into a small, real, continuous rate deficit that patches/input_usb.cpp's existing IN_TARGET_LAG/IN_DEADBAND drift correction (designed for exactly this class of slow deviation) absorbs inaudibly. Build-verified (kernel7l.img 1099748 bytes) and flashed to E:\kernel7l.img.

## 2026-07-08f — Fix periodic ~1-2s crunch+blips cycle: fractional accumulation for UAC2 IN nominal rate

- fix: patches/usbaudiodevice.cpp StartInRequest's nominal-rate fix (2026-07-08e) reduced per-completion buzz but introduced a new symptom: buzzing AND the original underrun crunch+blips audible together, recurring predictably on a ~1-2s cycle. Root cause: nomSamplesPerUframe was a single TRUNCATED integer per call ((m_uRate+4000)/8000) -- for a device negotiated at 44100Hz this claims 6 samples/microframe when the true value is 5.5125, an 8.8% SYSTEMATIC (not per-call-random) bias. That constant bias accumulates in the IN ring's avail level fast enough to overshoot AudioSystem.cpp's drain hysteresis deadband -- a mechanism whose own inline comment describes exactly this failure mode ("A bare threshold hunts: drain->over->stop->refill->drain, oscillating every ~1s with a small resync each toggle"), which the [DRAIN_LOW,DRAIN_HIGH] deadband was specifically built to prevent; the bias was large enough to reproduce it. Fix: replaced the truncated estimate with Q16.16 fractional accumulation across StartInRequest calls (new m_inNomAccum member), mirroring m_fbAccum's already-proven OUT-pacing pattern -- the negotiated rate is carried as an exact fraction and accumulated per microframe, so the long-run average claimed matches the true rate exactly (zero steady-state bias). AudioSystem.cpp's drain hysteresis itself was confirmed untouched by any commit this session (git log verified empty) -- this fix corrects the bias overwhelming it, not the deadband. Build-verified (kernel7l.img 1097316 bytes) and flashed to E:\kernel7l.img.

## 2026-07-08e — Fix US-2x2 buzz: size UAC2 IN batching by nominal rate, not protocol max

- fix: patches/usbaudiodevice.cpp StartInRequest's UAC2 batching still buzzed after 2026-07-08d because m_nInSubmitBytes assumed every batched packet delivered the protocol's declared MAXIMUM size (usMaxPacket) -- but a real async/adaptive UAC2 IN device's actual per-microframe delivery legitimately varies, so InCompletion over-read into stale buffer tail whenever a microframe delivered less, on up to 8 opportunities per URB completion (US-2x2, confirmed working perfectly pre-session with only occasional underrun -- buzz was purely introduced by this session's batching work). An intermediate design (min(GetResultLength(), submitted)) was caught before shipping: GetResultLength() clamps to roughly ONE packet's worth for a multi-packet URB (confirmed via the vendored DWHCI driver's TransactionComplete/GetResultLen source), so min() silently reduces to trusting that severe truncation in the common case, defeating the AIR 192|4 fix entirely. Real fix: decouple the DECLARED iso packet size (stays at the endpoint max, required so hardware accepts full-rate delivery without erroring) from the TRUSTED byte count, now sized at the NOMINAL bytes/microframe from the negotiated rate (m_uRate/8000, the same math already used for OUT feedback pacing) -- bounds worst-case over-read to real clock-tolerance jitter instead of a full packet. Why: user explicitly chose to keep pursuing a universal (device-blind) fix over reverting the AIR 192|4 work, after three consecutive regressions on the same path. Build-verified (kernel7l.img 1097316 bytes) and flashed to E:\kernel7l.img.

## 2026-07-08d — Fix buzz persisting after 2026-07-08c: UAC1 needs GetResultLength(), not a constant

- fix: patches/usbaudiodevice.cpp InCompletion's 2026-07-08c fix (below) switched ALL devices to a constant m_nInSubmitBytes[slot]=usMaxPacket, but GetResultLength() was only broken for UAC2's multi-packet path -- for UAC1 (UCA222, single-packet URB), GetResultLength() correctly reports the TRUE per-frame byte count, which VARIES frame-to-frame on a real synchronous full-speed device (packet-size alternation to average a non-integer samples/frame rate). Forcing a constant usMaxPacket every completion made InCompletion read stale buffer tail on every frame shorter than the max -- the buzz persisted because the prior fix only addressed UAC2, and introduced an equivalent bug on UAC1. Fix: nSamples source gated on m_bUAC2 -- UAC2 keeps m_nInSubmitBytes[slot] (genuinely necessary there), UAC1 reverts to plain GetResultLength() (the exact pre-session behavior, proven correct across this project's history). Why: user reported the buzz was STILL present after flashing 2026-07-08c's fix -- re-diagnosed from scratch (not assuming the prior analysis was complete) by diffing against the pre-session code and re-verifying the DWHCI driver's single-packet path specifically. Build-verified (kernel7l.img 1097188 bytes) and flashed to E:\kernel7l.img. OPEN QUESTION flagged to the user: this session could not confirm which physical device (UCA222/US-2x2/AIR 192|4) was connected during either buzz report -- this fix targets the UAC1/UCA222 mechanism specifically; if a UAC2 device is still buzzing, the next candidate is the UAC2 path's accepted short-packet-blindness tradeoff (m_nInSubmitBytes assumes all batched packets are full-size, never verified against real hardware).

## 2026-07-08c — Fix continuous distortion/buzz: vendored USB stack's multi-packet GetResultLength() is broken

- fix: patches/usbaudiodevice.cpp InCompletion computed nSamples from pURB->GetResultLength(), which the vendored Circle DWHCI driver (lib/usb/dwhcixferstagedata.cpp) reports WRONG for multi-packet iso URBs -- it clamps the correctly-accumulated total transfer size against a per-packet field that by stage-complete only holds the LAST packet's declared size, truncating the reported length to roughly 1/8th of the real byte count on the UAC2 N=8 microframe-batching path from the AIR 192|4 fix. The buffer itself was fully and correctly written; only the reported length was wrong, starving the IN ring on every completion and forcing continuous resync/repeat-sample fallback -- audible as persistent distortion/buzz, not clean silence. Fix: StartInRequest now records what it actually submitted (m_nInSubmitBytes[slot]); InCompletion parses that instead of trusting GetResultLength(). UAC1 (UCA222) is unaffected -- its single-packet GetResultLength() was never wrong, and now matches m_nInSubmitBytes exactly. Why: the user flashed the stack-overflow fix and reported continuous buzz/distortion -- root-caused by reading the vendored DWHCI transfer-completion source directly (not guesswork). Build-verified; SD card was unmounted on the dev host so flashing to E: is pending the user reconnecting it.

## 2026-07-08b — Fix total-silence regression from the IN batching commit

- fix: patches/usbaudiodevice.cpp InCompletion allocated its left_buf/right_buf sample arrays unconditionally at cap=USB_AUDIO_INBUF_BYTES/4=512 samples (2048 bytes) ON THE STACK, for every device including UCA222/US-2x2, in the USB completion-handler/ISR context -- an 8x stack-frame blowup vs the prior 256-byte footprint that overran the completion-handler stack and crashed the whole system, causing total loss of ALL sound on every device after flashing the previous commit. Fix: left_buf/right_buf are now static per-slot arrays instead of stack allocations; UAC1 sample values/counts are unchanged, only the storage class moved. Why: the user flashed the AIR 192|4 fix and reported total silence -- root-caused via direct code read (not guesswork) to the stack-sizing oversight in the prior commit. Build-verified and flashed to E:\kernel7l.img; live hardware confirmation deferred to the user.

## 2026-07-08 — UAC2 IN multi-packet-URB batching (fix AIR 192|4 capture underruns)

- fix: patches/usbaudiodevice.cpp StartInRequest batched N=8 microframes/URB and double-buffered IN URBs for UAC2 devices, mirroring the earlier OUT-side Tascam fix on the capture path. The single-packet/single-URB IN path missed microframes under per-URB re-arm overhead on high-bandwidth UAC2 devices (AIR 192|4) the same way the OUT path did before that fix; the lower-bandwidth US-2x2/UCA222 IN paths never exercised it. USB_AUDIO_INBUF_BYTES sized to 2048 (IN is device-paced, so packet size can never shrink to fit a smaller buffer the way host-paced OUT could). Added g_audioInMaxGapUs/g_audioInSubmitFail telemetry surfaced via the :4445 UAUD verb. Why: user reported the AIR 192|4 glitching while US-2x2/UCA222 worked perfectly, and asked whether newer high-bandwidth interfaces could be supported without breaking the existing ones. UAC1 (UCA222) path is untouched; UAC2 (US-2x2) gets the same additive double-buffer+batching the OUT side already proved safe. Build-verified (kernel7l.img links clean); live hardware confirmation deferred to the user (no physical interfaces attached to the dev session).

## 2026-06-10b — Sampler mode (buttons 65/66) + filters moved to end of chain

- feat: patches/sampler.h adds an independent sampler subsystem. Button 65 HELD records one shared "chromatic" sample (auto-clips leading/trailing silence on release); the 25 keyboard keys then play it pitched chromatically (middle C = note 60 = original speed), polyphonically. Button 66 HELD = drum mode: holding a keyboard key records into THAT key's own drum slot (auto-clip on release); a loaded drum slot plays at original pitch as a one-shot and overrides the chromatic sample on that key. The sampler is independent of the looper (touches no clip/masterPhase state) and mixes INTO m_input_buffer BEFORE the pitch/effects/microrepeat/filter chain, so samples get all effects and are recordable into a loop (under SHIFT they fold into a recording loop). Capture reads the DRY mic snapshot taken before the sampler render, so a sample never records itself or the loops. MIDI events cross from the ISR to Core 1 via a lock-free SPSC ring; buffers are touched only on Core 1. Click-free voice attack/release ramps + trim-edge fades. Why: the user asked for a hold-to-record sampler (65 chromatic, 66 per-key drums) that plays into the effected input and leaves the looper working. When the sampler has content the channel-1 keyboard plays it (live-pitch keyboard transpose suppressed; live pitch still reachable via mod-wheel/CC52). Witnessed by scripts/test-sampler.cpp (auto-trim, chromatic resample ratio, drum one-shot, polyphony, voice-steal, click-free, overrun clamp, no-content no-op — ALL PASS). Mapped in midiMap.h as MA_SAMPLER_RECORD/MA_SAMPLER_DRUM_MODE/MA_SAMPLER_KEY; surfaced in the :4445 TIME verb as sampRec/drumMode/sampLen/drumLoaded/voices.
- feat: the HP/LP filters now run at the END of the effects chain — AFTER the microrepeat glitch — instead of before the delay/reverb sends. apcEffectsProcessor split into processFilters (HP/LP) and processSends (delay/reverb); loopMachine runs sends in the old slot, then microrepeat, then filters before cbWriteBlock. Why: the user wants the filter to act on the stuttered/glitched signal. At default knobs (HP 0, LP 1) processFilters is a byte-exact pass-through, so the move is inaudible until a filter knob moves. Witnessed by scripts/test-filter-order.cpp (default-knob parity + order-observable-when-engaged — ALL PASS).
- Clean firmware build kernel7l.img 1065932B (full app rebuild after the apcKey25.h layout change); flashed to SD E:. Regression suite (test-microrepeat / test-monitor-route / test-pause-mute / test-resume-phase / test-first-loop-region / test-consec-lock / test-arrangement-forget / test-midi-config-parity) still ALL PASS.

## 2026-06-10 — Synced latch microrepeat (beat-repeat / stutter) on notes 82-86

- feat: patches/microRepeat.h adds a synced latch-based beat-repeat/stutter stage. Holding note 82/83/84/85/86 repeats a 1, 1/2, 1/4, 1/8, 1/16-beat slice of the full mix (live input + all loops) in sync with the master beat grid; releasing resumes the exact position playback would have had (the repeat is ephemeral — it never touches the loop play heads or masterPhase). It lives in the effects layer applied to m_input_buffer before cbWriteBlock, so under SHIFT (loops folded into the effect source) the stutter is recorded into a loop. Note 84 overrides the FORMAT button while held. Why: the user asked for synced latch microrepeats that repeat all loops without disturbing playback position and that record-through under SHIFT. Witnessed by scripts/test-microrepeat.cpp (slice-per-div, verbatim replay, position-passthrough, click-free, div-change, no-master guard, buffer clamp — ALL PASS), the unchanged regression suite, and a clean firmware build (kernel7l.img 1062092B). Mapped in midiMap.h as MA_MICROREPEAT (controller-agnostic); surfaced in the :4445 TIME verb as microRep=<div>.

## 2026-06-07 — SHIFT routes the loops INTO the effects (was: loops went silent)

- fix: holding SHIFT used to gate the loop output to silence (only the live mic input passed through effects), so the loops went silent. SHIFT now folds the running loop output INTO the effect source before the pitch/effects stages and before the record buffer, so the loops become effected and recordable; the dry loop output is suppressed complementarily (dry = 1 - fold) so loops are heard once, through the effects, with no loudness jump. Why: the user reported "when we pressed shift, the loops went silent instead of playing as the input into the effects" — the loops are supposed to become the input to the effects while SHIFT is held. The track audio + Link block were moved before the pitch/effects stages to make this possible (phase-neutral). Witnessed by scripts/test-monitor-route.cpp (ALL PASS, replaces the obsolete test-monitor-gate.cpp) and a clean firmware build (kernel7l.img 1061196B).

## 2026-06-06 — Pause is a pure mute; play head never stops

- feat: pausing a recorded loop now MUTES it (click-free m_pauseGain ramp) instead of resetting the play head — the clip stays CS_PLAYING/CS_LOOPING and keeps advancing phase-locked to the master, so resume is position-identical and rapid mute/unmute is instant. Once a loop is recorded it never stops playing the same way it started. Why: the user's rule that pause must only mute, never change phrase position. getTrackState reports a paused clip as STOPPED so the pad still blinks yellow and a tap resumes it; a real stop (stopImmediate/abort) and a fresh record clear the pause latch. Witnessed by scripts/test-pause-mute.cpp (zero drift across any pause duration, click-free, rapid-toggle-safe — ALL PASS), unchanged phase/region regression tests, and a clean firmware build (kernel7l.img 1061004B).

## 2026-06-05 — Arrangement memory tracks erase; MIDI mapping is data (controller-agnostic)

- feat: erasing a looper forgets it from arrangements; empty arrangement auto-deletes (LED dark) — `_forgetLooperFromPresets`/`_forgetAllPresets` (apcKey25Notes.cpp) drop an erased looper's bit from every preset mask and delete any arrangement whose mask reaches 0, so its pad goes dark exactly when its last member is gone. Why: an arrangement that still references deleted loopers is stale and lit a pad for a recall that does nothing. Witnessed by scripts/test-arrangement-forget.cpp (17 checks ALL PASS).
- feat: atomic controller-agnostic MIDI map (midiMap.h) — every in/out MIDI control is an independent record (`MidiInputMap` keyed on status/channel/data1-range -> logical action + value-mode; `MidiOutputMap` state -> note+velocity). Swap `g_activeProfile` to remap for another controller without code changes; value modes cover absolute and relative/endless encoders. Why: the request to make MIDI remappable for other controllers without forcing the APC25 style.
- feat: LED output is live config-driven — `_updateGridLeds` and the live-engage LED resolve velocities via `midiMapResolveOutput(g_activeProfile)`; a missing state degrades to OFF (no stuck LED). The default APC25 profile is byte-identical to the former hard-coded behavior (no-operational-change invariant). Witnessed by scripts/test-midi-config-parity.cpp (full event matrix, ALL PASS) and a clean firmware build (kernel7l.img 1060748B).

## 2026-05-12b — Residual scan: doc drift + cross-core dispatch observability

- doc: AGENTS.md — removed stale CORE_FOR_AUDIO_SYSTEM=0 claim (doUpdate no longer runs in USB ISR), spelled out 4-core partition + IPC primitives in Audio architecture, located telemetry drain on Core 2 in Logging section.
- feat: TELEM_DISPATCH_FULL telem code (audioTelemetry.h) — coreDispatchPush emits on overflow so Core 1 backpressure surfaces in the same event ring as IN/OUT underruns.
- feat: audio.cpp stat summary line gained `disp+N` field (delta of g_dispatchDropped) under ARM_ALLOW_MULTI_CORE — periodic summary now covers cross-core handoff health.

## 2026-05-12 — 4-core re-architecture: hard-RT dispatch / DSP worker / control plane / reserved idle

- feat: patches/coreDispatch.{h,cpp} — 64-slot SPSC ring + DSB+SEV/WFE primitive. Producer-side allocation-free; ISR-safe. New g_dispatchDropped counter for backpressure observability.
- feat: patches/paramSnapshot.{h,cpp} — double-buffered atomic-swap publish for control→DSP shared state (liveEngaged, livePitchSemitones, formantNorm, linkSynced, linkBPM, masterLoopBlocks). Single writer (Core 2), multi-reader (Core 1 audio path), no torn reads.
- refactor: patches/multicore.cpp — Core 0 = hard-RT dispatch (USB ISRs + reboot poll), Core 1 = DSP worker (WFE-blocked drain → AudioSystem::doUpdate), Core 2 = control plane (USB plug-and-play, Net.Process, usbMidiProcess, audio.cpp::loop, linkProcess, WiFi DHCP, scheduler yield, APC update), Core 3 = reserved idle (WFE forever — claimable later). All cores 1-3 were while(1); spinning before this commit.
- refactor: patches/AudioSystem.cpp::startUpdate — replaced inline doUpdate (which ran the entire signalsmith STFT in USB ISR context on Core 0) with coreDispatchPush(DISPATCH_AUDIO). DSP now runs on Core 1 outside any ISR.
- refactor: patches/kernel.cpp::Run — Core 0 main loop trimmed to socket poll + WFE; control-plane calls (USBHCI/AudioGadget plug-poll, Net.Process, usbMidiProcess, loop, wlanDhcp, linkProcess, Scheduler.Yield) hoisted into patches/kernel_run.cpp::coreControlPlaneTick run on Core 2.
- refactor: loopMachine.cpp::update — pTheAPC->getDebugState() + linkIsSynced/linkGetBPM reads inside DSP path replaced with paramSnapshotLoad(). pLivePitchWrapper->setPitchScale moved to Core 1 to keep signalsmith state single-writer.
- refactor: apcKey25.cpp + apcKey25Transpose.cpp — _applyLivePitch and update() now publish to paramSnapshot instead of calling setPitchScale directly. Cross-core safe.
- handshake: g_coreAudioReady flag + WFE/SEV — Cores 1+2 spin in WFE during Core 0's hardware init, released after setup() returns.
- invariants preserved: LOOPER_LOG queued path stays no-op; DWC2 OTG iso software toggle untouched; UCA222 IN/OTG ring deadbands untouched; phase-lock invariant untouched; per-clip RubberBandWrapper memory layout untouched; CUSBCDCGadget link order untouched.
- IPC discipline: SPSC lock-free rings only, no mutexes in audio path, atomic snapshot for shared state. SEV from producers, WFE on consumers — no busy-spin idle.

## 2026-04-20b — Dual-engine pitch shift: time-domain octaver + signalsmith

- feat: patches/RubberBandWrapper.h — when pitch scale is exact ±12 (0.5x or 2.0x ±1%), route through time-domain granular octaver (2-tap crossfading delay line, 512-sample Hann crossfade, ~3ms latency). All other ratios route to signalsmith. Clean guitar→bass with low latency; continuous bends + formant still use signalsmith.
- tune: signalsmith window 384/96 → 192/64 (~4ms latency). Clean low-octave no longer depends on signalsmith FFT resolution.

## 2026-04-20 — UCA222 min-latency tuning

- tune: patches/input_usb.cpp — IN_TARGET_LAG 256→128 (5.3ms→2.7ms), IN_DEADBAND 128→64. Halves UCA IN ring buffering. Resyncs may rise under clock drift; Q16 fractional read still absorbs steady-state drift inaudibly.
- tune: patches/RubberBandWrapper.h — signalsmith-stretch configure 512/192 → 384/96. Pitch-shift latency ~10.6ms → 8ms. Window chosen to resolve low-E fundamental for clean -12 octave (guitar→bass); interval 96 keeps formant shift smooth + responsive.
- docs: AGENTS.md latency figures updated to match.

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
