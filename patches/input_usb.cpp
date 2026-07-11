#include "input_usb.h"
#include "output_usb.h"
#include "usbaudiodevice.h"
#include "AudioSystem.h"
#include "audioTelemetry.h"
#include <circle/logger.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <circle/util.h>

audio_block_t *AudioInputUSB::s_block_mono = 0;
bool           AudioInputUSB::s_update_responsibility = false;
volatile u32   AudioInputUSB::s_peakLevel = 0;

// DIAG (:4445 DIAG verb): prove the graph-tick driver. g_diagInHandler =
// inHandler invocations; g_diagInBlockCross = block-boundary crossings (each
// should call startUpdate); g_diagInResp = s_update_responsibility (0 => the
// gate is closed and the graph never ticks despite IN flowing).
volatile unsigned g_diagInHandler   = 0;
volatile unsigned g_diagInBlockCross = 0;
volatile unsigned g_diagInResp      = 0;

#define IN_RING_SIZE 512
#define IN_TARGET_LAG   96
#define IN_DEADBAND     48
// Gentler, tighter rate correction. The old GAIN=16384 / MAX_DEV=256 let the
// read rate swing up to ±1.56% as it hunted the target lag; the live-pitch
// engine reads the delay line at a fixed rate, so that ±1.5% rate OSCILLATION
// passed straight through as an audible -12 PITCH WARBLE (~16s cycle = the
// "gurgle"; wideband analysis showed the octave was always full-amplitude,
// just wobbling in frequency). 8x gentler gain + 4x tighter clamp keeps the
// correction rate near the true average so the warble is below audibility,
// while still tracking real long-term drift (the deadband + large ring absorb
// jitter; correction just happens slower and smaller).
#define IN_RATE_GAIN    131072
#define IN_RATE_MAX_DEV 64
#define IN_FRAC_ONE     65536
// A hard resync (rd jumps to a new ring position with no relation to the old
// one) is a STEP DISCONTINUITY in the sample stream -- inaudible/mild through
// a flat passthrough, but a resonant filter (HP/LP/resonance CCs engaged)
// RINGS on a step the way it rings on any impulse, which is exactly the
// user-reported signature ("buzz is audible on the input, we can hear the
// filter affecting it") that six prior fixes targeting the rate-bias
// MAGNITUDE never addressed, because they only ever changed how OFTEN a
// resync fires, never smoothed the jump itself. Crossfade over
// IN_RESYNC_XFADE samples so the filter sees a ramp, not a step.
#define IN_RESYNC_XFADE 16

// MONO: single ring, single last-sample/xfade tracker.
static s16 s_in_ring_mono[IN_RING_SIZE];
static volatile unsigned s_in_ring_wr = 0;
static volatile unsigned s_in_ring_rd = 0;
static unsigned s_in_rd_frac = 0;
static s16 s_in_last_mono  = 0;

// Crossfade-across-resync state: when set, the next IN_RESYNC_XFADE output
// samples blend s_in_xfade_mono (the last sample BEFORE the jump) into the
// freshly-read post-jump stream, ramping the discontinuity into a short
// slope instead of a step.
static unsigned s_in_xfade_remain = 0;
static s16      s_in_xfade_mono   = 0;

volatile unsigned g_inUnderruns     = 0;
volatile unsigned g_inResyncs       = 0;
volatile int      g_inLastRateStep  = 65536;
volatile unsigned g_inLastTicks     = 0;

static s16 s_otg_ring_mono[IN_RING_SIZE];
static volatile unsigned s_otg_ring_wr = 0;
static volatile unsigned s_otg_ring_rd = 0;

volatile unsigned AudioInputUSB_inRingWr (void) { return s_in_ring_wr; }
unsigned AudioInputUSB_inAvail (void) { return s_in_ring_wr - s_in_ring_rd; }

