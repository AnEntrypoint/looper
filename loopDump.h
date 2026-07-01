#ifndef _loop_dump_h_
#define _loop_dump_h_

// On-demand dump of every recorded looper track to individual WAV files on the
// USB flash drive (the same FatFs "USB:" volume usbWavRecorder.cpp mounts for
// the continuous ring recording). Triggered by APC note 93 (channel 0) via
// LOOP_COMMAND_DUMP_TRACKS -> loopDumpRequest(); the actual FatFs writes are
// drained a few tracks per tick from loopDumpTick() on the Core-2 control
// plane, NEVER from the audio ISR or a USB completion handler.

void loopDumpInit (void);       // call once from setup()
void loopDumpTick (void);       // call every Core-2 control tick
void loopDumpRequest (void);    // arm a dump; safe to call from Core 2 (command dispatch)

// Live state for LED feedback (apcKey25) and the :4445 verb.
// 0 = idle, 1 = in progress, 2 = just finished (one-shot; cleared by the
// first apcKey25 poll after it flashes the pad), 3 = finished with errors.
extern volatile unsigned g_loopDumpState;
extern volatile unsigned g_loopDumpTracksWritten;

#endif
