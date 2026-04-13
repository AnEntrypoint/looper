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
// Drift correction using Bresenham-style long-window rate matching:
// Every OTG_RATE_WINDOW calls, measure (expected - actual) audio samples written.
// This gives the per-window drift in samples (positive = OTG faster).
// Spread exactly that many skip/repeat corrections evenly across the next window
// using a Bresenham error accumulator: increment by |drift| each call, apply
// correction when accumulator >= OTG_RATE_WINDOW, reset by OTG_RATE_WINDOW.
// This gives perfectly uniform correction spacing with no bursts.
// At 300ppm, drift ≈ 14 samples/window (1024 calls). Spacing = 1024/14 ≈ 73 calls.
#define OTG_LAG_TARGET    512
#define OTG_RATE_WINDOW   4096  // ~4s window — long enough to average block oscillation

static volatile unsigned s_otg_rd         = 0;
static bool              s_otg_rd_init    = false;
static unsigned          s_otg_call_cnt   = 0;    // calls since window start
static unsigned          s_otg_wr_start   = 0;    // wr at window start
static int               s_otg_drift_dir  = 0;    // +1 skip, -1 repeat, 0 unknown
static unsigned          s_otg_bresen_err = 0;    // Bresenham error accumulator
static unsigned          s_otg_bresen_n   = 0;    // |drift| corrections per window

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
        s_otg_rd       = wr - OTG_LAG_TARGET;
        s_otg_wr_start = wr;
        s_otg_rd_init  = true;
    }

    unsigned rd = s_otg_rd;
    int avail = (int)(wr - rd);

    // Hard resync only on catastrophic overrun/underrun.
    if (avail >= (int)(OUT_RING_SIZE - 64) || avail <= 0)
    {
        rd               = wr - OTG_LAG_TARGET;
        s_otg_wr_start   = wr;
        s_otg_call_cnt   = 0;
        s_otg_bresen_err = 0;
        s_otg_bresen_n   = 0;
        s_otg_drift_dir  = 0;
    }

    // Measure drift over window and schedule Bresenham-spread corrections.
    s_otg_call_cnt++;
    if (s_otg_call_cnt >= OTG_RATE_WINDOW)
    {
        int actual   = (int)(wr - s_otg_wr_start);
        int expected = (int)(OTG_RATE_WINDOW * nSamples);
        int drift    = expected - actual;   // >0 = OTG fast (need repeat), <0 = audio fast (need skip)
        s_otg_drift_dir  = (drift > 0) ? -1 : (drift < 0) ? +1 : 0;
        int absdrift     = drift > 0 ? drift : -drift;
        if (absdrift > 128) absdrift = 128;   // cap at 128 corrections/window = 31/sec
        s_otg_bresen_n   = (unsigned) absdrift;
        s_otg_bresen_err = 0;
        s_otg_wr_start   = wr;
        s_otg_call_cnt   = 0;
    }

    // Bresenham correction: spread s_otg_bresen_n corrections evenly over OTG_RATE_WINDOW calls.
    if (s_otg_bresen_n > 0)
    {
        s_otg_bresen_err += s_otg_bresen_n;
        if (s_otg_bresen_err >= OTG_RATE_WINDOW)
        {
            s_otg_bresen_err -= OTG_RATE_WINDOW;
            rd += s_otg_drift_dir;   // +1 skip or -1 repeat
        }
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