// MONO: sum the OTG device's L+R wire pair to mono before injecting into the
// mono graph ring (same convention as AudioInputUSB::inHandler below).
void AudioInputUSB_injectOTG (const s16 *pLeft, const s16 *pRight, unsigned nSamples)
{
    unsigned wr = s_otg_ring_wr;
    for (unsigned i = 0; i < nSamples; i++)
    {
        s32 m = ((s32)pLeft[i] + (s32)pRight[i]) / 2;
        s_otg_ring_mono[wr & (IN_RING_SIZE - 1)] = (s16)m;
        wr++;
    }
    s_otg_ring_wr = wr;
}

// MONO: 1 output port, matching LOOPER_NUM_CHANNELS (Looper.h). Not the macro
// directly -- this lib-side TU does not include Looper.h.
AudioInputUSB::AudioInputUSB (void) : AudioStream (0, 1, 0)
{
    memset (s_in_ring_mono, 0, sizeof s_in_ring_mono);
}

void AudioInputUSB::start (void)
{
    s_update_responsibility = AudioSystem::takeUpdateResponsibility ();
    CUSBAudioDevice *pDev = CUSBAudioDevice::Get ();
    if (pDev)
        pDev->RegisterInHandler (inHandler);
}

// Late-bind hook (see AudioOutputUSB_bindHandler): register the IN handler when
// the USB audio device enumerates after the graph already started.
void AudioInputUSB_bindHandler (CUSBAudioDevice *pDev)
{
    if (pDev)
    {
        pDev->RegisterInHandler (AudioInputUSB::inHandler);
        // Open the graph-tick gate now that a real IN device is bound. Without
        // this the boot handshake could leave s_update_responsibility false, so
        // inHandler never fired AudioSystem::startUpdate -> dead graph -> no
        // output (witnessed live: DIAG inResp=0, inHnd/inXing climbing, outWr=0).
        AudioInputUSB::claimUpdateResponsibility ();
    }
}

// MONO-tap raw snapshot (:4445 MRAW verb): the mono sum written into the ring
// here, so a live capture can compare directly against the pre-sum RAWD tap
// and against AudioOutputUSB's post-effects ring (see output_usb.cpp) to
// localize a reported artifact to a specific stage of the mono conversion.
//
// TRUE ROLLING WINDOW, not a per-completion snapshot: a single USB-IN
// completion only delivers ~48 samples (one microframe batch), well under
// the 128-sample display window RAWD-style verbs use -- an earlier version
// of this snapshot overwrote only the first ~48 slots per call, leaving the
// remaining ~80 slots permanently zero and producing a misleading "signal
// truncates to hard zero" artifact that was actually just an unwritten
// buffer tail, not a real audio dropout. Fix: g_audioMonoSnap is a proper
// ring, appended to (not overwritten) every completion, so a 128-sample
// read always reflects ~2-3 real completions' worth of continuous audio.
#define MRAW_SNAP_SAMPLES 128
volatile s16 g_audioMonoSnap[MRAW_SNAP_SAMPLES];
volatile unsigned g_audioMonoSnapSeq = 0;
static unsigned s_monoSnapWr = 0;

// Chronological-order accessor for the MRAW verb: the ring above is a plain
// overwrite buffer (index = absolute write count % MRAW_SNAP_SAMPLES), so a
// caller reading g_audioMonoSnap[0..127] in raw array order sees samples in
// WRAPPED order, not time order. This returns the current absolute write
// count so the reader can start from (wr % MRAW_SNAP_SAMPLES) -- the oldest
// still-valid slot -- and walk forward, wrapping, to reconstruct real
// chronological order.
unsigned AudioInputUSB_monoSnapWritePos (void) { return s_monoSnapWr; }

// LONG TRIGGERED CAPTURE (:4445 MLONG arm + MDUMP read, see kernel_run.cpp):
// MRAW's 128-sample window is only ~2.7ms at 48kHz -- far too short to catch
// one full cycle of the reported ~2433-sample (~50ms) periodic "snore", and
// polling MRAW repeatedly leaves large unobserved gaps between UDP round-
// trips (confirmed live: successive polls landed only ~2-4ms apart with each
// window covering just 128 samples, so the vast majority of real time between
// captures was never actually recorded). This is a much bigger ring (8192
// mono samples = ~170ms at 48kHz, comfortably spanning several full ~50ms
// glitch periods with margin) that free-runs continuously; MLONG arms/resets
// it and MDUMP reads back a chunk once enough real time has elapsed,
// eliminating the polling-gap blind spot entirely.
#define MLONG_CAPTURE_SAMPLES 8192   // must stay a multiple of MDUMP's 256-sample chunk size (kernel_run.cpp)
static s16 s_monoLongCapture[MLONG_CAPTURE_SAMPLES];
static volatile unsigned s_monoLongWr = 0;
static volatile bool s_monoLongArmed = false;

