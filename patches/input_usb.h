#ifndef _input_usb_h_
#define _input_usb_h_

#include "AudioStream.h"

class AudioInputUSB : public AudioStream
{
public:
    AudioInputUSB (void);

    virtual const char *getName (void) { return "AudioInputUSB"; }
    virtual u16 getType (void)         { return AUDIO_DEVICE_INPUT; }
    virtual void start  (void);

    // Public so the late-bind hook (AudioInputUSB_bindHandler) can register it
    // from CUSBAudioDevice::Configure() when the device enumerates after boot.
    static void inHandler (const s16 *pLeft, const s16 *pRight, unsigned nSamples);

    // Open the graph-tick gate. The boot-time first-stream-wins handshake
    // (takeUpdateResponsibility in start()) can leave s_update_responsibility
    // false, so inHandler never drives AudioSystem::startUpdate and the whole
    // graph stops ticking (silence). The USB IN handler IS the audio clock, so
    // the late-bind hook calls this when the IN device actually enumerates —
    // the gate is then open exactly when IN can clock the graph.
    static void claimUpdateResponsibility (void) { s_update_responsibility = true; }

protected:
    virtual void update (void);

private:

    static audio_block_t *s_block_mono;
    static bool           s_update_responsibility;

public:
    static volatile u32 s_peakLevel;
};

#endif
