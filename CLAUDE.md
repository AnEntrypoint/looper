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
- **OTG gadget handler direction**: `TAudioInHandler (s16 *pLeft, s16 *pRight, unsigned nSamples)` — NON-CONST, gadget calls it to GET samples (Pi→host USB IN); handler must FILL arrays. `TAudioOutHandler (const s16 *pLeft, const s16 *pRight, unsigned nSamples)` — CONST, gadget calls it to DELIVER samples (host→Pi USB OUT); handler receives read-only data. Names are from the USB host perspective. `output_otg.cpp::tapHandler` uses TAudioInHandler; `input_otg.cpp::injectHandler` uses TAudioOutHandler. DMA buffer: interleaved s16 LE, `p[i*2]=left, p[i*2+1]=right`, 192 bytes = 48 samples at 48kHz stereo.
- **OTG is side-channel only** when `LOOPER_OTG_AUDIO=1` alongside `LOOPER_USB_AUDIO=1`. UCA222 drives the AudioSystem chain. OTG started via `otgIn.start()` / `otgOut.start()` in `audio.cpp` setup(). No `AudioConnection` objects for OTG — bypasses the graph entirely.
- **Never do blocking I/O in USB completion handlers.** `CLogger::Write` (syslog UDP) in `InCompletion`/`OutCompletion` causes 1-second periodic audio gaps. Solution: use `LOOPER_LOG()` macro in `loopMachine::update()` and `loopMachine::updateState()` to enqueue log messages (instead of writing immediately). Messages are drained by `uiWindow::updateFrame()` in main loop. **Critical:** the drain loop in `uiWindow.cpp` must use `while` (not `if`) to drain all pending messages per frame, otherwise messages back up at 1/frame rate and freeze logging.
- **Isochronous USB requests require `AddIsoPacket(maxPacketSize)`** before `SubmitAsyncRequest`. Without it, Circle's xHCI asserts `m_nNumIsoPackets > 0`.
- **DWC2 OTG isochronous requires SET_EVEN_FRAME/SET_ODD_FRAME on every BeginTransfer.** Without setting bit 28 (SETEVENFR) or bit 29 (SETODDFR) in DIEPCTLn/DOEPCTLn, the gadget transmits at half rate (every other USB frame skipped). In `patches/dwusbgadgetendpoint.cpp`: read DSTS register (Device Status) at address `DWHCI_DEV_CFG + 0x008` (NOT bare `0x008`), extract SOFFN (USB frame number, bits 21:8), target next frame, set frame parity bit before transfer setup. OTG audio: output handler reads from fixed lag `OTG_TARGET_LAG=48` samples behind ring write pointer to avoid phase skips when OTG fires at frame boundaries (every 1ms = 48 samples at 48kHz) while audio writes occur at block boundaries (64 samples).
- **UCA222 IN and OUT are on separate USB device nodes** (different hub ports). Each gets its own `CUSBAudioDevice` instance. `s_pThis` = IN device, `s_pOut` = OUT device. `AudioInputUSB` uses `Get()`, `AudioOutputUSB` uses `GetOut()`.
- **Factory intercepts only `int1-2-0`** (audio streaming), not `int1-1-0` (audio control). Removing the AudioControl device prevents Circle's built-in `CUSBAudioStreamingDevice` from stealing the interface.
- **5 looper tracks** mapped to APC Key 25's 5 rows (notes 0-39). `LOOPER_NUM_TRACKS=5` in `commonDefines.h`.
- **VU meter on APC column 8** (rightmost). Peak level tracked in `inHandler` (no blocking), displayed as 5-LED green/red meter at 30fps from `_updateGridLeds`.

## Clip state machine

