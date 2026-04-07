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
