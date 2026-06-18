#ifndef _usbMidi_h_
#define _usbMidi_h_

#include <circle/types.h>

void usbMidiProcess(bool bPlugAndPlayUpdated);
void usbMidiInjectMidi(u8 status, u8 data1, u8 data2);
// Returns true if queued on at least one device, false if every connected
// device dropped the message (no slot / no endpoint). Callers maintaining a
// last-sent cache must only mark "sent" on true — otherwise dropped frames
// leave LED state frozen out of sync with hardware.
bool usbMidiSendNoteOn(u8 note, u8 velocity);
void usbMidiSendCC(int cc_num, int value);
void usbMidiSend(u8 status, u8 data1, u8 data2);
void usbMidiTelemetry(unsigned *slotsMask, int *count, unsigned *inPackets,
                      unsigned *outDropped, unsigned *outErrors);

#endif
