# CLAUDE.md

## Non-obvious build caveats

- `circle-prh/audio/bcm_pcm.cpp` includes `"BCM_PCM.h"` (uppercase) but the file is `bcm_pcm.h` (lowercase). Linux CI requires a symlink: `ln -sf bcm_pcm.h circle/_prh/audio/BCM_PCM.h` before building.
- `circle-prh` miniuart uses `ARM_IRQ_AUX` and `ARM_GPIO_GPPUD` which are absent in phorton1/circle for RASPPI=4 (circle uses GIC-400 for rPi4). Fixed via `patches/miniuart.cpp` with `#if RASPPI < 4` guards.
- `phorton1/circle` does not support `AARCH=64` — `alloc.cpp` casts pointers to `u32` which fails in 64-bit. Use `AARCH=32` for RASPPI=4; produces `kernel7l.img`.
- Default build (no defines) uses CS42448 octo audio path. `LOOPER_USB_AUDIO=1` requires `AudioInputUSB`/`AudioOutputUSB` which do not exist in `circle-prh/audio` — must be added before that build target works.
- On Linux CI, `#include <audio\\Audio.h>` with backslash fails. The include in `audio.cpp` must use forward slash: `<audio/Audio.h>`.
- CI produces `kernel7l.img` for RASPPI=4 AARCH=32 (rPi 4, 32-bit). File naming per circle Rules.mk: RASPPI=2→kernel7, RASPPI=3 32-bit→kernel8-32, RASPPI=4 32-bit→kernel7l.
