#include "output_usb.h"
#include "usbaudiodevice.h"
#include "AudioSystem.h"
#include <circle/util.h>

audio_block_t *AudioOutputUSB::s_block_left  = 0;
audio_block_t *AudioOutputUSB::s_block_right = 0;

static s16 s_out_left [AUDIO_BLOCK_SAMPLES];
static s16 s_out_right[AUDIO_BLOCK_SAMPLES];
static unsigned s_out_pos = AUDIO_BLOCK_SAMPLES;

AudioOutputUSB::AudioOutputUSB (void) : AudioStream (2, 0, m_input_queue)
{
}

void AudioOutputUSB::start (void)
{
    CUSBAudioDevice *pDev = CUSBAudioDevice::GetOut ();
    if (pDev)
        pDev->RegisterOutHandler (outHandler);
}

void AudioOutputUSB::outHandler (s16 *pLeft, s16 *pRight, unsigned nSamples)
{
    for (unsigned i = 0; i < nSamples; i++)
    {
        if (s_out_pos >= AUDIO_BLOCK_SAMPLES)
        {
            audio_block_t *left  = s_block_left;
            audio_block_t *right = s_block_right;

            if (left)
                memcpy (s_out_left,  left->data,  AUDIO_BLOCK_SAMPLES * sizeof (s16));
            else
                memset (s_out_left,  0, AUDIO_BLOCK_SAMPLES * sizeof (s16));
            if (right)
                memcpy (s_out_right, right->data, AUDIO_BLOCK_SAMPLES * sizeof (s16));
            else
                memset (s_out_right, 0, AUDIO_BLOCK_SAMPLES * sizeof (s16));

            s_out_pos = 0;
        }

        pLeft[i]  = s_out_left [s_out_pos];
        pRight[i] = s_out_right[s_out_pos];
        s_out_pos++;
    }
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
