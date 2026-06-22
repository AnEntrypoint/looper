#ifndef _output_usb_h_
#define _output_usb_h_

#include "AudioStream.h"

class AudioOutputUSB : public AudioStream
{
public:
    AudioOutputUSB (void);

    virtual const char *getName (void) { return "AudioOutputUSB"; }
    virtual u16 getType (void)         { return AUDIO_DEVICE_OUTPUT; }
    virtual void start  (void);

    // Public so the late-bind hook (AudioOutputUSB_bindHandler) can register it
    // from CUSBAudioDevice::Configure() when the device enumerates after boot.
    static void outHandler (s16 *pLeft, s16 *pRight, unsigned nSamples);

protected:
    virtual void update (void);

private:

    static audio_block_t *s_block_left;
    static audio_block_t *s_block_right;

    audio_block_t *m_input_queue[2];
};

#endif
