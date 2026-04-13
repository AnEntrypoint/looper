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

// OTG tap: rate-adaptive ring reader.
// Target lag = OTG_LAG_TARGET.
//
// Drift correction: compare cumulative audio samples written (ring_wr) vs cumulative
// OTG samples consumed (rd_base = rd ignoring corrections). Over time these diverge at
// the USB clock drift rate (~14 samples/sec at 300ppm). Accumulate difference in a
// fractional accumulator and correct by 1 sample (skip/repeat) when |acc| >= 1.
//
// Key: use a rolling window of RATE_WINDOW calls to smooth the 64-sample block
// write granularity. Track wr at window start; compare to wr now vs expected reads.
//
// At 300ppm: wr advances 48000/sec, rd_base advances 48*1000/sec = 48000/sec.
// Drift: OTG slightly faster → rd_base - wr_delta > 0 over time → rd++ corrections.
#define OTG_LAG_TARGET    512
#define OTG_RATE_WINDOW   1024   // calls per rate-check window (~1 second)

static volatile unsigned s_otg_rd   = 0;
static bool              s_otg_rd_init = false;
static unsigned          s_otg_call_count = 0;   // rolling call counter
static unsigned          s_otg_wr_window  = 0;   // ring_wr at window start
static int               s_otg_rate_acc   = 0;   // fractional rate error accumulator

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
        s_otg_rd        = wr - OTG_LAG_TARGET;
        s_otg_wr_window = wr;
        s_otg_rd_init   = true;
    }

    unsigned rd = s_otg_rd;
    int avail = (int)(wr - rd);

    // Hard resync only if ring is about to wrap (catastrophic overrun)
    // or we have no data at all (catastrophic underrun).
    if (avail >= (int)(OUT_RING_SIZE - 64) || avail <= 0)
    {
        rd = wr - OTG_LAG_TARGET;
        s_otg_wr_window  = wr;
        s_otg_call_count = 0;
        s_otg_rate_acc   = 0;
    }

    // Rate-matching drift correction over a sliding window of OTG_RATE_WINDOW calls.
    // Expected audio written in window: OTG_RATE_WINDOW * nSamples (at nominal rates equal).
    // Actual audio written: wr - s_otg_wr_window.
    // Difference (actual - expected) reflects USB clock drift.
    // Accumulate per-call fraction; correct when |acc| >= OTG_RATE_WINDOW.
    s_otg_call_count++;
    if (s_otg_call_count >= OTG_RATE_WINDOW)
    {
        int actual   = (int)(wr - s_otg_wr_window);
        int expected = (int)(OTG_RATE_WINDOW * nSamples);
        // OTG faster → fewer audio samples written per window → actual < expected.
        // We want rd-- (repeat) when OTG faster → rate_acc should go positive.
        // So accumulate (expected - actual): positive when OTG fast.
        s_otg_rate_acc  += expected - actual;
        s_otg_wr_window  = wr;
        s_otg_call_count = 0;
    }

    // Apply 1 correction per OTG call until acc is drained (spread out corrections).
    if (s_otg_rate_acc >= 1)
    {
        s_otg_rate_acc--;
        rd--;           // OTG too fast: repeat 1 sample to slow reader
    }
    else if (s_otg_rate_acc <= -1)
    {
        s_otg_rate_acc++;
        rd++;           // audio too fast: skip 1 sample to speed reader
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
