#ifndef _input_usb_h_
#define _input_usb_h_

#include "AudioStream.h"

class AudioInputUSB : public AudioStream
{
public:
    AudioInputUSB (void);

    virtual const char *getName (void) { return "AudioInputUSB"; }
    virtual u16 getType (void)         { return AUDIO_DEVICE_INPUT; }

protected:
    virtual void update (void);
    virtual void start  (void);

private:
    static void inHandler (const s16 *pLeft, const s16 *pRight, unsigned nSamples);

    static audio_block_t *s_block_left;
    static audio_block_t *s_block_right;
    static bool           s_update_responsibility;
};

#endif
