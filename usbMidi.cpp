#define log_name "usbmidi"

#include "usbMidi.h"
#include "apcKey25.h"
#include <circle/logger.h>
#include <circle/devicenameservice.h>
#include <circle/usb/usbmidi.h>
#include <circle/string.h>

static void packetHandler(unsigned nCable, u8 *pPacket, unsigned nLength)
{
    if (nLength < 3) return;
    if (pTheAPC)
        pTheAPC->handleMidi(pPacket[0], pPacket[1], pPacket[2]);
}

static bool s_registered[9] = {};
static CUSBMIDIDevice *s_pDevices[9] = {};
static int s_updateCount = 0;

void usbMidiProcess(bool bPlugAndPlayUpdated)
{
    if (!bPlugAndPlayUpdated) return;

    s_updateCount++;

    // Every 10th PnP update, log which umidi slots are present
    if (s_updateCount % 10 == 1)
    {
        CString name;
        CString found;
        for (int i = 1; i <= 4; i++)
        {
            name.Format("umidi%d", i);
            void *p = CDeviceNameService::Get()->GetDevice((const char *)name, FALSE);
            if (p) { found.Append((const char *)name); found.Append(" "); }
        }
        CLogger::Get()->Write(log_name, LogNotice, "PnP#%d umidi scan: [%s]",
            s_updateCount, found.GetLength() ? (const char *)found : "none");
    }

    CString name;
    for (int i = 1; i <= 8; i++)
    {
        if (s_registered[i]) continue;
        name.Format("umidi%d", i);
        CUSBMIDIDevice *pDev = (CUSBMIDIDevice *)
            CDeviceNameService::Get()->GetDevice((const char *)name, FALSE);
        if (!pDev) continue;
        s_pDevices[i] = pDev;
        s_registered[i] = true;
        CLogger::Get()->Write(log_name, LogNotice, "USB MIDI device connected: %s", (const char *)name);
        pDev->RegisterPacketHandler(packetHandler);
    }
}

void usbMidiSendNoteOn(u8 note, u8 velocity)
{
    u8 msg[3] = { 0x90, note, velocity };
    for (int i = 1; i <= 8; i++)
        if (s_pDevices[i]) s_pDevices[i]->SendPlainMIDI(0, msg, 3);
}

void usbMidiInjectMidi(u8 status, u8 data1, u8 data2)
{
    if (pTheAPC)
        pTheAPC->handleMidi(status, data1, data2);
}

void usbMidiSendCC(int cc_num, int value)
{
    u8 msg[3] = { 0xB0, (u8)cc_num, (u8)value };
    for (int i = 1; i <= 8; i++)
        if (s_pDevices[i]) s_pDevices[i]->SendPlainMIDI(0, msg, 3);
}

void usbMidiSend(u8 status, u8 data1, u8 data2)
{
    u8 msg[3] = { status, data1, data2 };
    for (int i = 1; i <= 8; i++)
        if (s_pDevices[i]) s_pDevices[i]->SendPlainMIDI(0, msg, 3);
}
