#include "output_usb.h"
#include "usbaudiodevice.h"
#include "AudioSystem.h"
#include <circle/synchronize.h>
#include <circle/util.h>

audio_block_t *AudioOutputUSB::s_block_left  = 0;
audio_block_t *AudioOutputUSB::s_block_right = 0;

#define OUT_RING_SIZE     512
#define OUT_RING_MASK     (OUT_RING_SIZE - 1)
static s16 s_ring_left [OUT_RING_SIZE];
static s16 s_ring_right[OUT_RING_SIZE];
static volatile unsigned s_ring_wr = 0;
static volatile unsigned s_ring_rd = 0;

// OTG tap: rate-adaptive ring reader.
// Target lag = OTG_LAG_TARGET. Drift correction every OTG_RATE_PERIOD calls:
// if avail > target+threshold → skip 1 sample (audio faster than OTG)
// if avail < target-threshold → repeat 1 sample (OTG faster than audio)
// This eliminates periodic hard resyncs from USB clock drift (~15 ppm).
// At 48kHz with 48-sample packets, OTG fires 1000x/sec.
// USB clock drift ~300ppm → ~14.4 samples/sec accumulation.
// Correct every 16 calls (~16ms): expected drift = 0.23 samples → threshold 0.
// Fractional accumulator: each call add drift estimate, correct when |acc|>=1.
#define OTG_LAG_TARGET    96

static volatile unsigned s_otg_rd = 0;
static bool   s_otg_rd_init = false;
static int    s_otg_err_acc = 0;    // signed fractional error accumulator (×64)

#ifndef LOOPER_USB_AUDIO
static volatile unsigned s_otg_sample_count = 0;
static volatile unsigned s_usb_in_wr_prev = 0;
extern volatile unsigned AudioInputUSB_inRingWr (void);
#endif

void AudioOutputUSB_tapOTG (s16 *pLeft, s16 *pRight, unsigned nSamples)
{
    unsigned wr = s_ring_wr;

    if (!s_otg_rd_init)
    {
        s_otg_rd = wr - OTG_LAG_TARGET;
        s_otg_rd_init = true;
    }

    unsigned rd = s_otg_rd;
    int avail = (int)(wr - rd);

    // Hard resync only if ring is about to wrap (catastrophic overrun)
    // or we have no data at all (catastrophic underrun).
    if (avail >= (int)(OUT_RING_SIZE - 16) || avail <= 0)
    {
        rd = wr - OTG_LAG_TARGET;
        avail = OTG_LAG_TARGET;
    }

    // Continuous fractional drift correction (×64 fixed-point accumulator).
    // Each call: acc += (avail - target). When |acc| >= 64, apply 1-sample
    // correction and subtract 64. This smoothly tracks USB clock divergence.
    s_otg_err_acc += (int)(avail) - OTG_LAG_TARGET;
    if (s_otg_err_acc >= 64)
    {
        s_otg_err_acc -= 64;
        rd++;           // skip 1 sample: reader lagging, speed up
    }
    else if (s_otg_err_acc <= -64)
    {
        s_otg_err_acc += 64;
        rd--;           // repeat 1 sample: reader ahead, slow down
    }

    for (unsigned i = 0; i < nSamples; i++)
    {
        pLeft[i]  = s_ring_left [rd & OUT_RING_MASK];
        pRight[i] = s_ring_right[rd & OUT_RING_MASK];
        rd++;
    }
    s_otg_rd = rd;

#ifndef LOOPER_USB_AUDIO
    unsigned usb_wr_now = AudioInputUSB_inRingWr ();
    if (usb_wr_now == s_usb_in_wr_prev)
    {
        unsigned prev = s_otg_sample_count;
        unsigned next = prev + nSamples;
        s_otg_sample_count = next;
        if ((prev / AUDIO_BLOCK_SAMPLES) != (next / AUDIO_BLOCK_SAMPLES))
            AudioSystem::startUpdate ();
    }
    s_usb_in_wr_prev = usb_wr_now;
#endif
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
    DataMemBarrier ();
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
    DataMemBarrier ();
    s_ring_wr = wr;

    if (new_left)  AudioSystem::release (new_left);
    if (new_right) AudioSystem::release (new_right);
}
