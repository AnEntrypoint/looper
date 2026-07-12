# AGENTS.md

Project notes for agents working on Lanmower's Looper. Supersedes CLAUDE.md (CLAUDE.md is just a pointer here).

This file is the LEAN index of HARD RULES + control mappings. The detailed,
load-bearing knowledge for each subsystem lives in recall memory (portable,
recall-grounded) — query `recall` with the subsystem name before working on it.
Each section points to its memory. Do NOT re-grow this file with prose that
belongs in a memory; add the memory and link it here instead.

## HARD BUILD RULES (get these wrong and you ship broken firmware)

- **You MUST build with `LOOPER_USB_AUDIO=1 LOOPER_OTG_AUDIO=1`.** The default
  build (no defines) compiles the **CS42448/TDM** audio path — the USB
  input/output AudioStreams are NOT in the graph, so there is NO audio in/out on
  a USB-audio rig (the long "input shows, no output" hunt). The compile pragma
  `Looper::audio.cpp using USB AUDIO` confirms the right path. Add
  `LOOPER_ENABLE_WLAN=1` for WiFi+Ableton Link (off by default → `:4445 WLAN`
  reads `wlan=disabled`). `scripts/build-local.ps1` (and `-Wlan`) pass these;
  a bare `make` does NOT. `recall` **looper-no-output-real-cause-usb-build-flag**.
- **`CHECK_DEPS=0` disables header-dependency tracking** — editing a HEADER does
  NOT recompile the `.o` files that `#include` it. After ANY header edit `rm` the
  dependent objects before `make` (or you deploy stale engine code with correct
  source). A define change (USB/WLAN) toggles compiled classes → `rm *.o`; then
  regenerate `wlan_firmware.o` (`arm-none-eabi-as` from `$TEMP/wlan_firmware.S` —
  `rm` deletes it, no make rule). A lib `.o` you `ar d`-deleted from `libaudio.a`
  must be `ar r`-re-added (make may report "up to date" and not re-archive).
- **Use `AARCH=32` for RASPPI=4** → `kernel7l.img`. No `AARCH=64`.
- **Local build = `scripts\build-local.ps1`** (run with `powershell.exe`, NOT
  `pwsh`). If the whole-script run trips on its final copy, the app `make` still
  works — run `make RASPPI=4 AARCH=32 LOOPER_USB_AUDIO=1 LOOPER_OTG_AUDIO=1
  [LOOPER_ENABLE_WLAN=1] ARM_ALLOW_MULTI_CORE=1 CHECK_DEPS=0 -j4` directly in
  `~/.looper-build/circle/_prh/_apps/Looper`.
- App-internal patches (`patches/kernel.cpp`, `kernel_run.cpp`, `multicore.cpp`,
  `coreDispatch.*`, `coreBusy.*`, `paramSnapshot.*`, `main.cpp`) are copied next
  to the Makefile and named in `OBJS`; lib patches (`output_usb.cpp`,
  `input_usb.cpp` → `libaudio.a`; `usbaudiodevice.cpp`, `usbdevicefactory.cpp` →
  `libusb.a`) are NOT app OBJS. Full caveats (symlinks, miniuart guards,
  UAC1/UAC2): `recall` **looper-build-caveats**, **looper-otg-and-tests**.

## DEPLOY / NETBOOT / SD (ops)

- The Pi can **netboot** (TFTP from `tftproot/<serial=7bec0617>/kernel7l.img`) or
  **boot from SD** (the `BOOTFS` FAT32 partition, e.g. drive `E:` on the dev host
  — holds `kernel7l.img` + `start4.elf` + `fixup4.dat` + `bcm2711-rpi-4-b.dtb` +
  `config.txt` + `cmdline.txt`). A committed BRANCH kernel does NOT reach the Pi
  until copied to the boot path. To "flash the SD": copy `dist/kernel7l.img` →
  `E:\kernel7l.img` (config/cmdline already match netboot).
