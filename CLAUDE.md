# CLAUDE.md

## Non-obvious build caveats

- `circle-prh/audio/bcm_pcm.cpp` includes `"BCM_PCM.h"` (uppercase) but the file is `bcm_pcm.h` (lowercase). Linux CI requires a symlink: `ln -sf bcm_pcm.h circle/_prh/audio/BCM_PCM.h` before building.
- `circle-prh` miniuart uses `ARM_IRQ_AUX` and `ARM_GPIO_GPPUD` which are absent in phorton1/circle for RASPPI=4 (circle uses GIC-400 for rPi4). Fixed via `patches/miniuart.cpp` with `#if RASPPI < 4` guards.
- `phorton1/circle` does not support `AARCH=64` — `alloc.cpp` casts pointers to `u32` which fails in 64-bit. Use `AARCH=32` for RASPPI=4; produces `kernel7l.img`.
- Default build (no defines) uses CS42448 octo audio path. `LOOPER_USB_AUDIO=1` requires `AudioInputUSB`/`AudioOutputUSB` which do not exist in `circle-prh/audio` — must be added before that build target works.
- On Linux CI, `#include <audio\\Audio.h>` with backslash fails. The include in `audio.cpp` must use forward slash: `<audio/Audio.h>`.
- CI produces `kernel7l.img` for RASPPI=4 AARCH=32 (rPi 4, 32-bit). File naming per circle Rules.mk: RASPPI=2→kernel7, RASPPI=3 32-bit→kernel8-32, RASPPI=4 32-bit→kernel7l.
- `CUSBCDCGadget` exposes no `GetSerial()` — `m_pInterface` is private and created lazily by `CreateDevice()` (called from `Initialize()`). Access the serial device via `CDeviceNameService::Get()->GetDevice("utty1", FALSE)` after `Initialize()` returns. Device is `nullptr` until USB host enumerates the gadget.
- `CUSBCDCGadget` lives in `lib/usb/gadget/` as a separate library (`libusbgadget.a`). Must build `make -C circle/lib/usb/gadget` and link `libusbgadget.a` before `libusb.a` in LIBS.
- On rPi4, `CUSBHCIDevice` is `CXHCIDevice` (xHCI, USB-A ports). `CUSBCDCGadget` uses `CDWUSBGadget` (DWC OTG, USB-C port). They are on separate controllers — both can coexist in the same kernel.
- APC Key 25 USB audio input (`uaudio1-2`) only supports 48000Hz, not 44100Hz. With `LOOPER_USB_AUDIO=1`, `CUSBAudioDevice::InCompletion` receives zero-length URBs and never calls `inHandler`, so `AudioSystem::startUpdate()` is never called and `loopMachine::update()` never runs. Fix: `loop()` drives `AudioSystem::startUpdate()` via `CTimer::GetClockTicks()` at `AUDIO_BLOCK_SAMPLES/AUDIO_SAMPLE_RATE` Hz (`#if USE_USB_AUDIO` guarded).
- `CUSBAudioDevice::Configure()` seeds both `StartInRequest()` and `StartOutRequest()`. However `OutCompletion` only re-submitted when `m_pOutHandler != null` — since `Configure()` runs during USBHCI init (before `AudioOutputUSB::start()` registers the handler), the first completion fires with no handler and the chain dies permanently. Fix: always call `StartOutRequest()` in `OutCompletion`; send silence via `memset(m_OutBuf, 0, ...)` when no handler is registered (`patches/usbaudiodevice.cpp`).
- `tftp-server.js` is the single all-in-one server process: TFTP (port 69), DHCP (port 67), syslog (port 514), and GitHub release auto-update. `dhcp-server.js` and `syslog-listener.js` still exist as standalone scripts but are not forked by `dev-server.js` — only `tftp-server.js` is. Run `node dev-server.js` (as admin, ports 67/69/514 require elevation).

## Audio architecture decisions

- **USB audio runs at 48000Hz** (UCA222 native rate). `AUDIO_SAMPLE_RATE=44100` in AudioTypes.h is the internal system rate — there is a mismatch that may need sample-rate conversion later.
- **AUDIO_BLOCK_SAMPLES=64** for minimum latency (~1.3ms at 48kHz).
- **Ring buffers decouple USB from audio processing.** USB IN completion writes to a 256-sample SPSC ring (`input_usb.cpp`). USB OUT completion reads from a 256-sample SPSC ring (`output_usb.cpp`). Audio chain never touches USB directly.
- **`startUpdate()` is driven by USB IN ring position**, not a timer. When the ring write position crosses an `AUDIO_BLOCK_SAMPLES` boundary, `startUpdate()` fires. This gives hardware-precise timing from USB frame completions (1ms/frame, 48 samples/frame at 48kHz).
- **`CORE_FOR_AUDIO_SYSTEM=0`** — `doUpdate()` runs directly inside the USB IN completion handler (interrupt context). No multicore IPI. The stub kernel has no `CCoreTask` so multicore audio would be undefined behavior.
- **Never do blocking I/O in USB completion handlers.** `CLogger::Write` (syslog UDP) in `InCompletion`/`OutCompletion` causes 1-second periodic audio gaps. All logging removed from audio interrupt path.
- **Isochronous USB requests require `AddIsoPacket(maxPacketSize)`** before `SubmitAsyncRequest`. Without it, Circle's xHCI asserts `m_nNumIsoPackets > 0`.
- **UCA222 IN and OUT are on separate USB device nodes** (different hub ports). Each gets its own `CUSBAudioDevice` instance. `s_pThis` = IN device, `s_pOut` = OUT device. `AudioInputUSB` uses `Get()`, `AudioOutputUSB` uses `GetOut()`.
- **Factory intercepts only `int1-2-0`** (audio streaming), not `int1-1-0` (audio control). Removing the AudioControl device prevents Circle's built-in `CUSBAudioStreamingDevice` from stealing the interface.
- **5 looper tracks** mapped to APC Key 25's 5 rows (notes 0-39). `LOOPER_NUM_TRACKS=5` in `commonDefines.h`.
- **VU meter on APC column 8** (rightmost). Peak level tracked in `inHandler` (no blocking), displayed as 5-LED green/red meter at 30fps from `_updateGridLeds`.

## Clip state machine

- **`ClipState` enum** (9 mutually-exclusive values) replaces the former 7 `CLIP_STATE_*` bitmask defines. Impossible combinations (e.g. `RECORD_IN|PLAY_MAIN`) are no longer representable.
- **`CS_RECORDING_TAIL` vs `CS_FINISHING`** encode whether the clip will auto-play after recording ends — eliminating the former `m_pendingPlay` boolean flag.
- **`CS_LOOPING`** encodes the former `PLAY_MAIN|PLAY_END` bitmask combination. `update()` reads both the main block pointer and fade block pointer in this state.
- **Phase alignment**: `_startPlaying()` sets `m_play_block = (masterPhase + 1) % numBlocks` because `update()` increments masterPhase before reading it in the same tick.
- **`loopClip.cpp` is split into three files** at the 200-line limit: `loopClip.cpp` (init/transitions), `loopClipUpdate.cpp` (per-buffer audio processing), `loopClipState.cpp` (state name/quantize/updateState).

## Planned architecture (not yet implemented)

- **3-minute rolling recording buffer**: Audio input continuously fills a circular buffer. "Record" marks a start point, second press marks end and deep-copies the segment into a clip. Eliminates recording start/stop clicks.
- **Independent track controls**: Stopping one track should not stop others. The current `LOOP_COMMAND_STOP_IMMEDIATE` stops all tracks — needs per-track stop.
