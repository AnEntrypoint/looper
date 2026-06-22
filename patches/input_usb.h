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

protected:
    virtual void update (void);

private:

    static audio_block_t *s_block_left;
    static audio_block_t *s_block_right;
    static bool           s_update_responsibility;

public:
    static volatile u32 s_peakLevel;
};

#endif