- The dev-host **`tftp-server.js`** serves DHCP(67)/TFTP(69)/syslog(514). The Pi
  gets NO DHCP → cannot netboot if it is down. Start it **detached** (PowerShell
  `Start-Process`, not a Bash `&` job which the harness kills) with
  `LOOPER_NO_AUTO_UPDATE=1` (else its GitHub auto-update reflashes the stale
  release). A soft `:4444 REBOOT` does NOT reliably re-netboot — **power-cycle**
  does. `recall` **looper-deploy-netboot-and-deadgraph**, and the platform memory
  notes (pi-netboot-tftp-paths, pi-udp-probe-powershell, sd-boot-fileset).
- Probe live state with `:4445` UDP verbs via PowerShell `UdpClient` (bash
  `/dev/udp` gives false negatives). Verbs: `UAUD` (USB audio: outWr/outAvail/
  outPeak/outDeliv/outUR…), `DIAG` (graph tick: outWr/walkN/typeMask/nInUpd/
  inResp…), `TIME` (backdate/grid/sampler state + `eng=` octaver-engine-running
  flag — witnesses whether pitch/SNAC runs during passthrough), `WLAN`, `LINK`,
  `CLIP`, `GPAD` (gamepad branch), `BUID` (running-kernel build id), `UDSC`.
  Audio-capture/glitch-hunt verbs: `MRAW` (128-sample snapshot ring),
  `MLONG`+`MDUMP` (one-shot long free-run capture, chunked readback — sized to
  catch the ~50ms snore/tick that MRAW's ~2.7ms window misses), `MEVT` (live
  glitch-event scanner, last 64 events), `UWAV` (continuous ring-WAV recorder
  control). `:4444` has `REBOOT`/`WLAN`/`BUID`; `LTX`/`LMSG`/`TALV`/`RALV`/`RFRM`
  are Link wire-format probes.

## HARD ARCHITECTURE RULES

- **Never block in USB completion handlers** (syslog/CLogger UDP there = audio
  gaps). Observability = the ISR-safe lock-free `audioTelemetry` ring + `:4445`.
- **Cross-core single-writer.** Core 0 = USB ISR dispatch; Core 1 = DSP worker
  (whole audio graph via `doUpdate`); Core 2 = control plane (net, MIDI, APC,
  Link, telemetry); Core 3 = idle. Control→DSP params flow ONLY through
  `paramSnapshot` (double-buffered, single writer Core 2). ISR producers push
  SPSC rings drained on Core 2; never call a Core-2-owned object from an ISR.
  The IN handler (`AudioInputUSB::inHandler`) is the audio clock — it drives
  `AudioSystem::startUpdate`; the gate is `s_update_responsibility`, force-opened
  at IN device bind (`claimUpdateResponsibility`). An OUTPUT sink must run its
  `update()` every block even with `m_numConnections==0`.
- Audio: USB **and** internal both 48000Hz (`AUDIO_SAMPLE_RATE=48000`,
  `AUDIO_BLOCK_SAMPLES=64`) — the old 44100 internal rate is gone. The whole
  pipeline is **MONO** (`LOOPER_NUM_CHANNELS=1`, commit `7bb789d`); rings,
  blocks, and the continuous buffer are single-channel. Detail: `recall`
  **looper-audio-architecture**.

## OPERATOR CONTROL MAPPING (must not change without intent — load-bearing UI)

