#include "output_usb.h"
#include "usbaudiodevice.h"
#include "AudioSystem.h"
#include <circle/util.h>

audio_block_t *AudioOutputUSB::s_block_left  = 0;
audio_block_t *AudioOutputUSB::s_block_right = 0;

static s16 s_play_left [AUDIO_BLOCK_SAMPLES];
static s16 s_play_right[AUDIO_BLOCK_SAMPLES];
static s16 s_next_left [AUDIO_BLOCK_SAMPLES];
static s16 s_next_right[AUDIO_BLOCK_SAMPLES];
static volatile boolean s_next_ready = FALSE;
static unsigned s_out_pos = AUDIO_BLOCK_SAMPLES;

AudioOutputUSB::AudioOutputUSB (void) : AudioStream (2, 0, m_input_queue)
{
    memset (s_play_left,  0, sizeof s_play_left);
    memset (s_play_right, 0, sizeof s_play_right);
    memset (s_next_left,  0, sizeof s_next_left);
    memset (s_next_right, 0, sizeof s_next_right);
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
            if (s_next_ready)
            {
                memcpy (s_play_left,  s_next_left,  sizeof s_play_left);
                memcpy (s_play_right, s_next_right, sizeof s_play_right);
                s_next_ready = FALSE;
            }
            s_out_pos = 0;
        }

        pLeft[i]  = s_play_left [s_out_pos];
        pRight[i] = s_play_right[s_out_pos];
        s_out_pos++;
    }
}

void AudioOutputUSB::update (void)
{
    audio_block_t *new_left  = receiveReadOnly (0);
    audio_block_t *new_right = receiveReadOnly (1);

    if (new_left)
        memcpy (s_next_left,  new_left->data,  AUDIO_BLOCK_SAMPLES * sizeof (s16));
    else
        memset (s_next_left,  0, AUDIO_BLOCK_SAMPLES * sizeof (s16));
    if (new_right)
        memcpy (s_next_right, new_right->data, AUDIO_BLOCK_SAMPLES * sizeof (s16));
    else
        memset (s_next_right, 0, AUDIO_BLOCK_SAMPLES * sizeof (s16));

    s_next_ready = TRUE;

    if (new_left)  AudioSystem::release (new_left);
    if (new_right) AudioSystem::release (new_right);
}