- **`ClipState` enum** (9 mutually-exclusive values) replaces the former 7 `CLIP_STATE_*` bitmask defines. Impossible combinations (e.g. `RECORD_IN|PLAY_MAIN`) are no longer representable.
- **`CS_RECORDING_TAIL` vs `CS_FINISHING`** encode whether the clip will auto-play after recording ends — eliminating the former `m_pendingPlay` boolean flag.
- **`CS_LOOPING`** encodes the former `PLAY_MAIN|PLAY_END` bitmask combination. `update()` reads both the main block pointer and fade block pointer in this state.
- **Phase alignment**: `_startPlaying()` hard-locks `m_play_block = ((masterPhase - recordStartPhaseOffset) % numBlocks + numBlocks) % numBlocks`. `recordStartPhaseOffset` is set to `masterPhase` at recording start (always a phrase boundary due to pending latch). This guarantees `play_block=0` at every phrase boundary regardless of drift, since `numBlocks` is always a power-of-2 multiple or division of `masterLoopBlocks`. Canonical phase formula uses `% numBlocks + numBlocks) % numBlocks` to handle multi-phrase clips (old `+2*numBlocks` was insufficient).
- **`loopClip.cpp` is split into three files** at the 200-line limit: `loopClip.cpp` (init/transitions), `loopClipUpdate.cpp` (per-buffer audio processing), `loopClipState.cpp` (state name/quantize/updateState).
- **Quantize candidate selection** in `loopClipState.cpp`: `_calcQuantizeTarget()` uses fixed 7-candidate array {M/8, M/4, M/2, M, 2M, 4M, 8M} where M is master loop blocks. Picks nearest by absolute distance, skips candidates < CROSSFADE_BLOCKS*2.
- **ISR blocking fix**: All `CLogger::Get()->Write()` calls in loopClip.cpp, loopMachine.cpp, and other audio-path files replaced with `LOOPER_LOG()` macro. This enqueues messages non-blockingly; they are drained by `uiWindow::updateFrame()` in main loop. Critical: the drain loop must use `while` (not `if`) to prevent message backlog.

## WiFi and Ableton Link

- **Ethernet is the boot/syslog interface.** `CNetSubSystem` uses static IP `192.168.137.100`, gateway/DNS `192.168.137.1`, syslog target `192.168.137.1`. No DHCP wait. Connect dev machine to the same Ethernet segment.
- **WiFi (`CBcm4343Device m_WLAN`) is used only for Ableton Link.** `CNetSubSystem` runs on Ethernet. `CBcm4343Device` is initialized standalone (no `CNetSubSystem`) and used via raw `SendFrame`/`ReceiveFrame`. WLAN init/join failures are warnings only — Ethernet always boots.
- **Open network join**: `m_WLAN.JoinOpenNet("ticker")` in `Initialize()`, called after fatfs mount and before `m_Net.Initialize(FALSE)`. Member order in kernel class: `CBcm4343Device m_WLAN` after `FATFS m_FileSystem`, before `CNetSubSystem m_Net`.
- **WiFi AP fallback**: If `JoinOpenNet("ticker")` fails, Pi calls `m_WLAN.CreateOpenNet("ticker", 6, false)` to start as AP. Set `s_wlanIsAP=true` and call `wlanApInit(&m_WLAN)` to set static AP IP 192.168.4.1. In main loop: `bDhcpDone = !s_wlanJoined || s_wlanIsAP` (skip client DHCP in AP mode), then `if (s_wlanIsAP) wlanDhcpServe()` to handle client DHCP requests. DHCP pool: 192.168.4.2-9. New file `wlanDHCPServer.cpp` (141 lines) implements bare-metal server. In wlanDHCP.cpp, `wlanApSetIP()` sets s_ip and s_got=true so `wlanDhcpOK()` and `wlanDhcpIP()` work in AP mode. Heartbeat shows `wlan=ap` when active.
- **WiFi firmware** (`brcmfmac43455-sdio.{bin,txt,clm_blob}`) must be at `SD:/firmware/`. `tftp-server.js` copies them to `tftproot/firmware/` from the release zip for manual SD card placement.
- **Ableton Link** in `abletonLink.cpp` builds raw Ethernet+IP+UDP frames (14+20+8 byte headers) for multicast `224.76.78.75:20808` and calls `CBcm4343Device::SendFrame`. Receive via `ReceiveFrame` polls each loop, filters by ethertype `0x0800`, proto `17`, dst IP and port. Magic header `_asdp_v\x01`. ASDP v1 layout: magic(8) + version(4) + nodeId(8) at offset 12 + TLVs starting at offset 20. Parses `tmln` TLV (key `0x746d6c6e`, 24 bytes: microsPerBeat + beatOrigin + timeOrigin, big-endian int64). Sends alive every 1s with `mmbe` + `tmln`. Self-echo filtered on nodeId. Version bytes observed from peer: `01 05 00 00`; we send `01 05`.
- **Link-driven quantize**: When `linkIsSynced()`, `loopMachine::update()` sets `m_masterLoopBlocks = round(INTEGRAL_BLOCKS_PER_SECOND * 60.0 * 16.0 / bpm + 0.5)` rounded to the nearest multiple of 8 (4-bar phrase). `INTEGRAL_BLOCKS_PER_SECOND = ceil(44100/64) = 690`. Recording latches at phrase boundaries via `m_track_pending`; `recordStartPhaseOffset = masterPhase` at latch time.
- **`linkProcess()` called in main loop** (after `loop()`, before `m_Scheduler.Yield()`).
- **IGMP v2 membership report** sent for group `224.76.78.75` once DHCP completes, then every 30s. Required for phone hotspot APs with multicast snooping to forward Link frames to the Pi. Frame: ETH 14B + IP 24B (IHL=0x46, Router Alert option 0x94040000) + IGMP 8B (type=0x16). Implemented in `sendIgmpJoin()` in `abletonLink.cpp`.
- **WiFi DHCP**: `wlanDhcpSendDiscover()` called once before main loop; `wlanDhcpPoll()` called each iteration. DHCP frames arrive only after `m_Scheduler.Yield()`. Pi gets IP from the ticker hotspot (typically 192.168.4.x). Assigned IP used as source IP in Link frames via `wlanDhcpIP()`.
- **libwlan.a** built via `make -C circle/addon/wlan RASPPI=4 AARCH=32`. Linked before libcircle.a. INCLUDE adds `$(CIRCLEHOME)/addon/wlan`.