APC Key 25 grid (NUM_TRACKS=20, NUM_LAYERS=1): cols 0-1 = preset slots
(`row*2+col`); cols 2-5 = loopers (`row*4+(col-2)`); cols 6-7 blanked.
Looper pad gestures: empty tap → arm record (on press); recording tap → finish+
play; playing tap → pause (= MUTE, head keeps advancing); paused tap → resume
(position-identical); long-hold ≥1000ms → erase (LED off). SHIFT-hold = route
loops INTO the live effects (not mute). Microrepeat = held notes 82-86 (1, 1/2,
1/4, 1/8, 1/16 beat glitch). Sampler = buttons 65 (chromatic) / 66 (drum). Live
pitch = CC1 modwheel / CC52 / ch1+ch2 note-on. Effects CCs: reverb 48, delay 49,
time 50, formant 53; filters HP 51, res 54, LP 55; transpose 52. Command bases
(`commonDefines.h`): TRACK 0x20, STOP_TRACK 0x40, ERASE_TRACK 0x60, CLEAR_LAYER
0xA0, HALVE 0xC0, DOUBLE 0xE0.
Detail: `recall` **looper-grid-presets-ui**, **looper-microrepeat-full**,
**looper-sampler**, **looper-live-pitch-formant**, **looper-midimap-profile**.

USB gamepad control (on the gamepad branch): `recall`
**looper-gamepad-control** (HID pad → APC MIDI; Z=transpose, Rz=formant,
X=filter/res, LT/RT=delay, R1=reverb, L1=shift, buttons=loopers, hat=glitch).

## WiFi + Ableton Link (opt-in `LOOPER_ENABLE_WLAN`)

Compile-gated OFF by default. The looper hosts a `ticker` AP if alone, joins the
esp32's `ticker` AP if present (symmetric); the esp32 (`../esp-idf-link`) does
the same. The bcm4343 has a **unicast-RX wall** (no unicast-to-self) so standard
Link ping/pong measurement never completes there → custom multicast channels:
LCLK (esp→pi clock, port 20810), LTMP (pi→esp tempo-set, 20811), **TTMP (esp→pi
timeline, 20812)** for BIDIRECTIONAL tempo (either device sets the group tempo).
`:4445 WLAN` (clkRx/ttmpRx/link=…) + `LINK` are the witnesses. Detail: `recall`
**looper-wlan-link-full**, **looper-esp-bidirectional-link-ttmp**,
**looper-esp-link-sync-not-working-diagnosis**.

## SUBSYSTEM POINTERS (detail lives in recall memory)

- Timing / latency-backdated record / continuous buffer → `recall`
  **looper-timing-backdate**
- Clip state machine (9-state FSM, first-loop exact-region, pause=mute) →
  `recall` **looper-clip-state-machine**
- Clip tempo sync = VARISPEED resample → `recall` **looper-clip-varispeed**
- USB MIDI host (per-device OUT cap, async OUT, LED coalesce) → `recall`
  **looper-usb-midi-host**
- MIDI mapping is data (`midiMap.h`) → `recall` **looper-midimap-profile**
- Continuous USB ring-WAV recorder → `recall` **looper-usbwav-recorder**
- UAC2 host (Tascam US-2x2) + USB-enum-tolerate → `recall` (uac2 + usb-enum mems)
- AIR192|4 round-trip crackle/comb → deposit IN-ring at the device's **native**
  rate (`nomRate = m_fbRate`, commit `53854a6`); the earlier comb was a
  deposit-rate mismatch, not an interpolator bug (superseded `43b6aa9`). The
  once-residual **2048-sample one-sample-dropout tick (~23.44Hz, ~6/sec)** is
  **RESOLVED** on current firmware — user hardware-validated a live-tone analog
  round-trip captures clean, no tick (2026-07-12). The native-rate deposit fix +
  cubic-Hermite resync safety net (`43b6aa9`) round-trip is clean end-to-end.
  → `recall` **air192-native-rate-comb-fix**, **air192-2048-tick-open**.

## Logging

`LOOPER_LOG(...)` is a no-op (`Looper.h` #else). Do NOT re-enable the queued path
without making `LogUpdate()` ISR-safe (`new` in it corrupts the heap under
audio-ISR load). Observability = the `:4445` UDP verbs + the `audioTelemetry` ring.

## Planned (not yet implemented)

- 3-minute rolling recording buffer (largely realized by the continuous buffer —
  `recall` **looper-timing-backdate**).

@.gm/next-step.md
