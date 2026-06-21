# AGENTS.md

Project notes for agents working on Lanmower's Looper. Supersedes CLAUDE.md (CLAUDE.md is just a pointer here).

This file is the LEAN index of HARD RULES + control mappings. The detailed,
load-bearing knowledge for each subsystem now lives in recall memory (portable,
recall-grounded) — query `recall` with the subsystem name before working on it.
Each section below points to its memory. Do NOT re-grow this file with prose that
belongs in a memory; add the memory and link it here instead.

## HARD BUILD RULES (get these wrong and you ship broken firmware)

- **`CHECK_DEPS=0` disables header-dependency tracking.** The firmware make (and
  `build-local.ps1`) only rebuilds a `.o` when its own `.cpp` is newer — editing a
  HEADER does NOT recompile the `.o` files that `#include` it. After ANY header
  edit you MUST `rm` the dependent objects (e.g. `loopMachine.o audio.o`, or all
  `*.o`) before `make`, or you deploy STALE engine code while the source looks
  correct. After `rm *.o` also regenerate `wlan_firmware.o` (`arm-none-eabi-as`
  from `$TEMP/wlan_firmware.S` — `rm` deletes it, no make rule rebuilds it). A
  kernel-size change confirms the engine objects actually recompiled.
- **Use `AARCH=32` for RASPPI=4** (phorton1/circle casts pointers to u32) →
  produces `kernel7l.img`. No `AARCH=64`.
- **Local build = `scripts\build-local.ps1`** (mirror of CI `.github/workflows/build.yml`).
  Run it with **`powershell.exe`, NOT `pwsh`** (pwsh is not installed). Needs
  `scoop install gcc-arm-none-eabi`, `python`, `git`. Robocopies the repo into
  `~/.looper-build/circle/_prh/_apps/Looper`, applies patches, builds. If the
  whole-script run trips on the final copy step, the app `make` itself still works
  — run `make RASPPI=4 AARCH=32 ARM_ALLOW_MULTI_CORE=1 CHECK_DEPS=0 -j4` directly
  in the build dir to surface real compile errors and produce `kernel7l.img`.
- **App-internal patches** (`patches/kernel.cpp`, `kernel_run.cpp`, `multicore.cpp`,
  `coreDispatch.*`, `coreBusy.*`, `paramSnapshot.*`, `main.cpp`,
  `gamepadInput.{h,cpp}`, `gamepadState.h`) must be copied next to the Makefile
  (the `appInternal` list in `build-local.ps1` + the `cp` lines in `build.yml`) AND
  the `.o` named in the Makefile `OBJS`. Files at repo root (`audio.cpp`,
  `apcKey25*.cpp`, `usbMidi.cpp`, …) are robocopied automatically.
- **Linux CI symlink:** `circle-prh/audio/bcm_pcm.cpp` includes `BCM_PCM.h`
  (uppercase) but the file is `bcm_pcm.h` — `ln -sf bcm_pcm.h .../BCM_PCM.h`.
- **`dev-server.js`** (`tftp-server.js`) = the all-in-one dev host (TFTP/DHCP/
  syslog/GitHub auto-update). Run `node dev-server.js` as admin. Use
  `LOOPER_NO_AUTO_UPDATE=1` when testing a LOCAL kernel (auto-update overwrites
  the tftproot kernel with the latest GitHub release and reboots the Pi).

Full build caveats (OTG gadget, miniuart RASPPI<4 guards, UAC1/UAC2 paths, USB
patch lists): `recall` **looper-build-caveats**, **looper-otg-and-tests**.

## HARD ARCHITECTURE RULES

- **Never do blocking I/O in USB completion handlers** (CLogger/syslog UDP in a
  completion causes periodic audio gaps). Observability is the ISR-safe lock-free
  event ring (`patches/audioTelemetry.{h,cpp}`) drained on Core 2.
- **Cross-core single-writer discipline.** Core 0 = hard-RT USB ISR dispatch;
  Core 1 = DSP worker (whole audio graph); Core 2 = control plane (USB PnP,
  net/sockets, MIDI, gamepad, APC update, Link, telemetry); Core 3 = idle. Control
  → DSP params flow ONLY through `paramSnapshot` (double-buffered atomic-swap,
  single writer Core 2). ISR producers push into SPSC rings drained on Core 2;
  never call Core-2-owned objects (apcKey25/effects) from an ISR.
- Audio: USB 48000Hz, internal `AUDIO_SAMPLE_RATE=44100`, `AUDIO_BLOCK_SAMPLES=64`.
  Full detail: `recall` **looper-audio-architecture**.

## OPERATOR CONTROL MAPPING (must not change without intent — load-bearing UI)

