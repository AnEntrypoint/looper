#include "input_usb.h"
#include "output_usb.h"
#include "usbaudiodevice.h"
#include "AudioSystem.h"
#include <circle/logger.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <circle/util.h>

audio_block_t *AudioInputUSB::s_block_left  = 0;
audio_block_t *AudioInputUSB::s_block_right = 0;
bool           AudioInputUSB::s_update_responsibility = false;
volatile u32   AudioInputUSB::s_peakLevel = 0;

#define IN_RING_SIZE 512
#define IN_TARGET_LAG   128
#define IN_DEADBAND     64
#define IN_RATE_GAIN    16384
#define IN_RATE_MAX_DEV 256
#define IN_FRAC_ONE     65536

static s16 s_in_ring_left [IN_RING_SIZE];
static s16 s_in_ring_right[IN_RING_SIZE];
static volatile unsigned s_in_ring_wr = 0;
static volatile unsigned s_in_ring_rd = 0;
static unsigned s_in_rd_frac = 0;
static s16 s_in_last_left  = 0;
static s16 s_in_last_right = 0;

volatile unsigned g_inUnderruns     = 0;
volatile unsigned g_inResyncs       = 0;
volatile int      g_inLastRateStep  = 65536;
volatile unsigned g_inLastTicks     = 0;

static s16 s_otg_ring_left [IN_RING_SIZE];
static s16 s_otg_ring_right[IN_RING_SIZE];
static volatile unsigned s_otg_ring_wr = 0;
static volatile unsigned s_otg_ring_rd = 0;

volatile unsigned AudioInputUSB_inRingWr (void) { return s_in_ring_wr; }
unsigned AudioInputUSB_inAvail (void) { return s_in_ring_wr - s_in_ring_rd; }

void AudioInputUSB_injectOTG (const s16 *pLeft, const s16 *pRight, unsigned nSamples)
{
    unsigned wr = s_otg_ring_wr;
    for (unsigned i = 0; i < nSamples; i++)
    {
        s_otg_ring_left [wr & (IN_RING_SIZE - 1)] = pLeft[i];
        s_otg_ring_right[wr & (IN_RING_SIZE - 1)] = pRight[i];
        wr++;
    }
    s_otg_ring_wr = wr;
}

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
    u32 peak = 0;
    for (unsigned i = 0; i < nSamples; i++)
    {
        s_in_ring_left [wr & (IN_RING_SIZE - 1)] = pLeft[i];
        s_in_ring_right[wr & (IN_RING_SIZE - 1)] = pRight[i];
        u32 absL = pLeft[i] < 0 ? (u32)(-pLeft[i]) : (u32)pLeft[i];
        u32 absR = pRight[i] < 0 ? (u32)(-pRight[i]) : (u32)pRight[i];
        if (absL > peak) peak = absL;
        if (absR > peak) peak = absR;
        wr++;
    }
    DataMemBarrier ();
    s_in_ring_wr = wr;
    if (peak > s_peakLevel) s_peakLevel = peak;
    g_inLastTicks = CTimer::GetClockTicks ();

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
        DataMemBarrier ();
        unsigned wr_snap = s_in_ring_wr;
        unsigned rd = s_in_ring_rd;
        unsigned rd_frac = s_in_rd_frac;
        int avail = (int)(wr_snap - rd);

        if (avail >= (int)(IN_RING_SIZE * 3 / 4) || avail < (int)AUDIO_BLOCK_SAMPLES)
        {
            rd = wr_snap - IN_TARGET_LAG;
            rd_frac = 0;
            g_inResyncs++;
            avail = IN_TARGET_LAG;
        }

        int dev = avail - (int)IN_TARGET_LAG;
        int band_dev = 0;
        if (dev > IN_DEADBAND)       band_dev = dev - IN_DEADBAND;
        else if (dev < -IN_DEADBAND) band_dev = dev + IN_DEADBAND;
        if (band_dev > IN_RATE_MAX_DEV)  band_dev = IN_RATE_MAX_DEV;
        if (band_dev < -IN_RATE_MAX_DEV) band_dev = -IN_RATE_MAX_DEV;
        int rate_step = IN_FRAC_ONE + (band_dev * IN_FRAC_ONE) / IN_RATE_GAIN;
        g_inLastRateStep = rate_step;

        unsigned otg_rd = s_otg_ring_rd;
        for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        {
            s32 l, r;
            if ((int)(s_in_ring_wr - rd) > 1)
            {
                s16 l0 = s_in_ring_left [rd       & (IN_RING_SIZE - 1)];
                s16 r0 = s_in_ring_right[rd       & (IN_RING_SIZE - 1)];
                s16 l1 = s_in_ring_left [(rd + 1) & (IN_RING_SIZE - 1)];
                s16 r1 = s_in_ring_right[(rd + 1) & (IN_RING_SIZE - 1)];
                l = l0 + (((s32)(l1 - l0) * (s32)rd_frac) >> 16);
                r = r0 + (((s32)(r1 - r0) * (s32)rd_frac) >> 16);
                s_in_last_left  = (s16)l;
                s_in_last_right = (s16)r;
                rd_frac += rate_step;
                rd      += rd_frac >> 16;
                rd_frac &= 0xFFFF;
            }
            else
            {
                l = s_in_last_left;
                r = s_in_last_right;
                g_inUnderruns++;
            }
            if (otg_rd != s_otg_ring_wr)
            {
                l += s_otg_ring_left [otg_rd & (IN_RING_SIZE - 1)];
                r += s_otg_ring_right[otg_rd & (IN_RING_SIZE - 1)];
                otg_rd++;
            }
            new_left->data[i]  = l > 32767 ? 32767 : (l < -32768 ? -32768 : (s16)l);
            new_right->data[i] = r > 32767 ? 32767 : (r < -32768 ? -32768 : (s16)r);
        }
        s_in_ring_rd  = rd;
        s_in_rd_frac  = rd_frac;
        s_otg_ring_rd = otg_rd;
    }

    transmit (new_left,  0);
    transmit (new_right, 1);
    if (new_left)  AudioSystem::release (new_left);
    if (new_right) AudioSystem::release (new_right);
}