void AudioInputUSB_armLongCapture (void)
{
    s_monoLongWr = 0;
    s_monoLongArmed = true;
}

unsigned AudioInputUSB_longCaptureWritePos (void) { return s_monoLongWr; }
bool AudioInputUSB_longCaptureArmed (void) { return s_monoLongArmed; }

// GLITCH-EVENT LOG (:4445 MEVT verb): a live 170ms window caught NOTHING,
// and the user confirmed the "snore" is not continuously periodic but comes
// in seconds-scale bursts (glitchy stretch, clean stretch, repeat) -- an
// 8192-sample raw capture cannot span multiple seconds cheaply over UDP
// (would be hundreds of KB), so instead of capturing raw samples this scans
// EVERY mono sample in real time for the same discontinuity signature (a
// jump much bigger than the local running-average step size) and logs just
// the timestamp (CTimer ticks) + sample value + step size of each detected
// event into a small ring. Cheap enough to run continuously and poll
// sparsely until a real burst is caught, unlike a raw-sample capture.
// Flat parallel arrays, not a struct -- kernel_run.cpp (the :4445 MEVT
// reader) is a separate app-side translation unit from this lib-side file;
// four primitive-typed accessor functions avoid a cross-TU struct-type
// mismatch (a struct declared identically-but-separately in two TUs is a
// DIFFERENT type to the compiler, which fails extern linkage) with no
// shared header needed, matching this codebase's existing convention for
// every other cross-TU telemetry accessor (e.g. AudioInputUSB_inRingWr).
#define MEVT_LOG_SIZE 64
static u32      s_monoGlitchTick[MEVT_LOG_SIZE];
static unsigned s_monoGlitchAbsSample[MEVT_LOG_SIZE];
static s16      s_monoGlitchValue[MEVT_LOG_SIZE];
static s16      s_monoGlitchPrevValue[MEVT_LOG_SIZE];
static volatile unsigned s_monoGlitchLogWr = 0;
static s16 s_monoGlitchPrev = 0;
static s32 s_monoGlitchAvgStep = 64;   // running estimate of "normal" step size, Q0 (plain int, slow IIR)
static unsigned s_monoGlitchAbsSampleCtr = 0;

unsigned AudioInputUSB_glitchLogWritePos (void) { return s_monoGlitchLogWr; }
const u32      *AudioInputUSB_glitchLogTicks  (void) { return s_monoGlitchTick; }
const unsigned *AudioInputUSB_glitchLogSample (void) { return s_monoGlitchAbsSample; }
const s16      *AudioInputUSB_glitchLogValue  (void) { return s_monoGlitchValue; }
const s16      *AudioInputUSB_glitchLogPrev   (void) { return s_monoGlitchPrevValue; }
const s16 *AudioInputUSB_longCaptureBuffer (void) { return s_monoLongCapture; }