APC Key 25 grid (NUM_TRACKS=20, NUM_LAYERS=1):
- **Cols 0-1** (10 pads) = preset slots (`_presetFromPad = row*2 + col`).
- **Cols 2-5** (20 pads) = loopers (`_looperFromPad = row*4 + (col-2)`).
- **Cols 6-7** = blanked.

Looper pad gestures: empty tap → arm record (on press); recording tap → finish+
play; playing tap → pause (= MUTE, head keeps advancing); paused tap → resume
(position-identical); long-hold ≥1000ms → erase (LED off). SHIFT-hold = route the
loops INTO the live effects (not mute). Microrepeat = held notes 82-86 (1, 1/2,
1/4, 1/8, 1/16 beat glitch). Sampler = buttons 65 (chromatic) / 66 (drum). Live
pitch = CC1 modwheel / CC52 / ch1+ch2 note-on. Effects CCs: reverb 48, delay 49,
time 50, formant 53; filters: HP 51, res 54, LP 55; transpose 52.
Command bases (`commonDefines.h`): TRACK 0x20, STOP_TRACK 0x40, ERASE_TRACK 0x60,
CLEAR_LAYER 0xA0, HALVE 0xC0, DOUBLE 0xE0.

Detail: `recall` **looper-grid-presets-ui**, **looper-microrepeat-full**,
**looper-sampler**, **looper-live-pitch-formant**, **looper-midimap-profile**.

## USB GAMEPAD CONTROL (`patches/gamepadInput.{h,cpp}`, `gamepadState.h`)

A generic USB HID gamepad (Circle `CUSBGamePadStandardDevice`, registers as
`upad1..4`) drives the SAME controls as the APC by synthesizing APC MIDI bytes
through `pTheAPC->handleMidi` — NO new DSP, every mapping reuses a tested path.

| Control            | Maps to                              |
|--------------------|--------------------------------------|
| Z-axis             | transpose -12..+12 (CC52, deadzone)  |
| Z-rotation (Rz)    | formant shift (CC53, deadzone)       |
| X-axis vertical    | down = lowpass (CC55) / up = highpass (CC51), deadzone |
| X-axis horizontal  | resonance (CC54, deadzone)           |
| LT / RT triggers   | delay amount (CC49) / delay time (CC50) |
| R1 bumper          | reverb MAX (CC48, momentary)         |
| L1 bumper          | SHIFT                                |
| remaining buttons  | loopers (full record/finish/pause/erase gesture) |
| HAT / dpad         | glitch microRepeat speeds (notes 82-85) |

Cross-core: the status handler (USB ISR, Core 0) only SNAPSHOTS the state into a
coalescing latest-state buffer; `gamepadProcessTick()` (Core 2, in
`coreControlPlaneTick`) diffs vs last-applied and emits. Axis index is HID
report-descriptor declaration order (NOT guaranteed across pads) → named tunable
`GP_AXIS_*` constants (`gamepadInput.h`); a different pad is a one-line fix.
`:4445 GPAD` verb. Host test `scripts/test-gamepad.cpp` ALL PASS.
**HARDWARE-VALIDATION PENDING** (real-pad axis ordering by ear). Detail: `recall`
**looper-gamepad-control**.

## SUBSYSTEM POINTERS (detail lives in recall memory)

- Timing / latency-backdated record / continuous buffer / Link train-on-first-loop
  → `recall` **looper-timing-backdate**
- Clip state machine (9-state FSM, first-loop exact-region, pause=mute,
  consecutive-loop quant grid) → `recall` **looper-clip-state-machine**
- Clip tempo sync = VARISPEED resample (not time-stretch) → `recall`
  **looper-clip-varispeed**
- USB MIDI host (per-device OUT cap, async OUT, LED coalesce) → `recall`
  **looper-usb-midi-host**
- MIDI mapping is data (`midiMap.h` atomic profile) → `recall`
  **looper-midimap-profile**
- Continuous USB ring-WAV recorder → `recall` **looper-usbwav-recorder**
- WiFi + Ableton Link (opt-in, full protocol, unicast-RX wall, LCLK/LTMP,
  topology) → `recall` **looper-wlan-link-full** (+ existing memories for
  esp32-ticker, dhcp-broadcast-flag, wlan-gated, UAC2/USB-enum)
- OTG gadget audio + test coverage → `recall` **looper-otg-and-tests**

## Logging

`LOOPER_LOG(...)` is currently a no-op (`Looper.h` #else). Do NOT re-enable the
queued path without making `LogUpdate()` ISR-safe (`new logString_t`/`new CString`
are not — they corrupt the heap under audio-ISR load). Observability = the
`:4445` UDP verbs + the ISR-safe `audioTelemetry` event ring.

## Planned (not yet implemented)

- 3-minute rolling recording buffer: continuous fill, mark in/out, deep-copy into
  clip (eliminates start/stop clicks). [partially realized by the continuous
  buffer — see **looper-timing-backdate**.]

@.gm/next-step.md