## Test coverage

- **Simulation scenarios** in `test/looper-sim.js`: 48 total scenarios covering sub-phrase (M/2), multi-phrase (2M), stop-quantize, deferred quantize, per-track latch quantize, and edge cases. Validates phase alignment, block selection, and state transitions without hardware.

## OTG gadget audio build caveats

When patching Circle's `lib/usb/gadget/` to add `CUSBAudioGadget` for combined OTG+USB audio:

- **Include path for patched headers**: Use `#include "dwusbgadgetendpoint.h"` (quotes, not angle brackets) when compiling from `lib/usb/gadget/`. Angle brackets resolve to the original Circle header even when a patched version exists in the same directory.
- **DMA_BUFFER macro unavailable**: The cloned Circle version may not define `DMA_BUFFER` in `<circle/macros.h>` within the gadget subdirectory context. Replace `DMA_BUFFER(u32, m_DummyBuffer, 1)` with plain `u32 m_DummyBuffer` and use `&m_DummyBuffer` when assigning to `void*` pointer targets.
- **dwusbgadgetendpoint API version**: Older Circle versions implement `FinishTransfer(void)` (no args), `OnControlMessage(void)` as virtual non-static member, and `HandleUSBReset(void)` as non-static member. Patched `.h` must declare these exactly — any signature mismatch causes "no declaration matches" linker errors.
- **AudioInputUSB/AudioOutputUSB start() visibility**: These classes declare `start()` as `protected` in circle-prh. Patched headers must also declare it `protected`, but `audio.cpp::setup()` calls `input.start()` and `output.start()` directly — requires `start()` to be `public` in the patch headers.
- **Audio Makefile object list**: `usbaudiogadget.o` and `usbaudiogadgetendpoint.o` must be explicitly added to `patches/audio_Makefile` OBJS list. Missing objects cause linker "undefined reference to CUSBAudioGadget" even though the `.cpp` files are copied to the patch directory.
- **Looper Makefile define**: `LOOPER_OTG_AUDIO` must be added via `ifdef LOOPER_OTG_AUDIO / DEFINE += -DLOOPER_OTG_AUDIO / endif` in the main `Makefile`. Without this, the preprocessor flag is not passed even when `make LOOPER_OTG_AUDIO=1` is invoked.
- **UAC1 AC Header descriptor size**: UAC1 Class-Specific AC Interface Header Descriptor with `bInCollection=2` is 10 bytes (8 fixed + 2 baInterfaceNr[] array). Array declaration must be `u8 ACHeader[10]`, not `[9]`. Using `[9]` causes "too many initializers" compile error.
- **CI failure layer progression**: Each CI run reveals one new build layer: (1) include path → (2) header/cpp API mismatch → (3) missing OBJS in Makefile → (4) access control (visibility) → (5) descriptor size errors. Fix each layer and push a new commit. Expect 5-8 iterations to resolve the full stack.

## Planned architecture (not yet implemented)

- **3-minute rolling recording buffer**: Audio input continuously fills a circular buffer. "Record" marks a start point, second press marks end and deep-copies the segment into a clip. Eliminates recording start/stop clicks.
- **Independent track controls**: Stopping one track should not stop others. The current `LOOP_COMMAND_STOP_IMMEDIATE` stops all tracks — needs per-track stop.
