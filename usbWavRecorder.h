#ifndef _usb_wav_recorder_h
#define _usb_wav_recorder_h

// Continuous ring-buffer WAV dump to a USB mass-storage device (flash drive /
// external HDD). If a drive is present it is auto-mounted (FatFs "USB:" volume,
// Circle device "umsd1") and the always-on continuousBuffer (the wet engine mix)
// is streamed to ONE fixed-size WAV file. When the file fills the drive the write
// position WRAPS back to the start of the data region and overwrites the oldest
// audio -- a black-box ring recorder. No drive = zero overhead (cheap ~1Hz probe).
//
// All FatFs I/O runs on the Core-2 control plane (usbWavTick from
// coreControlPlaneTick), NEVER the audio ISR -- the cb decouples them, giving a
// large slack (180s) so the periodic block writes never stall audio.

void usbWavInit (void);   // call once from setup()
void usbWavTick (void);   // call every Core-2 control tick

// Live telemetry for the :4445 UWAV verb.
extern volatile unsigned           g_uwavMounted;   // 1 = drive mounted + WAV open
extern volatile unsigned           g_uwavWraps;     // ring wrap count
extern volatile unsigned long long g_uwavBytes;     // total audio bytes written
extern volatile unsigned long long g_uwavMaxData;   // ring data size (bytes)

#endif
