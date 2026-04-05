#ifndef _output_usb_h_
#define _output_usb_h_

#include "AudioStream.h"

class AudioOutputUSB : public AudioStream
{
public:
    AudioOutputUSB (void);

    virtual const char *getName (void) { return "AudioOutputUSB"; }
    virtual u16 getType (void)         { return AUDIO_DEVICE_OUTPUT; }

protected:
    virtual void update (void);
    virtual void start  (void);

private:
    static void outHandler (s16 *pLeft, s16 *pRight, unsigned nSamples);

    static audio_block_t *s_block_left;
    static audio_block_t *s_block_right;

    audio_block_t *m_input_queue[2];
};

#endif
