#include "output_usb.h"
#include "usbaudiodevice.h"
#include "AudioSystem.h"
#include <circle/util.h>

audio_block_t *AudioOutputUSB::s_block_left  = 0;
audio_block_t *AudioOutputUSB::s_block_right = 0;

#define OUT_RING_SIZE     256
#define OUT_RING_MASK     (OUT_RING_SIZE - 1)
#define OTG_TARGET_LAG    128
#define OTG_LAG_MIN       80
#define OTG_LAG_MAX       200
static s16 s_ring_left [OUT_RING_SIZE];
static s16 s_ring_right[OUT_RING_SIZE];
static volatile unsigned s_ring_wr = 0;
static volatile unsigned s_ring_rd = 0;
static volatile unsigned s_ring_otg_rd = (unsigned)(0 - OTG_TARGET_LAG);

void AudioOutputUSB_tapOTG (s16 *pLeft, s16 *pRight, unsigned nSamples)
{
    unsigned rd = s_ring_otg_rd;
    unsigned wr = s_ring_wr;
    unsigned lag = (wr - rd) & OUT_RING_MASK;
    if (lag < OTG_LAG_MIN || lag > OTG_LAG_MAX)
        rd = wr - OTG_TARGET_LAG;
    static const s16 sine48[48] = {
        0,4277,8481,12539,16383,19947,23169,25995,28377,30272,
        31650,32487,32767,32487,31650,30272,28377,25995,23169,19947,
        16383,12539,8481,4277,0,-4277,-8481,-12539,-16383,-19947,
        -23169,-25995,-28377,-30272,-31650,-32487,-32767,-32487,-31650,-30272,
        -28377,-25995,-23169,-19947,-16383,-12539,-8481,-4277
    };
    static unsigned s_phase = 0;
    for (unsigned i = 0; i < nSamples; i++)
    {
        lag = (wr - rd) & OUT_RING_MASK;
        s16 l = 0, r = 0;
        if (lag > 0)
        {
            l = s_ring_left [rd & OUT_RING_MASK];
            r = s_ring_right[rd & OUT_RING_MASK];
            rd++;
        }
        s16 tone = sine48[s_phase++ % 48] >> 2;
        s32 ml = (s32)l + tone, mr = (s32)r + tone;
        pLeft[i]  = ml > 32767 ? 32767 : (ml < -32768 ? -32768 : (s16)ml);
        pRight[i] = mr > 32767 ? 32767 : (mr < -32768 ? -32768 : (s16)mr);
    }
    s_ring_otg_rd = rd;
}

AudioOutputUSB::AudioOutputUSB (void) : AudioStream (2, 0, m_input_queue)
{
    memset (s_ring_left,  0, sizeof s_ring_left);
    memset (s_ring_right, 0, sizeof s_ring_right);
}

void AudioOutputUSB::start (void)
{
    CUSBAudioDevice *pDev = CUSBAudioDevice::GetOut ();
    if (pDev)
        pDev->RegisterOutHandler (outHandler);
}

void AudioOutputUSB::outHandler (s16 *pLeft, s16 *pRight, unsigned nSamples)
{
    unsigned rd = s_ring_rd;
    for (unsigned i = 0; i < nSamples; i++)
    {
        if (rd != s_ring_wr)
        {
            pLeft[i]  = s_ring_left [rd & (OUT_RING_SIZE - 1)];
            pRight[i] = s_ring_right[rd & (OUT_RING_SIZE - 1)];
            rd++;
        }
        else
        {
            pLeft[i]  = 0;
            pRight[i] = 0;
        }
    }
    s_ring_rd = rd;
}

void AudioOutputUSB::update (void)
{
    audio_block_t *new_left  = receiveReadOnly (0);
    audio_block_t *new_right = receiveReadOnly (1);

    unsigned wr = s_ring_wr;
    for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
    {
        s_ring_left [wr & (OUT_RING_SIZE - 1)] = new_left  ? new_left->data[i]  : 0;
        s_ring_right[wr & (OUT_RING_SIZE - 1)] = new_right ? new_right->data[i] : 0;
        wr++;
    }
    s_ring_wr = wr;

    if (new_left)  AudioSystem::release (new_left);
    if (new_right) AudioSystem::release (new_right);
}
