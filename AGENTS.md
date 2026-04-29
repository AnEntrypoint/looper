# AGENTS.md

Project notes for agents working on Lanmower's Looper. Supersedes CLAUDE.md — CLAUDE.md is now just a pointer here.

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

## Audio architecture

- **USB audio runs at 48000Hz** (UCA222 native). `AUDIO_SAMPLE_RATE=44100` in AudioTypes.h is the internal system rate.
- **AUDIO_BLOCK_SAMPLES=64** for low latency (~1.3ms at 48kHz).
- **Ring buffers decouple USB from audio chain.** USB IN writes to a 512-sample SPSC ring (`patches/input_usb.cpp`). USB OUT to a 2048-sample SPSC ring (`patches/output_usb.cpp`).
- **Drift correction: Q16 fractional read + linear interpolation.** Both IN and OTG-tap read positions are fractional (u32 int + u16 frac). Rate step = `FRAC_ONE + (band_dev * FRAC_ONE) / RATE_GAIN` with wide deadband (IN target=128, DB=64 — tuned for min UCA222 latency; OTG target=768, DB=192). Inaudible on tonal content. Catastrophic-deviation clause resets read position. Rate clamped to ±256/16384 (≈1.5%).
- **Underrun fallback repeats last sample**, not zero. Eliminates clicks on brief starvation.
- **Watchdog**: if USB IN hasn't delivered in >5ms, `loop()` force-fires `AudioSystem::startUpdate()`.
- **`startUpdate()` is driven by USB IN ring position**, not a timer.
- **`CORE_FOR_AUDIO_SYSTEM=0`** — `doUpdate()` runs directly inside USB IN completion handler (interrupt context). No multicore IPI.
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
- **Observability now**: ISR-safe lock-free event ring (`patches/audioTelemetry.{h,cpp}`, 256-slot SPSC). ISR sites push `(code, ticks, arg)` triplets — no allocs, no UDP. `audio.cpp::loop()` (main thread) drains up to 32 events per call and emits one `CLogger::Write` per event; producers also bump counters that a 0.5Hz summary line reports as deltas (only when nonzero). Event codes: `IN_UR`, `IN_RS`, `OUT_UR`, `OTG_RS`, `WD`, `LAG`. Drops counted in `g_telemDropped`.

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
- **Pitch shift path (RubberBandWrapper.h)**: dual-engine.
  - When `m_pitchScale ≈ 0.5 or 2.0` (within ±1%): **time-domain granular octaver** (2-tap crossfading delay-line, OCT_GRAIN=512, Hann crossfade). ~3ms latency, clean on guitar→bass.
  - All other ratios: **signalsmith-stretch** at blockSamples=192, intervalSamples=64. ~4ms latency. Used for continuous CC bends and formant mangling.
  - Octaver activation hysteresis is ±1% so mod-wheel near -12 snaps to clean mode.
- **Pitch scale**: `_applyLivePitch()` calls `setPitchScale(pow(2, semitones / 12))`.
- **CC1 (mod wheel)**: deadzone 59-69 disengages. Outside: ±6 semitones by `((data2 - 64) * 6 / 63)`.
- **CC52**: linear 0-127 → ±6 semitones.
- **Channel 1 note-on (0x91)**: toggles engage; pitch = note - 60.
- **Channel 2 note-on (0x92)**: pitch = note - 60, always engages.

## Planned architecture (not yet implemented)

- **3-minute rolling recording buffer**: continuous circular fill, record marks in/out, deep-copies into clip. Eliminates start/stop clicks.
- **Independent track controls**: per-track stop instead of `LOOP_COMMAND_STOP_IMMEDIATE` (which stops all).