void AudioInputUSB::inHandler (const s16 *pLeft, const s16 *pRight, unsigned nSamples)
{
    // MONO: sum L+R to mono here, immediately at USB-IN decode -- every
    // downstream consumer (ring, graph, loops) sees only the mono signal.
    unsigned wr = s_in_ring_wr;
    unsigned prev_block = wr / AUDIO_BLOCK_SAMPLES;
    u32 peak = 0;
    unsigned snapWr = s_monoSnapWr;
    for (unsigned i = 0; i < nSamples; i++)
    {
        s32 m = ((s32)pLeft[i] + (s32)pRight[i]) / 2;
        s_in_ring_mono[wr & (IN_RING_SIZE - 1)] = (s16)m;
        g_audioMonoSnap[snapWr % MRAW_SNAP_SAMPLES] = (s16)m;
        snapWr++;
        u32 absM = m < 0 ? (u32)(-m) : (u32)m;
        if (absM > peak) peak = absM;
        wr++;
    }
    s_monoSnapWr = snapWr;
    g_audioMonoSnapSeq++;

    // Long triggered capture: while armed, append every mono sample into the
    // big free-running buffer until it's full, then auto-disarm (stays put
    // for MDUMP to read back -- not a ring, so a single arm/fill/read cycle
    // never gets overwritten mid-read).
    if (s_monoLongArmed)
    {
        unsigned lwr = s_monoLongWr;
        for (unsigned i = 0; i < nSamples && lwr < MLONG_CAPTURE_SAMPLES; i++)
        {
            s32 m = ((s32)pLeft[i] + (s32)pRight[i]) / 2;
            s_monoLongCapture[lwr++] = (s16)m;
        }
        s_monoLongWr = lwr;
        if (lwr >= MLONG_CAPTURE_SAMPLES) s_monoLongArmed = false;
    }

    // ALWAYS-ON glitch-event scanner: the user confirmed the "snore" is a
    // seconds-scale on/off burst pattern, not continuously periodic -- a raw
    // capture long enough to span that (multiple seconds) is impractical
    // over UDP, so instead this runs continuously and cheaply, logging only
    // the moment a real discontinuity happens. Detection: track a slow-IIR
    // running estimate of the "normal" per-sample step size; a step several
    // times that estimate is flagged as a glitch event (tick + absolute
    // sample index + value + prev value logged to a small ring, MEVT_LOG_SIZE
    // deep). Runs on the SAME mono stream as everything else (post L+R sum),
    // so a hit here proves the artifact is present at (or before) USB-IN
    // decode; a miss here while the user still hears it would point squarely
    // at the effects chain / USB-OUT stage instead.
    {
        unsigned absCtr = s_monoGlitchAbsSampleCtr;
        s16 prev = s_monoGlitchPrev;
        s32 avgStep = s_monoGlitchAvgStep;
        unsigned logWr = s_monoGlitchLogWr;
        for (unsigned i = 0; i < nSamples; i++)
        {
            s32 m = ((s32)pLeft[i] + (s32)pRight[i]) / 2;
            s16 cur = (s16)m;
            s32 step = cur - prev;
            s32 absStep = step < 0 ? -step : step;
            // Flag: a step at least 6x the running-normal estimate AND at
            // least 800 counts absolute (floors out false positives at
            // near-silence, where even a tiny avgStep makes the 6x ratio
            // trivially exceeded by ordinary noise).
            if (absStep > 800 && absStep > avgStep * 6)
            {
                unsigned slot = logWr % MEVT_LOG_SIZE;
                s_monoGlitchTick[slot]       = CTimer::GetClockTicks ();
                s_monoGlitchAbsSample[slot]  = absCtr;
                s_monoGlitchValue[slot]      = cur;
                s_monoGlitchPrevValue[slot]  = prev;
                logWr++;
            }
            // Slow IIR toward the current step (>>6 = ~1.6% per sample --
            // adapts to genuine amplitude changes over ~100+ samples, far
            // slower than a single glitch event, so one outlier step doesn't
            // itself corrupt the "normal" baseline it's compared against).
            avgStep += (absStep - avgStep) >> 6;
            if (avgStep < 8) avgStep = 8;   // floor so near-silence doesn't zero the threshold
            prev = cur;
            absCtr++;
        }
        s_monoGlitchAbsSampleCtr = absCtr;
        s_monoGlitchPrev = prev;
        s_monoGlitchAvgStep = avgStep;
        s_monoGlitchLogWr = logWr;
    }

    DataMemBarrier ();
    s_in_ring_wr = wr;
    if (peak > s_peakLevel) s_peakLevel = peak;
    g_inLastTicks = CTimer::GetClockTicks ();

    // DIAG: count inHandler calls + block-boundary crossings + expose the
    // responsibility flag, to prove whether the graph-tick driver fires.
    extern volatile unsigned g_diagInHandler, g_diagInBlockCross, g_diagInResp;
    g_diagInHandler++;
    g_diagInResp = s_update_responsibility ? 1u : 0u;
    unsigned cur_block = wr / AUDIO_BLOCK_SAMPLES;
    if (cur_block != prev_block)
    {
        g_diagInBlockCross++;
        // Guard: only tick the DSP when the ring holds a full drain's worth.
        // UAC2 delivers 48 samples/URB (N=8 microframes); the DSP drains 64
        // per tick. Without this guard, two rapid block-boundary crossings
        // fire two startUpdate calls that drain 128 samples when only 96 were
        // deposited -- avail hits 0, update() resyncs, re-reads stale samples
        // -> smeared/warped audio on UAC2. For UAC1 (UCA222) avail is always
        // >= IN_TARGET_LAG=96 when this fires, so the guard is a no-op there.
        if (s_update_responsibility &&
            (int)(wr - s_in_ring_rd) >= (int)AUDIO_BLOCK_SAMPLES)
            AudioSystem::startUpdate ();
    }
}

