#define log_name "usbgamepad"

#include "gamepadInput.h"
#include "apcKey25.h"
#include <circle/usb/usbgamepad.h>
#include <circle/devicenameservice.h>
#include <circle/logger.h>
#include <circle/string.h>

// Global instance, allocated in audio.cpp setup() alongside pSampler/pMicroRepeat.
gamepadInput *pTheGamepad = 0;

// Emit sink: synthesize the exact MIDI the APC would send and feed it through
// the existing handler. Runs on Core 2 (from gamepadProcessTick). pTheAPC owns
// apcKey25 state on Core 2, so this is single-writer-safe.
static void gamepadEmitMidi(unsigned char status, unsigned char d1, unsigned char d2)
{
    if (pTheAPC) pTheAPC->handleMidi(status, d1, d2);
}

// USB status handler — fires in USB completion ISR context (Core 0). MUST NOT
// touch apcKey25/effects. Only snapshots the raw state into the coalescing ring
// (lock-free, no alloc). The Core-2 tick diffs + emits.
static void gamepadStatusHandler(unsigned nDeviceIndex, const TGamePadState *pState)
{
    (void)nDeviceIndex;
    if (!pTheGamepad || !pState) return;

    GpState g;
    g.naxes = pState->naxes;
    if (g.naxes > GP_MAX_AXIS) g.naxes = GP_MAX_AXIS;
    for (int i = 0; i < g.naxes; i++) {
        g.axes[i].value   = pState->axes[i].value;
        g.axes[i].minimum = pState->axes[i].minimum;
        g.axes[i].maximum = pState->axes[i].maximum;
    }
    g.nhats = pState->nhats;
    if (g.nhats > GP_MAX_HATS) g.nhats = GP_MAX_HATS;
    for (int i = 0; i < g.nhats; i++) g.hats[i] = pState->hats[i];
    g.nbuttons = pState->nbuttons;
    g.buttons  = pState->buttons;

    pTheGamepad->pushState(&g);
}

static CUSBGamePadDevice *s_pPad[5] = {};   // upad1..upad4 (index 1..4)

// Core-2 plug-and-play poll. Mirrors usbMidiProcess: re-query each slot, bind
// the status handler on a NEW device, mark disconnected on removal (which
// clears momentary controls so a yanked pad never leaves SHIFT/reverb/glitch
// stuck on).
void gamepadProcess(bool bPlugAndPlayUpdated)
{
    if (!bPlugAndPlayUpdated) return;
    if (!pTheGamepad) return;

    CString name;
    bool anyConnected = false;
    for (int i = 1; i <= 4; i++)
    {
        name.Format("upad%d", i);
        CUSBGamePadDevice *pDev = (CUSBGamePadDevice *)
            CDeviceNameService::Get()->GetDevice((const char *)name, FALSE);
        if (pDev && pDev != s_pPad[i])
        {
            s_pPad[i] = pDev;
            pDev->RegisterStatusHandler(gamepadStatusHandler);
            CLogger::Get()->Write(log_name, LogNotice, "USB gamepad connected: %s", (const char *)name);
        }
        else if (!pDev && s_pPad[i])
        {
            s_pPad[i] = nullptr;
            CLogger::Get()->Write(log_name, LogNotice, "USB gamepad gone: %s", (const char *)name);
        }
        if (s_pPad[i]) anyConnected = true;
    }

    if (anyConnected != pTheGamepad->connected())
    {
        pTheGamepad->setEmit(gamepadEmitMidi);
        pTheGamepad->setNumTracks(LOOPER_NUM_TRACKS);
        pTheGamepad->setConnected(anyConnected);   // false-edge clears momentary
    }
}

// Core-2 per-tick drain (called from coreControlPlaneTick after usbMidiProcess).
void gamepadProcessTick(void)
{
    if (pTheGamepad) pTheGamepad->processTick();
}

// :4445 GPAD telemetry.
void gamepadTelemetry(int *connected, int *axes, unsigned *buttons,
                      int *hatNote, unsigned *dropped, int *shift, int *reverb)
{
    if (!pTheGamepad) {
        if (connected) *connected = 0;
        if (axes) *axes = 0;
        if (buttons) *buttons = 0;
        if (hatNote) *hatNote = 0;
        if (dropped) *dropped = 0;
        if (shift) *shift = 0;
        if (reverb) *reverb = 0;
        return;
    }
    bool sh = false, rv = false;
    pTheGamepad->telemetry(axes, buttons, hatNote, dropped, &sh, &rv);
    if (connected) *connected = pTheGamepad->connected() ? 1 : 0;
    if (shift)  *shift  = sh ? 1 : 0;
    if (reverb) *reverb = rv ? 1 : 0;
}
