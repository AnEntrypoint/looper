#ifndef _usbMidi_h_
#define _usbMidi_h_

#include <circle/types.h>

void usbMidiProcess(bool bPlugAndPlayUpdated);
void usbMidiSendNoteOn(u8 note, u8 velocity);
void usbMidiSendCC(int cc_num, int value);

#endif