void AudioInputUSB::update (void)
{
    audio_block_t *new_mono = AudioSystem::allocate ();

    if (new_mono)
    {
        DataMemBarrier ();
        unsigned wr_snap = s_in_ring_wr;
        unsigned rd = s_in_ring_rd;
        unsigned rd_frac = s_in_rd_frac;
        int avail = (int)(wr_snap - rd);

        if (avail >= (int)(IN_RING_SIZE * 3 / 4) || avail < (int)AUDIO_BLOCK_SAMPLES)
        {
            audioTelemetryPush (TELEM_IN_RESYNC, (u32)avail);
            // Capture the last sample BEFORE the jump so the post-jump ramp
            // below can crossfade from it instead of stepping straight to
            // the new position's (unrelated) waveform value.
            s_in_xfade_mono   = s_in_last_mono;
            s_in_xfade_remain = IN_RESYNC_XFADE;
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
            s32 m;
            if ((int)(s_in_ring_wr - rd) > 1)
            {
                s16 m0 = s_in_ring_mono[rd       & (IN_RING_SIZE - 1)];
                s16 m1 = s_in_ring_mono[(rd + 1) & (IN_RING_SIZE - 1)];
                m = m0 + (((s32)(m1 - m0) * (s32)rd_frac) >> 16);
                s_in_last_mono = (s16)m;
                rd_frac += rate_step;
                rd      += rd_frac >> 16;
                rd_frac &= 0xFFFF;
            }
            else
            {
                m = s_in_last_mono;
                g_inUnderruns++;
                audioTelemetryPush (TELEM_IN_UNDERRUN, (u32)(s_in_ring_wr - rd));
            }
            if (otg_rd != s_otg_ring_wr)
            {
                m += s_otg_ring_mono[otg_rd & (IN_RING_SIZE - 1)];
                otg_rd++;
            }
            if (s_in_xfade_remain > 0)
            {
                // Linear ramp: weight the pre-jump sample down to 0 and the
                // post-jump sample up to full over IN_RESYNC_XFADE samples,
                // so a resonant filter downstream sees a slope, not a step.
                s32 w = (s32) s_in_xfade_remain;   // IN_RESYNC_XFADE..1
                s32 wPre  = w;
                s32 wPost = (s32) IN_RESYNC_XFADE - w + 1;
                m = (m * wPost + (s32) s_in_xfade_mono * wPre) / (s32) (IN_RESYNC_XFADE + 1);
                s_in_xfade_remain--;
            }
            new_mono->data[i] = m > 32767 ? 32767 : (m < -32768 ? -32768 : (s16)m);
        }
        s_in_ring_rd  = rd;
        s_in_rd_frac  = rd_frac;
        s_otg_ring_rd = otg_rd;
    }

    transmit (new_mono, 0);
    if (new_mono) AudioSystem::release (new_mono);
}
