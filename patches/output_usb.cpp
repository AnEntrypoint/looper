#include "output_usb.h"
#include "usbaudiodevice.h"
#include "AudioSystem.h"
#include "audioTelemetry.h"
#include <circle/synchronize.h>
#include <circle/util.h>

audio_block_t *AudioOutputUSB::s_block_left  = 0;
audio_block_t *AudioOutputUSB::s_block_right = 0;

#define OUT_RING_SIZE     1024
#define OUT_RING_MASK     (OUT_RING_SIZE - 1)
static s16 s_ring_left [OUT_RING_SIZE];
static s16 s_ring_right[OUT_RING_SIZE];
static volatile unsigned s_ring_wr = 0;
static volatile unsigned s_ring_rd = 0;
static s16 s_out_last_left  = 0;
static s16 s_out_last_right = 0;

volatile unsigned g_outUnderruns   = 0;
volatile unsigned g_otgResyncs     = 0;
volatile int      g_otgLastRateStep = 65536;

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
#define OTG_LAG_TARGET    384   // headroom: 384/48000 = 8ms lag (halved with ring shrink)
#define OTG_DEADBAND      96    // proportionally narrower for tighter ring
#define OTG_RATE_GAIN     16384
#define OTG_RATE_MAX_DEV  256
#define OTG_FRAC_ONE      65536

static volatile unsigned s_otg_rd      = 0;
static unsigned          s_otg_rd_frac = 0;
static bool              s_otg_rd_init = false;
static s16               s_otg_last_l  = 0;
static s16               s_otg_last_r  = 0;

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
    unsigned rd_frac = s_otg_rd_frac;
    int avail = (int)(wr - rd);

    if (avail >= (int)(OUT_RING_SIZE - 64) || avail <= (int)nSamples)
    {
        audioTelemetryPush (TELEM_OTG_RESYNC, (u32)avail);
        rd      = wr - OTG_LAG_TARGET;
        rd_frac = 0;
        avail   = OTG_LAG_TARGET;
        g_otgResyncs++;
    }

    int dev = avail - (int)OTG_LAG_TARGET;
    int band_dev = 0;
    if (dev > OTG_DEADBAND)       band_dev = dev - OTG_DEADBAND;
    else if (dev < -OTG_DEADBAND) band_dev = dev + OTG_DEADBAND;
    if (band_dev > OTG_RATE_MAX_DEV)  band_dev = OTG_RATE_MAX_DEV;
    if (band_dev < -OTG_RATE_MAX_DEV) band_dev = -OTG_RATE_MAX_DEV;
    int rate_step = OTG_FRAC_ONE + (band_dev * OTG_FRAC_ONE) / OTG_RATE_GAIN;
    g_otgLastRateStep = rate_step;

    for (unsigned i = 0; i < nSamples; i++)
    {
        s16 l0 = s_ring_left [rd       & OUT_RING_MASK];
        s16 r0 = s_ring_right[rd       & OUT_RING_MASK];
        s16 l1 = s_ring_left [(rd + 1) & OUT_RING_MASK];
        s16 r1 = s_ring_right[(rd + 1) & OUT_RING_MASK];
        pLeft[i]  = (s16)(l0 + (((s32)(l1 - l0) * (s32)rd_frac) >> 16));
        pRight[i] = (s16)(r0 + (((s32)(r1 - r0) * (s32)rd_frac) >> 16));
        s_otg_last_l = pLeft[i];
        s_otg_last_r = pRight[i];
        rd_frac += rate_step;
        rd      += rd_frac >> 16;
        rd_frac &= 0xFFFF;
    }
    s_otg_rd      = rd;
    s_otg_rd_frac = rd_frac;

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
            audioTelemetryPush (TELEM_OUT_UNDERRUN, 0);
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
