#include "output_usb.h"
#include "usbaudiodevice.h"
#include "AudioSystem.h"
#include "audioTelemetry.h"
#include <circle/synchronize.h>
#include <circle/util.h>


#define OUT_RING_SIZE     4096   // bigger ring: absorb bursty engine-vs-USB rate excursions
#define OUT_RING_MASK     (OUT_RING_SIZE - 1)
static s16 s_ring_left [OUT_RING_SIZE];
static s16 s_ring_right[OUT_RING_SIZE];
static volatile unsigned s_ring_wr = 0;
static volatile unsigned s_ring_rd = 0;
static s16 s_out_last_left  = 0;
static s16 s_out_last_right = 0;

volatile unsigned g_outUnderruns   = 0;
volatile unsigned g_otgResyncs     = 0;
volatile unsigned g_outRingResync  = 0;   // main USB-OUT ring catastrophic resyncs (the glitch, pre-fix)
volatile int      g_otgLastRateStep = 65536;
// Diagnostic: counts AudioOutputUSB::update() block writes into the OUT ring.
// If this is FROZEN while the OUT URBs clock (outDeliv climbing), the graph tick
// is not reaching the output node = no-output cause distinct from a rate/mix
// fault. Surfaced as :4445 UAUD outWr.
volatile unsigned g_outWrites      = 0;
// DIAG: g_outUpdEntered = AudioOutputUSB::update() invocations (proves the
// doUpdate stream-walk reaches the output node); g_outNumConn = its
// m_numConnections (doUpdate gates `if(p->m_numConnections) p->update()`, so 0
// would skip the output sink entirely).
volatile unsigned g_outUpdEntered  = 0;
volatile unsigned g_outNumConn     = 0;

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

// MONO: 1 input port, matching LOOPER_NUM_CHANNELS (Looper.h). Not the macro
// directly -- this lib-side TU does not include Looper.h.
AudioOutputUSB::AudioOutputUSB (void) : AudioStream (1, 0, m_input_queue)
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

// Late-bind hook: the audio graph's start() runs at boot, BEFORE the USB audio
// device enumerates, so GetOut() is null then and the OUT handler never gets
// registered -- the iso pipe runs but ships silence. CUSBAudioDevice::Configure()
// calls this once the OUT endpoint is bound so the handler is always wired.
void AudioOutputUSB_bindHandler (CUSBAudioDevice *pDev)
{
    if (pDev)
        pDev->RegisterOutHandler (AudioOutputUSB::outHandler);
}

// Main USB OUT reader. Reads nSamples 1:1 from the ring. RATE control lives in the
// SEND-COUNT (StartOutRequest's PI ring-fill controller sizes nSamples so the
// long-run send rate converges to the engine = device-clock rate, compensating the
// high-speed OUT URB-rate deficit that under-fed the DAC). A safety resync (counted
// in g_outRingResync) only fires if the ring is genuinely starved/overflowed.
static bool s_out_rd_init = false;
void AudioOutputUSB::outHandler (s16 *pLeft, s16 *pRight, unsigned nSamples)
{
    DataMemBarrier ();
    unsigned wr_snap = s_ring_wr;
    // Low OUT target lag. UAC2 (async OUT, e.g. Tascam) controls the SEND rate via
    // the feedback PI in StartOutRequest (holds the ring near avail=256) so this
    // reader just reads passively there. UAC1 (UCA222, synchronous) has NO send-rate
    // PI, so without help here the read position drifts and the OUT lag creeps up.
    // For UAC1 we hold the lag at a SMALL target with a deadband skip/repeat trim
    // (the same click-tolerant scheme as the OTG tap) so monitoring latency stays
    // low. OUT_RD_TARGET = ~96 samples = ~2ms behind the writer.
    extern volatile unsigned g_audioUAC2;
    const int OUT_RD_TARGET  = 96;
    const int OUT_RD_DEADBAND = 32;   // > per-block write oscillation, so it doesn't hunt
    int seedLag = g_audioUAC2 ? 128 : OUT_RD_TARGET;
    if (!s_out_rd_init) { s_ring_rd = wr_snap - seedLag; s_out_rd_init = true; }
    unsigned rd = s_ring_rd;
    int avail = (int)(wr_snap - rd);

    // ONLY hard-resync on OVERFLOW (writer about to overwrite unread data). A low
    // avail is handled CLICK-FREE by the per-sample repeat-last fallback below --
    // a hard rd jump there was the audible resync burst.
    if (avail >= (int)(OUT_RING_SIZE - 64))
    {
        rd = wr_snap - (g_audioUAC2 ? (int)nSamples * 2 : OUT_RD_TARGET);
        g_outRingResync++;
    }

    // UAC1 rate-trim: nudge the read position by ONE sample/call when avail leaves
    // the deadband around the small target, so the OUT lag is pinned low instead of
    // drifting toward overflow. UAC2 is skipped (its feedback PI already paces).
    if (!g_audioUAC2)
    {
        int dev = avail - OUT_RD_TARGET;
        if (dev > OUT_RD_DEADBAND)       { rd++;    avail--; }   // too much lag: drop one
        else if (dev < -OUT_RD_DEADBAND) { if (rd) rd--; avail++; } // too little: repeat one
    }

    for (unsigned i = 0; i < nSamples; i++)
    {
        if ((int)(s_ring_wr - rd) > 0)
        {
            pLeft[i]  = s_ring_left [rd & OUT_RING_MASK];
            pRight[i] = s_ring_right[rd & OUT_RING_MASK];
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

// MONO-tap raw snapshot (:4445 MRAW verb): the post-effects mono block right
// before it is duplicated into the OUT ring, so a live capture can compare
// this against input_usb.cpp's post-sum snapshot to localize a reported
// artifact to the effects chain vs the USB IN/OUT boundary.
#define MRAW_OUT_SNAP_SAMPLES 64
volatile s16 g_audioMonoOutSnap[MRAW_OUT_SNAP_SAMPLES];
volatile unsigned g_audioMonoOutSnapSeq = 0;

void AudioOutputUSB::update (void)
{
    extern volatile unsigned g_outUpdEntered, g_outNumConn;
    g_outUpdEntered++;                 // proves doUpdate's walk reached this node
    g_outNumConn = m_numConnections;   // gate value (doUpdate skips if 0)
    // MONO graph input, duplicated to both physical wire channels (the USB
    // device still expects stereo). Single input port 0 now that
    // AudioOutputUSB is constructed with LOOPER_NUM_CHANNELS input ports.
    audio_block_t *new_mono = receiveReadOnly (0);

    unsigned wr = s_ring_wr;
    for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
    {
        s16 m = new_mono ? new_mono->data[i] : 0;
        s_ring_left [wr & (OUT_RING_SIZE - 1)] = m;
        s_ring_right[wr & (OUT_RING_SIZE - 1)] = m;
        if (i < MRAW_OUT_SNAP_SAMPLES) g_audioMonoOutSnap[i] = m;
        wr++;
    }
    g_audioMonoOutSnapSeq++;
    DataMemBarrier ();
    s_ring_wr = wr;
    g_outWrites++;   // diag: proves update() (graph tick) reaches the output node

    if (new_mono) AudioSystem::release (new_mono);
}
