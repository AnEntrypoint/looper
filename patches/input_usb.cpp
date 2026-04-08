#include "input_usb.h"
#include "output_usb.h"
#include "usbaudiodevice.h"
#include "AudioSystem.h"
#include <circle/logger.h>
#include <circle/util.h>

audio_block_t *AudioInputUSB::s_block_left  = 0;
audio_block_t *AudioInputUSB::s_block_right = 0;
bool           AudioInputUSB::s_update_responsibility = false;

#define IN_RING_SIZE 256
static s16 s_in_ring_left [IN_RING_SIZE];
static s16 s_in_ring_right[IN_RING_SIZE];
static volatile unsigned s_in_ring_wr = 0;
static volatile unsigned s_in_ring_rd = 0;

AudioInputUSB::AudioInputUSB (void) : AudioStream (0, 2, 0)
{
    memset (s_in_ring_left,  0, sizeof s_in_ring_left);
    memset (s_in_ring_right, 0, sizeof s_in_ring_right);
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
    unsigned wr = s_in_ring_wr;
    unsigned prev_block = wr / AUDIO_BLOCK_SAMPLES;
    for (unsigned i = 0; i < nSamples; i++)
    {
        s_in_ring_left [wr & (IN_RING_SIZE - 1)] = pLeft[i];
        s_in_ring_right[wr & (IN_RING_SIZE - 1)] = pRight[i];
        wr++;
    }
    s_in_ring_wr = wr;

    unsigned cur_block = wr / AUDIO_BLOCK_SAMPLES;
    if (cur_block != prev_block && s_update_responsibility)
        AudioSystem::startUpdate ();
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

    if (new_left && new_right)
    {
        unsigned rd = s_in_ring_rd;
        for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        {
            if (rd != s_in_ring_wr)
            {
                new_left->data[i]  = s_in_ring_left [rd & (IN_RING_SIZE - 1)];
                new_right->data[i] = s_in_ring_right[rd & (IN_RING_SIZE - 1)];
                rd++;
            }
            else
            {
                new_left->data[i]  = 0;
                new_right->data[i] = 0;
            }
        }
        s_in_ring_rd = rd;
    }

    transmit (new_left,  0);
    transmit (new_right, 1);
    if (new_left)  AudioSystem::release (new_left);
    if (new_right) AudioSystem::release (new_right);
}
