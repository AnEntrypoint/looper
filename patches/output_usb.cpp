#include "output_usb.h"
#include "usbaudiodevice.h"
#include "AudioSystem.h"
#include <circle/util.h>

audio_block_t *AudioOutputUSB::s_block_left  = 0;
audio_block_t *AudioOutputUSB::s_block_right = 0;

AudioOutputUSB::AudioOutputUSB (void) : AudioStream (2, 0, m_input_queue)
{
}

void AudioOutputUSB::start (void)
{
    CUSBAudioDevice *pDev = CUSBAudioDevice::Get ();
    if (pDev)
        pDev->RegisterOutHandler (outHandler);
}

void AudioOutputUSB::outHandler (s16 *pLeft, s16 *pRight, unsigned nSamples)
{
    audio_block_t *left  = s_block_left;
    audio_block_t *right = s_block_right;

    unsigned copy = (nSamples < AUDIO_BLOCK_SAMPLES) ? nSamples : AUDIO_BLOCK_SAMPLES;
    if (left)
        memcpy (pLeft,  left->data,  copy * sizeof (s16));
    else
        memset (pLeft,  0, copy * sizeof (s16));
    if (right)
        memcpy (pRight, right->data, copy * sizeof (s16));
    else
        memset (pRight, 0, copy * sizeof (s16));
}

void AudioOutputUSB::update (void)
{
    audio_block_t *new_left  = receiveReadOnly (0);
    audio_block_t *new_right = receiveReadOnly (1);

    __disable_irq ();
    audio_block_t *old_left  = s_block_left;
    audio_block_t *old_right = s_block_right;
    s_block_left  = new_left;
    s_block_right = new_right;
    __enable_irq ();

    if (old_left)  AudioSystem::release (old_left);
    if (old_right) AudioSystem::release (old_right);
}
