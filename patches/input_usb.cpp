#include "input_usb.h"
#include "output_usb.h"
#include "usbaudiodevice.h"
#include "AudioSystem.h"
#include <circle/logger.h>
#include <circle/util.h>

audio_block_t *AudioInputUSB::s_block_left  = 0;
audio_block_t *AudioInputUSB::s_block_right = 0;
bool           AudioInputUSB::s_update_responsibility = false;

static s16 s_accum_left [AUDIO_BLOCK_SAMPLES];
static s16 s_accum_right[AUDIO_BLOCK_SAMPLES];
static unsigned s_accum_pos = 0;

AudioInputUSB::AudioInputUSB (void) : AudioStream (0, 2, 0)
{
}

void AudioInputUSB::start (void)
{
    s_update_responsibility = AudioSystem::takeUpdateResponsibility ();
    CUSBAudioDevice *pDev = CUSBAudioDevice::Get ();
    if (pDev)
        pDev->RegisterInHandler (inHandler);
}

void AudioInputUSB::inHandler (const s16 *pLeft, const s16 *pRight, unsigned nSamples)
{
    for (unsigned i = 0; i < nSamples; i++)
    {
        s_accum_left [s_accum_pos] = pLeft[i];
        s_accum_right[s_accum_pos] = pRight[i];
        s_accum_pos++;

        if (s_accum_pos >= AUDIO_BLOCK_SAMPLES)
        {
            audio_block_t *left  = s_block_left;
            audio_block_t *right = s_block_right;

            if (left)
                memcpy (left->data,  s_accum_left,  AUDIO_BLOCK_SAMPLES * sizeof (s16));
            if (right)
                memcpy (right->data, s_accum_right, AUDIO_BLOCK_SAMPLES * sizeof (s16));

            s_accum_pos = 0;

            if (s_update_responsibility)
                AudioSystem::startUpdate ();
        }
    }
}

void AudioInputUSB::update (void)
{
    audio_block_t *new_left  = AudioSystem::allocate ();
    audio_block_t *new_right = 0;
    if (new_left)
    {
        new_right = AudioSystem::allocate ();
        if (!new_right)
        {
            AudioSystem::release (new_left);
            new_left = 0;
        }
    }

    __disable_irq ();
    audio_block_t *out_left  = s_block_left;
    audio_block_t *out_right = s_block_right;
    s_block_left  = new_left;
    s_block_right = new_right;
    __enable_irq ();

    transmit (out_left,  0);
    AudioSystem::release (out_left);
    transmit (out_right, 1);
    AudioSystem::release (out_right);
}
