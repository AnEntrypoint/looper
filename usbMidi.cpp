#define log_name "usbmidi"

#include "usbMidi.h"
#include "apcKey25.h"
#include <circle/logger.h>
#include <circle/devicenameservice.h>
#include <circle/usb/usbmidi.h>
#include <circle/string.h>

static CUSBMIDIDevice *s_pMIDIDevice = 0;

static void packetHandler(unsigned nCable, u8 *pPacket, unsigned nLength)
{
    if (nLength < 3) return;
    if (pTheAPC)
        pTheAPC->handleMidi(pPacket[0], pPacket[1], pPacket[2]);
}

void usbMidiProcess(bool bPlugAndPlayUpdated)
{
    if (bPlugAndPlayUpdated && s_pMIDIDevice == 0)
    {
        s_pMIDIDevice = (CUSBMIDIDevice *) CDeviceNameService::Get()->GetDevice("umidi1", FALSE);
        if (s_pMIDIDevice)
        {
            CLogger::Get()->Write(log_name, LogNotice, "USB MIDI device connected");
            s_pMIDIDevice->RegisterPacketHandler(packetHandler);
        }
    }
}

void usbMidiSendNoteOn(u8 note, u8 velocity)
{
    if (!s_pMIDIDevice) return;
    u8 msg[3] = { 0x90, note, velocity };
    s_pMIDIDevice->SendPlainMIDI(0, msg, 3);
}

void usbMidiSendCC(int cc_num, int value)
{
    if (!s_pMIDIDevice) return;
    u8 msg[3] = { 0xB0, (u8)cc_num, (u8)value };
    s_pMIDIDevice->SendPlainMIDI(0, msg, 3);
}
