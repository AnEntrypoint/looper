#ifndef AUDIO_TELEMETRY_H
#define AUDIO_TELEMETRY_H

#include <circle/types.h>

// ISR-safe event ring. Producers (USB completion handlers, audio update())
// call audioTelemetryPush() with one of the codes below. Consumer is
// audio.cpp::loop() (main thread) which drains and logs via CLogger::Write.
//
// No allocations, no strings, no UDP from producer side. Push is a single
// SPSC slot fill + atomic write-pointer bump. If the ring is full the
// event is dropped and g_telemDropped is bumped. 256 slots @ 12 bytes
// = 3 KB static.

enum AudioTelemCode : u32 {
    TELEM_IN_UNDERRUN     = 1,   // arg = current avail
    TELEM_IN_RESYNC       = 2,   // arg = avail before resync
    TELEM_OUT_UNDERRUN    = 3,   // arg = unused
    TELEM_OTG_RESYNC      = 4,   // arg = avail before resync
    TELEM_WATCHDOG        = 5,   // arg = ticks since last IN
    TELEM_MIDI_OUT_DROP   = 6,   // arg = unused
    TELEM_MIDI_OUT_ERR    = 7,   // arg = unused
    TELEM_LAG_SAMPLE      = 8,   // arg = (in_avail<<16)|out_avail (periodic snapshot)
};

struct AudioTelemEvent {
    u32 code;
    u32 ticks;
    u32 arg;
};

void audioTelemetryPush (u32 code, u32 arg);          // ISR-safe
bool audioTelemetryPop  (AudioTelemEvent *out);       // main-thread; false when empty

extern volatile unsigned g_telemDropped;

#endif
