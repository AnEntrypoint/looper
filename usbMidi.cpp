#define log_name "usbmidi"

#include "usbMidi.h"
#include "apcKey25.h"
#include <circle/logger.h>
#include <circle/devicenameservice.h>
#include <circle/usb/usbmidi.h>
#include <circle/string.h>

volatile unsigned g_midiInPackets = 0;   // total MIDI packets received from any device

static void packetHandler(unsigned nCable, u8 *pPacket, unsigned nLength)
{
    if (nLength < 3) return;
    g_midiInPackets++;
    if (pTheAPC)
        pTheAPC->handleMidi(pPacket[0], pPacket[1], pPacket[2]);
}

static bool s_registered[9] = {};
static CUSBMIDIDevice *s_pDevices[9] = {};
static int s_updateCount = 0;

void usbMidiProcess(bool bPlugAndPlayUpdated)
{
    if (!bPlugAndPlayUpdated) return;

    // Re-query every slot each PnP update and track the live device pointer, not
    // a sticky "registered once" flag. A device that disappears then re-appears
    // (re-enumeration into the same slot) gets a NEW CUSBMIDIDevice*; the old
    // sticky flag skipped it, leaving s_pDevices[] dangling AND the LED cache
    // never re-invalidated (post-reconnect LEDs frozen on stale colors). Now any
    // pointer CHANGE (gone, new, or swapped) re-registers and invalidates the LED
    // cache so the next tick re-sends every LED to match true state.
    CString name;
    bool changed = false;
    for (int i = 1; i <= 8; i++)
    {
        name.Format("umidi%d", i);
        CUSBMIDIDevice *pDev = (CUSBMIDIDevice *)
            CDeviceNameService::Get()->GetDevice((const char *)name, FALSE);
        if (pDev == s_pDevices[i]) continue;          // unchanged slot
        s_pDevices[i]  = pDev;
        s_registered[i] = (pDev != nullptr);
        changed = true;
        if (pDev)
        {
            CLogger::Get()->Write(log_name, LogNotice, "USB MIDI device connected: %s", (const char *)name);
            pDev->RegisterPacketHandler(packetHandler);
        }
        else
        {
            CLogger::Get()->Write(log_name, LogNotice, "USB MIDI device gone: %s", (const char *)name);
        }
    }
    // Any roster change re-syncs the LED cache so every pad re-sends next tick.
    if (changed && pTheAPC) pTheAPC->invalidateLedCache();
}

bool usbMidiSendNoteOn(u8 note, u8 velocity)
{
    u8 msg[3] = { 0x90, note, velocity };
    bool any = false;
    for (int i = 1; i <= 8; i++) {
        if (s_pDevices[i]) {
            if (s_pDevices[i]->SendPlainMIDI(0, msg, 3)) any = true;
        }
    }
    return any;
}

// Telemetry for the :4445 MIDI verb: bitmask of present umidi1..umidi8 slots
// (bit i-1), the count of MIDI devices, MIDI-in packet count, and the MIDI-OUT
// drop/error counters. Lets us see live whether the APC enumerated as a MIDI
// device and whether LED sends are flowing, without syslog.
extern volatile unsigned g_midiOutDropped;
extern volatile unsigned g_midiOutErrors;
void usbMidiTelemetry(unsigned *slotsMask, int *count, unsigned *inPackets,
                      unsigned *outDropped, unsigned *outErrors)
{
    unsigned mask = 0; int n = 0;
    for (int i = 1; i <= 8; i++) if (s_pDevices[i]) { mask |= (1u << (i - 1)); n++; }
    if (slotsMask)  *slotsMask  = mask;
    if (count)      *count      = n;
    if (inPackets)  *inPackets  = g_midiInPackets;
    if (outDropped) *outDropped = g_midiOutDropped;
    if (outErrors)  *outErrors  = g_midiOutErrors;
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
