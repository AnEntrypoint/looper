#include "output_usb.h"
#include "usbaudiodevice.h"
#include "AudioSystem.h"
#include <circle/synchronize.h>
#include <circle/util.h>

audio_block_t *AudioOutputUSB::s_block_left  = 0;
audio_block_t *AudioOutputUSB::s_block_right = 0;

#define OUT_RING_SIZE     2048
#define OUT_RING_MASK     (OUT_RING_SIZE - 1)
static s16 s_ring_left [OUT_RING_SIZE];
static s16 s_ring_right[OUT_RING_SIZE];
static volatile unsigned s_ring_wr = 0;
static volatile unsigned s_ring_rd = 0;
static s16 s_out_last_left  = 0;
static s16 s_out_last_right = 0;

volatile unsigned g_outUnderruns = 0;
volatile unsigned g_otgResyncs   = 0;
volatile unsigned g_otgSkips     = 0;
volatile unsigned g_otgRepeats   = 0;

unsigned AudioOutputUSB_outAvail (void) { return s_ring_wr - s_ring_rd; }

// OTG tap: rate-adaptive ring reader.
// Target lag = OTG_LAG_TARGET samples behind ring write pointer.
//
// Drift correction: direct deadband control.
// Every OTG call: if avail > TARGET+DEADBAND, skip one sample (rd++).
//                if avail < TARGET-DEADBAND, repeat one sample (rd--).
// Deadband=48 > block-write oscillation (~32), so oscillation doesn't
// trigger spurious corrections. At 300ppm=14 samples/sec drift:
// ~14 corrections/sec, spaced ~69 calls (69ms) apart uniformly.
#define OTG_LAG_TARGET    768   // headroom: 768/48000 = 16ms lag
#define OTG_DEADBAND      48    // > block-write oscillation amplitude (~32)

static volatile unsigned s_otg_rd      = 0;
static bool              s_otg_rd_init = false;

#ifndef LOOPER_USB_AUDIO
static volatile unsigned s_otg_sample_count = 0;
static volatile unsigned s_usb_in_wr_prev = 0;
extern volatile unsigned AudioInputUSB_inRingWr (void);
#endif

void AudioOutputUSB_tapOTG (s16 *pLeft, s16 *pRight, unsigned nSamples)
{
    DataMemBarrier ();
    unsigned wr = s_ring_wr;

    if (!s_otg_rd_init)
    {
        s_otg_rd      = wr - OTG_LAG_TARGET;
        s_otg_rd_init = true;
    }

    unsigned rd = s_otg_rd;
    int avail = (int)(wr - rd);

    // Hard resync on catastrophic overrun/underrun.
    if (avail >= (int)(OUT_RING_SIZE - 64) || avail <= 0)
    {
        rd    = wr - OTG_LAG_TARGET;
        avail = OTG_LAG_TARGET;
        g_otgResyncs++;
    }

    // Direct deadband correction: single skip or repeat per call.
    if (avail > OTG_LAG_TARGET + OTG_DEADBAND)
    {
        rd++;
        g_otgSkips++;
    }
    else if (avail < OTG_LAG_TARGET - OTG_DEADBAND)
    {
        rd--;
        g_otgRepeats++;
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
    unsigned wr_snap = s_ring_wr;
    unsigned rd = s_ring_rd;
    int avail = (int)(wr_snap - rd);

    if (avail >= (int)(OUT_RING_SIZE - 64) || avail < (int)nSamples)
    {
        rd = wr_snap - nSamples * 2;
    }

    for (unsigned i = 0; i < nSamples; i++)
    {
        if ((int)(s_ring_wr - rd) > 0)
        {
            pLeft[i]  = s_ring_left [rd & (OUT_RING_SIZE - 1)];
            pRight[i] = s_ring_right[rd & (OUT_RING_SIZE - 1)];
            s_out_last_left  = pLeft[i];
            s_out_last_right = pRight[i];
            rd++;
        }
        else
        {
            pLeft[i]  = s_out_last_left;
            pRight[i] = s_out_last_right;
            g_outUnderruns++;
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
