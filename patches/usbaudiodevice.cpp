#include "usbaudiodevice.h"
#include "uac2parse.h"
#include <circle/usb/usb.h>
#include <circle/usb/usbhostcontroller.h>
#include <circle/devicenameservice.h>
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <circle/string.h>
#include <assert.h>

// Raw config descriptor of the last UAC2 device, captured in usbdevice.cpp.
extern u8 g_uac2Desc[];
extern volatile unsigned g_uac2DescLen;

// Convert one little-endian sample of subslot bytes to s16 (take the top 16
// bits, matching the engine's s16 pipeline). 24-bit -> bytes [1..2], 16 -> [0..1].
static inline s16 uac2ToS16 (const u8 *p, unsigned subslot)
{
    if (subslot >= 4) return (s16) ((p[3] << 8) | p[2]);
    if (subslot == 3) return (s16) ((p[2] << 8) | p[1]);
    if (subslot == 2) return (s16) ((p[1] << 8) | p[0]);
    return (s16) (p[0] << 8);
}

// Write an s16 into subslot bytes little-endian (low bytes zero-padded).
static inline void uac2FromS16 (u8 *p, unsigned subslot, s16 v)
{
    if (subslot >= 4) { p[0]=0; p[1]=0; p[2]=(u8)(v & 0xFF); p[3]=(u8)(v >> 8); }
    else if (subslot == 3) { p[0]=0; p[1]=(u8)(v & 0xFF); p[2]=(u8)(v >> 8); }
    else if (subslot == 2) { p[0]=(u8)(v & 0xFF); p[1]=(u8)(v >> 8); }
    else p[0] = (u8)(v >> 8);
}

static const char FromAudio[] = "uaudio";

CUSBAudioDevice *CUSBAudioDevice::s_pThis = 0;
CUSBAudioDevice *CUSBAudioDevice::s_pOut  = 0;
unsigned         CUSBAudioDevice::s_nDeviceNumber = 1;

// Live audio-IN status for the :4445 UAUD verb. These are EXTERNAL volatile
// globals, written on the USB-enumeration/ISR cores and read on the Core-2
// control plane. The class statics (s_pThis etc.) read stale across cores from
// the verb, so the bound-state + format are mirrored here at bind time and the
// delivery counters (g_audioInDeliv/g_audioInPeak, defined in usbdevicefactory)
// are bumped from InCompletion. inDeliv climbing + inPeak>0 == real input flowing.
volatile unsigned g_audioInBound  = 0;   // IN device (s_pThis) bound
volatile unsigned g_audioOutBound = 0;   // OUT device (s_pOut) bound
volatile unsigned g_audioUAC2     = 0;   // bound IN device is UAC2
volatile unsigned g_audioChannels = 0;   // IN format channels
volatile unsigned g_audioSubslot  = 0;   // IN format bytes/sample
volatile unsigned g_audioRate     = 0;   // negotiated sample rate (Hz)

CUSBAudioDevice::CUSBAudioDevice (CUSBFunction *pFunction)
:   CUSBFunction (pFunction),
    m_pEndpointIn  (0),
    m_pEndpointOut (0),
    m_pEndpointFb  (0),
    m_pInHandler   (0),
    m_pOutHandler  (0),
    m_pFbURB       (0),
    m_fbRate       (6u << 16),   // nominal 48000/8000 (high-speed) until feedback
    m_fbAccum      (0),
    m_inNomAccum   (0),
    m_nPeakIn      (0),
    m_nLastMonitorTick (0),
    m_bUAC2        (FALSE),
    m_uRate        (48000),
    m_uSubslot     (2),
    m_uChannels    (2)
{
    m_pInURB[0]  = 0;
    m_pInURB[1]  = 0;
    m_pOutURB[0] = 0;
    m_pOutURB[1] = 0;
    m_nInSubmitBytes[0] = 0;
    m_nInSubmitBytes[1] = 0;
}

CUSBAudioDevice::~CUSBAudioDevice (void)
{
    delete m_pEndpointIn;
    delete m_pEndpointOut;
    delete m_pEndpointFb;
    delete m_pInURB[0];
    delete m_pInURB[1];
    delete m_pOutURB[0];
    delete m_pOutURB[1];
    if (s_pThis == this) { s_pThis = 0; g_audioInBound = 0; }
    if (s_pOut == this)  { s_pOut  = 0; g_audioOutBound = 0; }
}

boolean CUSBAudioDevice::Configure (void)
{
    CLogger::Get ()->Write (FromAudio, LogNotice, "Configure() called proto=0x%02x",
        (unsigned) GetInterfaceProtocol ());
    boolean bSelected = FALSE;

    // If THIS interface is a UAC2 audio-streaming interface (protocol 0x20), go
    // straight to the UAC2 path. The UAC1 SelectInterfaceByClass(1,2,0,..) loop
    // below WALKS the config parser to the end looking for a proto-0 match; on a
    // UAC2 device that finds nothing and leaves the parser exhausted (current
    // interface descriptor null), so a subsequent UAC2 SelectInterfaceByClass
    // finds nothing either -- the device would never bind. Branch up front.
    if (GetInterfaceProtocol () == 0x20)
    {
        bSelected = ConfigureUAC2 ();
    }
    for (unsigned nAlt = 1; nAlt <= 4 && !bSelected; nAlt++)
    {
        if (SelectInterfaceByClass (1, 2, 0, nAlt))
        {
            CLogger::Get ()->Write (FromAudio, LogNotice,
                "Selected audio streaming alt-setting %u", nAlt);
            bSelected = TRUE;
        }
    }
    if (!bSelected)
    {
        CLogger::Get ()->Write (FromAudio, LogWarning,
            "No usable audio-streaming alt-setting -- refusing device");
        ConfigurationError (FromAudio);
        return FALSE;
    }

    const TUSBEndpointDescriptor *pDesc;
    while ((pDesc = (const TUSBEndpointDescriptor *) GetDescriptor (DESCRIPTOR_ENDPOINT)) != 0)
    {
        boolean bIsIn   = (pDesc->bEndpointAddress & 0x80) == 0x80;
        boolean bIsIso  = (pDesc->bmAttributes & 0x03) == 0x01;
        if (!bIsIso) continue;
        // A UAC2 explicit feedback endpoint (iso, usage type 0x10) is IN-direction
        // but carries the device's desired sample rate, not audio. Keep it as the
        // feedback pipe (services async OUT pacing) -- NOT as a data endpoint.
        if ((pDesc->bmAttributes & 0x30) == 0x10)
        {
            if (!m_pEndpointFb)
                m_pEndpointFb = new CUSBEndpoint (GetDevice (), pDesc);
            continue;
        }

        if (bIsIn && !m_pEndpointIn)
            m_pEndpointIn  = new CUSBEndpoint (GetDevice (), pDesc);
        else if (!bIsIn && !m_pEndpointOut)
            m_pEndpointOut = new CUSBEndpoint (GetDevice (), pDesc);
    }

    if (!m_pEndpointIn && !m_pEndpointOut)
    {
        CLogger::Get ()->Write (FromAudio, LogError,
            "No isochronous endpoints found");
        ConfigurationError (FromAudio);
        return FALSE;
    }

    if (!CUSBFunction::Configure ())
    {
        CLogger::Get ()->Write (FromAudio, LogError, "Cannot set interface");
        return FALSE;
    }

    CString DeviceName;
    DeviceName.Format ("uaudio%u", s_nDeviceNumber++);
    CDeviceNameService::Get ()->AddDevice (DeviceName, this, FALSE);

    // The audio graph's start() ran at boot, before this device enumerated, so its
    // RegisterIn/OutHandler call found GetIn()/GetOut() null and never wired the
    // handlers -- the iso pipe would then run but ship silence. Bind the handlers
    // NOW that the endpoints exist, so audio actually flows. (free funcs in
    // input_usb.cpp / output_usb.cpp)
    extern void AudioInputUSB_bindHandler  (CUSBAudioDevice *pDev);
    extern void AudioOutputUSB_bindHandler (CUSBAudioDevice *pDev);

    if (m_pEndpointIn && !s_pThis)
    {
        s_pThis = this;
        g_audioInBound  = 1;            // mirror for the cross-core UAUD verb
        g_audioUAC2     = m_bUAC2 ? 1 : 0;
        g_audioChannels = m_uChannels;
        g_audioSubslot  = m_uSubslot;
        g_audioRate     = m_uRate;
        AudioInputUSB_bindHandler (this);
    }
    if (m_pEndpointOut && !s_pOut)
    {
        s_pOut = this;
        g_audioOutBound = 1;
        AudioOutputUSB_bindHandler (this);
    }

    CLogger::Get ()->Write (FromAudio, LogNotice, "USB audio configured (in=%s out=%s)",
        m_pEndpointIn ? "yes" : "no", m_pEndpointOut ? "yes" : "no");

    if (m_pEndpointIn)
    {
        // Double-buffer (2 IN URBs in flight) is a UAC2-ONLY measure, mirroring
        // the OUT double-buffer below: a high-bandwidth UAC2 IN device (e.g. the
        // AIR 192|4) can miss microframes during the per-URB re-arm gap between
        // InCompletion's delete+StartInRequest and the next URB actually being
        // queued. The full-speed UCA222 (UAC1, 1ms re-arm window) never falls
        // behind, so it stays single-buffered (unchanged behaviour/latency).
        StartInRequest (0);
        if (m_bUAC2)
            StartInRequest (1);
    }
    if (m_pEndpointOut)
    {
        // Double-buffer (2 OUT URBs in flight) is a UAC2-ONLY measure: the
        // high-speed async-OUT device (Tascam US-2x2) misses microframes during
        // the per-URB re-arm gap, so one URB must always be in flight. It costs
        // ~1 URB period of extra pipeline latency. The full-speed UCA222 (UAC1,
        // 1ms re-arm window) never falls behind, so it does NOT need the second
        // URB -- and before the UAC2 work it ran single-buffered. Keep UAC1
        // single-buffered to restore its original low monitoring latency;
        // OutCompletion re-arms per-slot, so a single slot 0 self-sustains.
        StartOutRequest (0);
        if (m_bUAC2 && m_pEndpointFb)
            StartOutRequest (1);
    }
    if (m_pEndpointFb)
        StartFbRequest ();   // service async OUT feedback so the device paces/plays

    return TRUE;
}

// UAC2 (USB Audio Class 2.0): select this AS interface's operational alt
// (protocol 0x20), learn its sample format (subslot/channels) from the captured
// config descriptor, and set 48000 Hz on the device's Clock Source entity via a
// class control request to the Audio Control interface (UAC2 sets rate on the
// clock entity, NOT via a UAC1 endpoint request). Returns TRUE if a UAC2
// operational alt was selected. The caller's endpoint-grab + StartIn/OutRequest
// then run unchanged (the feedback EP is skipped there).
boolean CUSBAudioDevice::ConfigureUAC2 (void)
{
    if (!SelectInterfaceByClass (1, 2, 0x20, 1))
        return FALSE;

    m_bUAC2 = TRUE;

    Uac2Info info;
    if (uac2ParseConfig (g_uac2Desc, g_uac2DescLen, &info))
    {
        int myIf = GetInterfaceNumber ();
        const Uac2Stream *s = 0;
        if (info.in.valid  && info.in.interfaceNum  == myIf) s = &info.in;
        else if (info.out.valid && info.out.interfaceNum == myIf) s = &info.out;
        if (s)
        {
            if (s->subslotSize > 0) m_uSubslot  = (unsigned) s->subslotSize;
            if (s->channels    > 0) m_uChannels = (unsigned) s->channels;
        }
        if (info.clockId != 0 && info.acInterfaceNum >= 0)
        {
            u16 wIndex = (u16) ((info.clockId << 8) | info.acInterfaceNum);

            // Negotiate the sample rate generically (works on any UAC2 device, not
            // just a 48000 one): GET_RANGE the Clock Source's supported sample
            // frequencies, then prefer 48000 (the looper's USB rate), else 44100,
            // else the highest supported. A device that doesn't do 48000 (e.g. a
            // 44100-only looper) still engages at its native rate.
            unsigned chosen = 48000;
            int nr = GetHost ()->ControlMessage (GetEndpoint0 (),
                        0xA1,                  // IN | class | interface
                        0x02,                  // RANGE
                        (u16) (0x01 << 8),     // CS_SAM_FREQ_CONTROL, ch 0
                        wIndex, m_InBuf, sizeof m_InBuf);
            if (nr >= 2)
            {
                const u8 *rb = (const u8 *) m_InBuf;
                unsigned nsub = rb[0] | (rb[1] << 8);
                boolean has48 = FALSE, has441 = FALSE; unsigned best = 0;
                for (unsigned i = 0; i < nsub; i++)
                {
                    unsigned off = 2 + i * 12;
                    if (off + 8 > (unsigned) nr) break;
                    unsigned mn = rb[off]   | (rb[off+1]<<8) | (rb[off+2]<<16) | (rb[off+3]<<24);
                    unsigned mx = rb[off+4] | (rb[off+5]<<8) | (rb[off+6]<<16) | (rb[off+7]<<24);
                    if (48000 >= mn && 48000 <= mx) has48  = TRUE;
                    if (44100 >= mn && 44100 <= mx) has441 = TRUE;
                    if (mx > best && mx <= 192000)  best   = mx;
                }
                chosen = has48 ? 48000 : has441 ? 44100 : (best ? best : 48000);
            }
            m_uRate = chosen;

            // SET_CUR the chosen rate (4-byte LE) via the Clock Source entity.
            m_OutBuf[0] = (u8)(chosen & 0xFF);       m_OutBuf[1] = (u8)((chosen >> 8) & 0xFF);
            m_OutBuf[2] = (u8)((chosen >> 16) & 0xFF); m_OutBuf[3] = (u8)((chosen >> 24) & 0xFF);
            int r = GetHost ()->ControlMessage (GetEndpoint0 (),
                        0x21, 0x01, (u16) (0x01 << 8), wIndex, m_OutBuf, 4);

            // GET_CUR readback to confirm the device accepted it.
            unsigned got = 0;
            int gr = GetHost ()->ControlMessage (GetEndpoint0 (),
                        0xA1, 0x01, (u16) (0x01 << 8), wIndex, m_InBuf, 4);
            if (gr >= 4)
            {
                const u8 *gb = (const u8 *) m_InBuf;
                got = gb[0] | (gb[1]<<8) | (gb[2]<<16) | (gb[3]<<24);
                if (got) m_uRate = got;
            }
            // Seed the async-OUT feedback rate from the negotiated rate assuming a
            // high-speed bInterval=1 endpoint (8000 service intervals/s); the
            // explicit feedback endpoint refines this per-device within the first
            // few ms. Q16.16 frames per service interval.
            m_fbRate = (u32) (((u64) m_uRate << 16) / 8000);
            if (m_fbRate == 0) m_fbRate = 6u << 16;

            CLogger::Get ()->Write (FromAudio, LogNotice,
                "UAC2 clock=%u if=%d set=%u rc=%d got=%u fmt=%uch/%ubit",
                (unsigned) info.clockId, info.acInterfaceNum, chosen, r, got,
                m_uChannels, m_uSubslot * 8);
        }
    }
    CLogger::Get ()->Write (FromAudio, LogNotice, "UAC2 alt selected (if %d sub=%u ch=%u)",
        GetInterfaceNumber (), m_uSubslot, m_uChannels);
    return TRUE;
}

void CUSBAudioDevice::RegisterInHandler (TAudioInHandler *pHandler)
{
    m_pInHandler = pHandler;
}

void CUSBAudioDevice::RegisterOutHandler (TAudioOutHandler *pHandler)
{
    m_pOutHandler = pHandler;
}

boolean CUSBAudioDevice::StartInRequest (unsigned slot)
{
    assert (m_pEndpointIn != 0);
    assert (slot < 2);
    assert (m_pInURB[slot] == 0);

    // Double-buffer: each slot owns half of m_InBuf so the two in-flight URBs never
    // share a buffer (the xHCI may DMA both concurrently).
    u8 *inBuf = (u8 *) m_InBuf + slot * USB_AUDIO_INBUF_BYTES;

    u16 usMaxPacket = (u16) m_pEndpointIn->GetMaxPacketSize ();
    if (usMaxPacket > USB_AUDIO_INBUF_BYTES) usMaxPacket = USB_AUDIO_INBUF_BYTES;

    extern volatile unsigned g_audioInSubmitFail;

    // UAC2 IN: carry MULTIPLE iso packets (microframes) in ONE URB, mirroring the
    // OUT-side fix (StartOutRequest) for the identical root cause: a single-packet
    // URB's completion rate falls slightly short of the true microframe rate under
    // per-URB re-arm overhead on a high-speed device, silently dropping microframes
    // of CAPTURED audio (an AIR 192|4-class device, not exercised by the lower-
    // bandwidth US-2x2/UCA222 IN paths). Batching N microframes/URB drops the
    // completion rate to 8000/N/s (easily sustained) while the xHCI still receives
    // one packet per microframe on the wire -- no microframe missing.
    //
    // InCompletion does NOT use pURB->GetResultLength() to size the parse -- the
    // vendored Circle DWHCI driver's GetResultLength() is UNUSABLE for multi-
    // packet iso URBs (dwhcixferstagedata.cpp's per-packet TransactionComplete
    // loop reassigns m_nTransferSize to each packet's OWN declared size as it
    // advances, never restored to the full-URB total, so GetResultLen() at
    // stage-complete clamps to roughly ONE packet's worth -- neither a safe
    // lower nor upper bound on the true accumulated transfer). The buffer IS
    // fully and correctly written (m_pBufferPointer advances by the real
    // per-packet byte count during the transfer); only GetResultLength()'s
    // REPORTED total is wrong. Fix: InCompletion trusts m_nInSubmitBytes[slot]
    // (recorded here) instead.
    //
    // Trusting a claimed byte count blindly is only safe if it's unlikely to
    // exceed what the device actually delivered -- so pktSize (the DECLARED
    // per-microframe iso packet size, which bounds what the hardware will
    // accept from the device -- must stay at usMaxPacket, the endpoint's true
    // max, or a legitimate full-rate delivery would overrun/error) is kept
    // separate from nomPktSize (the NOMINAL expected bytes/microframe at the
    // negotiated rate, used only to size m_nInSubmitBytes -- the count
    // InCompletion trusts). A real UAC2 IN device's actual per-microframe
    // delivery fluctuates in a narrow band AROUND its nominal rate (clock
    // tolerance, not free variation down to zero), so claiming the NOMINAL
    // total instead of the MAXIMUM total bounds the worst-case over-read (when
    // a real microframe delivers less than claimed) to a few stale samples
    // instead of up to a whole packet's worth -- this was the root cause of
    // buzz/distortion on US-2x2 (nomPktSize==usMaxPacket in the prior attempt
    // meant every short microframe read a maximally-oversized garbage tail).
    if (m_bUAC2)
    {
        const unsigned N = 8;   // microframes per URB (1ms; matches StartOutRequest's N)
        u16 pktSize = usMaxPacket;   // hardware-declared iso packet size, unchanged
        if (pktSize == 0) pktSize = 1;
        unsigned nPkts = USB_AUDIO_INBUF_BYTES / pktSize;
        if (nPkts > N) nPkts = N;
        if (nPkts < 1) nPkts = 1;

        // Nominal bytes/microframe at the negotiated rate, accumulated in Q16.16
        // across calls (mirrors m_fbAccum's proven OUT-pacing pattern) so the
        // LONG-RUN average is exact -- a single per-call truncated estimate
        // (e.g. 6 vs true 5.5125 samples/microframe at 44100Hz) is a SYSTEMATIC
        // bias that accumulates in the IN ring fast enough to overwhelm
        // AudioSystem.cpp's drain hysteresis deadband, reproducing the periodic
        // ~1s oscillate-and-resync cycle that deadband exists to prevent
        // (audible as periodic crunch+blips on top of the per-completion buzz).
        // nomRate is frames-per-microframe in Q16.16, same formula as m_fbRate's
        // seed (ConfigureUAC2: m_fbRate = (m_uRate<<16)/8000). frameBytes is
        // bytes per sample-frame (all channels).
        unsigned frameBytes = m_uSubslot * m_uChannels;
        if (frameBytes == 0) frameBytes = 4;
        // Bias the claimed rate slightly BELOW the true nominal rather than
        // claiming the exact mean, so a real microframe delivering LESS than
        // the mean (clock-tolerance jitter, not free variation) doesn't cause
        // InCompletion to over-read into stale buffer tail. But the bias must
        // stay SMALLER than input_usb.cpp's drift corrector can absorb, or the
        // "fix" just becomes a new, permanent, guaranteed rate deficit instead
        // of an occasional one -- which is what 8134/8192 (~0.71% low) did.
        // input_usb.cpp's corrector (IN_TARGET_LAG=96, IN_DEADBAND=48,
        // IN_RATE_MAX_DEV=64/IN_RATE_GAIN=131072) can only speed up the read
        // rate by at most 64*65536/131072 = 0.049% once outside the deadband,
        // and only has IN_TARGET_LAG-AUDIO_BLOCK_SAMPLES=32 samples of slack
        // before a hard resync. A continuous 0.71% deficit (~14x the
        // corrector's max absorb rate) drains that 32-sample slack in ~0.1s of
        // real time, forcing a resync roughly every cycle -- reproducing the
        // very periodic crunch+blips pattern this bias was meant to eliminate,
        // now driven by a controlled deficit instead of overshoot (both
        // audible, both cyclical). Fix: shrink the bias to 65500/65536
        // (~0.055%), safely under the corrector's 0.049%-per-step absorb
        // ceiling AND under typical USB clock tolerance (~0.25%), so it still
        // clears the overwhelming majority of real microframes (avoiding the
        // over-read) while the residual deficit is small enough for the
        // existing drift correction to track continuously instead of
        // resyncing. Any genuinely short microframe past this narrower margin
        // still falls back to input_usb.cpp's per-sample underrun handling
        // (repeat-last-sample, already inaudible for isolated single-sample
        // events -- see input_usb.cpp:205-207).
        u32 nomRate = (u32) (((u64) m_uRate << 16) * 65500 / 65536 / 8000);
        if (nomRate == 0) nomRate = 1u << 16;
        unsigned nomSamplesTotal = 0;
        for (unsigned k = 0; k < nPkts; k++)
        {
            m_inNomAccum += nomRate;
            nomSamplesTotal += m_inNomAccum >> 16;
            m_inNomAccum &= 0xFFFF;
        }
        unsigned nomTotalBytes = nomSamplesTotal * frameBytes;
        unsigned declaredTotalBytes = (unsigned) pktSize * nPkts;
        if (nomTotalBytes > declaredTotalBytes) nomTotalBytes = declaredTotalBytes;   // never claim more than declared
        if (nomTotalBytes == 0) nomTotalBytes = declaredTotalBytes;

        m_pInURB[slot] = new CUSBRequest (m_pEndpointIn, inBuf, pktSize * nPkts);
        assert (m_pInURB[slot] != 0);
        for (unsigned k = 0; k < nPkts; k++) m_pInURB[slot]->AddIsoPacket (pktSize);
        m_pInURB[slot]->SetCompletionRoutine (InStub, 0, this);
        // m_nInSubmitBytes uses the NOMINAL total (fractionally accumulated,
        // exact long-run average) not pktSize*nPkts (the declared/allocated
        // maximum) so InCompletion never over-claims by more than the device's
        // real clock-tolerance jitter around its nominal rate.
        m_nInSubmitBytes[slot] = nomTotalBytes;
        boolean ok = GetHost ()->SubmitAsyncRequest (m_pInURB[slot]);
        if (!ok) g_audioInSubmitFail++;
        return ok;
    }

    // UAC1 (UCA222) legacy single-packet path, unchanged. GetResultLength() is
    // reliable here (single AddIsoPacket -> the multi-packet clamp bug above
    // never engages), but m_nInSubmitBytes is still recorded for uniform
    // handling in InCompletion.
    m_pInURB[slot] = new CUSBRequest (m_pEndpointIn, inBuf, usMaxPacket);
    assert (m_pInURB[slot] != 0);
    m_pInURB[slot]->AddIsoPacket (usMaxPacket);
    m_pInURB[slot]->SetCompletionRoutine (InStub, 0, this);
    m_nInSubmitBytes[slot] = usMaxPacket;
    boolean ok = GetHost ()->SubmitAsyncRequest (m_pInURB[slot]);
    if (!ok) g_audioInSubmitFail++;
    return ok;
}

boolean CUSBAudioDevice::StartOutRequest (unsigned slot)
{
    assert (m_pEndpointOut != 0);
    assert (slot < 2);
    assert (m_pOutURB[slot] == 0);

    // Double-buffer: each slot owns half of m_OutBuf so the two in-flight URBs never
    // share a buffer (the xHCI may DMA both concurrently).
    u8 *outBuf = (u8 *) m_OutBuf + slot * USB_AUDIO_OUTBUF_BYTES;

    u16 usMaxPacket = (u16) m_pEndpointOut->GetMaxPacketSize ();
    if (usMaxPacket > USB_AUDIO_OUTBUF_BYTES) usMaxPacket = USB_AUDIO_OUTBUF_BYTES;
    unsigned frameBytes = m_uSubslot * m_uChannels;
    if (frameBytes == 0) frameBytes = 4;
    const unsigned cap = USB_AUDIO_BLOCK_BYTES / 4;

    extern volatile unsigned g_audioOutDeliv, g_audioOutPeak, g_audioOutSubmitFail;

    // UAC2 async OUT: carry MULTIPLE iso packets (microframes) in ONE URB. ROOT CAUSE
    // of the Tascam line-out glitch: the single-URB completion rate was ~7955/s, NOT
    // 8000/s -- the heavy per-URB pack work made the re-arm miss ~45 microframes/s on
    // the high-speed device (the IN does 8006/s). Each missed microframe is ~6 lost
    // samples; the ~270 samples/s deficit drained the DAC's small FIFO every ~0.85s ->
    // periodic dropout (silence) on the analog line-out (the digital stream was clean;
    // the device simply starved). The full-speed UCA222 (1ms re-arm window) never fell
    // behind, so it never showed this. Fix (buffer math, no added latency): batch N
    // microframes per URB so the completion rate drops to 8000/N/s (easily sustained)
    // while the xHCI still emits one packet per microframe -> no microframe missing.
    // Each packet stays feedback-paced (the device's requested rate).
    if (m_bUAC2 && m_pEndpointFb)
    {
        const unsigned N = 8;                                  // microframes per URB (1ms; 1000 URB/s = the glitch-free full-speed UCA222 cadence)
        unsigned maxPerPkt = usMaxPacket / frameBytes;
        unsigned bufPerPkt = (USB_AUDIO_OUTBUF_BYTES / N) / frameBytes;
        if (maxPerPkt > bufPerPkt) maxPerPkt = bufPerPkt;
        u8 *pb = outBuf;
        unsigned pkt[N];
        unsigned total = 0;
        // Final residual: the device's feedback value is ~static and ~0.02% off its
        // true DAC rate, so feedback-pacing alone slowly drifted the DAC FIFO -> one
        // dropout every ~20s. The engine writes the OUT ring at the device's IN clock,
        // which EQUALS its DAC clock (one device clock), so a feedback-vs-DAC mismatch
        // shows up as the OUT-ring level drifting -- observable. A gentle integral on
        // the ring level biases the send rate so the ring (hence the DAC FIFO) holds,
        // converging the long-run send rate to the true DAC rate. Double-buffering keeps
        // the ring stable enough that this integral converges without windup.
        extern unsigned AudioOutputUSB_outAvail (void);
        static int s_outBias = 0;                              // Q16.16 added to feedback rate
        int avail = (int) AudioOutputUSB_outAvail ();
        // Only integrate when the OUT ring has data. At boot the DSP has not
        // ticked yet so avail=0; integrating at (0-256)>>3=-32/URB winds
        // s_outBias to -biasLim in ~245 URBs (~245ms) reducing effRate ~2%
        // and causing DAC underruns before the first sample arrives.
        // Hold s_outBias=0 while the ring is empty so effRate stays nominal.
        if (avail > 0)
            s_outBias += (avail - 256) >> 3;                   // gentle integral toward avail=256
        else
            s_outBias = 0;
        int biasLim = (int) (m_fbRate / 50);                   // clamp to ~2%
        if (s_outBias >  biasLim) s_outBias =  biasLim;
        if (s_outBias < -biasLim) s_outBias = -biasLim;
        u32 effRate = (u32) ((int) m_fbRate + s_outBias);
        for (unsigned k = 0; k < N; k++)
        {
            m_fbAccum += effRate;
            unsigned ns = m_fbAccum >> 16;
            m_fbAccum &= 0xFFFF;
            if (ns > maxPerPkt) ns = maxPerPkt;
            unsigned bytes = ns * frameBytes;
            if (m_pOutHandler && ns > 0)
            {
                s16 lb[cap], rb[cap];
                (*m_pOutHandler) (lb, rb, ns);
                if (m_uChannels > 2) memset (pb, 0, bytes);
                for (unsigned i = 0; i < ns; i++)
                {
                    u8 *f = pb + i * frameBytes;
                    uac2FromS16 (f, m_uSubslot, lb[i]);
                    if (m_uChannels > 1) uac2FromS16 (f + m_uSubslot, m_uSubslot, rb[i]);
                    s16 v = lb[i] < 0 ? (s16) -lb[i] : lb[i];
                    if ((unsigned) v > g_audioOutPeak) g_audioOutPeak = (unsigned) v;
                }
            }
            else
            {
                memset (pb, 0, bytes);
            }
            pkt[k] = bytes;
            pb    += bytes;
            total += bytes;
        }
        g_audioOutDeliv++;
        m_pOutURB[slot] = new CUSBRequest (m_pEndpointOut, outBuf, total);
        assert (m_pOutURB[slot] != 0);
        for (unsigned k = 0; k < N; k++) m_pOutURB[slot]->AddIsoPacket (pkt[k]);
        m_pOutURB[slot]->SetCompletionRoutine (OutStub, 0, this);
        boolean ok = GetHost ()->SubmitAsyncRequest (m_pOutURB[slot]);
        if (!ok) g_audioOutSubmitFail++;
        return ok;
    }

    // UAC1 (UCA222) legacy single-packet, fixed maxPacket fill.
    unsigned nSamples = usMaxPacket / frameBytes;
    if (nSamples > cap) nSamples = cap;
    if (nSamples * frameBytes > usMaxPacket) nSamples = usMaxPacket / frameBytes;
    unsigned bytes = nSamples * frameBytes;

    if (m_pOutHandler && nSamples > 0)
    {
        s16 left_buf[cap], right_buf[cap];
        (*m_pOutHandler) (left_buf, right_buf, nSamples);
        u8 *pb = outBuf;
        if (m_uChannels > 2) memset (pb, 0, bytes);
        g_audioOutDeliv++;
        for (unsigned i = 0; i < nSamples; i++)
        {
            u8 *f = pb + i * frameBytes;
            uac2FromS16 (f, m_uSubslot, left_buf[i]);
            if (m_uChannels > 1) uac2FromS16 (f + m_uSubslot, m_uSubslot, right_buf[i]);
            s16 v = left_buf[i] < 0 ? (s16) -left_buf[i] : left_buf[i];
            if ((unsigned) v > g_audioOutPeak) g_audioOutPeak = (unsigned) v;
        }
    }
    else
    {
        memset (outBuf, 0, bytes ? bytes : frameBytes);
        if (!bytes) bytes = frameBytes;
    }

    m_pOutURB[slot] = new CUSBRequest (m_pEndpointOut, outBuf, bytes);
    assert (m_pOutURB[slot] != 0);
    m_pOutURB[slot]->AddIsoPacket (bytes);
    m_pOutURB[slot]->SetCompletionRoutine (OutStub, 0, this);
    boolean ok = GetHost ()->SubmitAsyncRequest (m_pOutURB[slot]);
    if (!ok) g_audioOutSubmitFail++;
    return ok;
}

boolean CUSBAudioDevice::StartFbRequest (void)
{
    assert (m_pEndpointFb != 0);
    assert (m_pFbURB == 0);

    u16 usPacketSize = (u16) m_pEndpointFb->GetMaxPacketSize ();
    if (usPacketSize > sizeof m_FbBuf) usPacketSize = sizeof m_FbBuf;

    m_pFbURB = new CUSBRequest (m_pEndpointFb, m_FbBuf, usPacketSize);
    assert (m_pFbURB != 0);
    m_pFbURB->AddIsoPacket (usPacketSize);
    m_pFbURB->SetCompletionRoutine (FbStub, 0, this);
    return GetHost ()->SubmitAsyncRequest (m_pFbURB);
}

void CUSBAudioDevice::FbCompletion (CUSBRequest *pURB)
{
    assert (pURB == m_pFbURB);
    // Explicit feedback value: the device's desired data rate in frames per
    // (micro)frame. High-speed = 4 bytes Q16.16; full-speed = 3 bytes Q10.14
    // (shift left 2 to normalise to Q16.16). LE.
    if (pURB->GetStatus () && pURB->GetResultLength () >= 3)
    {
        unsigned n = pURB->GetResultLength ();
        const u8 *p = (const u8 *) m_FbBuf;
        u32 v;
        if (n >= 4) v = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
        else        v = ((u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16)) << 2;  // 10.14 -> 16.16
        // Sanity-clamp to [1, cap) frames so a garbage feedback can't overrun.
        u32 maxRate = (USB_AUDIO_BLOCK_BYTES / 4) << 16;
        if (v > 0 && v < maxRate) m_fbRate = v;
    }
    extern volatile unsigned g_audioFbRate, g_audioFbCount;
    g_audioFbRate = m_fbRate;
    g_audioFbCount++;
    delete m_pFbURB;
    m_pFbURB = 0;
    StartFbRequest ();
}

void CUSBAudioDevice::InCompletion (CUSBRequest *pURB)
{
    assert (pURB != 0);
    unsigned slot = (pURB == m_pInURB[1]) ? 1 : 0;
    assert (pURB == m_pInURB[slot]);

    unsigned frameBytes = m_uSubslot * m_uChannels;   // UAC1: 2*2 = 4
    if (frameBytes == 0) frameBytes = 4;
    // nSamples source is gated on m_bUAC2, NOT uniform. For a single-packet URB
    // (UAC1/UCA222), pURB->GetResultLength() is CORRECT and VARIABLE -- a real
    // full-speed synchronous isochronous endpoint commonly alternates packet
    // sizes frame-to-frame (e.g. N-1 packets of size S, 1 of size S-frameBytes)
    // to average a non-integer samples/frame rate over time; GetResultLength()
    // reports the TRUE per-frame byte count each completion (single AddIsoPacket
    // per URB -- there is no multi-packet ambiguity to get wrong here).
    // Using a CONSTANT m_nInSubmitBytes[slot]=usMaxPacket here instead (an
    // earlier attempt at this fix) was itself a regression: it read a fixed
    // usMaxPacket-sized chunk every completion regardless of what the device
    // actually sent that frame, pulling in stale/uninitialized buffer tail on
    // every short frame -- continuous distortion/buzz on UAC1 (UCA222), the
    // device this project's whole USB-audio history proves worked perfectly
    // with plain GetResultLength() before this session touched the file.
    //
    // For UAC2 (multi-packet), GetResultLength() is NOT TRUSTED, on this
    // hardware's ACTUAL host controller path -- Raspberry Pi 4 uses XHCI
    // (USB 3.0 controller), not DWHCI (USB 2.0, older Pi models only);
    // confirmed via arm-none-eabi-nm on the built kernel7l.elf showing
    // CDWHCITransferStageData symbols entirely absent from the link. An
    // earlier version of this comment cited dwhcixferstagedata.cpp's
    // per-packet TransactionComplete/GetResultLen mechanics as the cause --
    // that analysis was of a driver file this build never compiles; it does
    // not apply here. The REAL mechanism on XHCI (lib/usb/xhciendpoint.cpp
    // CXHCIEndpoint::TransferEvent/EnqueueTRB): each iso packet in a batched
    // URB is its own independent TD (no chain bit set), with IOC (interrupt-
    // on-completion) set ONLY on the LAST packet's TRB -- so exactly one
    // transfer event fires per URB, reporting the XHCI hardware's residual
    // length for ONLY that final TRB. SetResultLen(nBufLen - lastResidual)
    // therefore silently assumes every packet BEFORE the last delivered its
    // full declared size, with no way to detect an early short packet --
    // an OVER-report risk (claims more than was actually received), the
    // opposite failure shape from a truncation bug. m_nInSubmitBytes[slot]
    // (what StartInRequest submitted, sized at the NOMINAL expected bytes/
    // microframe rather than the protocol's declared MAXIMUM) sidesteps this
    // blind spot entirely by never reading GetResultLength() for UAC2 at all:
    // a real device's actual per-microframe delivery fluctuates in a narrow
    // band around its nominal rate, so claiming the nominal-sized total reads
    // at most a few stale samples per completion on the rare short microframe,
    // instead of trusting a hardware result field that cannot see a short
    // packet anywhere but the very last one in the batch. See pktSize
    // computation in StartInRequest for the nominal-rate sizing.
    unsigned nSourceBytes = m_bUAC2 ? m_nInSubmitBytes[slot]
                                     : (pURB->GetStatus () ? pURB->GetResultLength () : 0);
    if (pURB->GetStatus () && nSourceBytes >= frameBytes && m_pInHandler != 0)
    {
        // cap sized for the batched UAC2 buffer (USB_AUDIO_INBUF_BYTES worth of
        // minimum-1-byte-subslot mono frames); the legacy UAC1 single-packet path
        // never approaches this bound.
        //
        // UAC2 multi-packet: on this hardware's actual XHCI path, GetResultLength()
        // reflects only the LAST packet's hardware residual in the batch (see
        // InCompletion's top comment) -- it cannot see a short packet anywhere
        // else in the batch, so trusting it risks OVER-claiming into stale
        // buffer tail on any non-last short microframe, with no way to detect
        // the fault. Fix: use m_nInSubmitBytes[slot] (what StartInRequest
        // submitted, sized at the nominal expected rate) for UAC2 only, so the
        // claim is bounded by what we asked for, not by a hardware field that
        // is blind to all but the very last packet's actual delivery.
        // Tradeoff: cannot detect a genuinely short/zero-length packet mid-batch
        // (assumes full-size packets) -- accepted for UAC2 since GetResultLength()
        // cannot reliably detect it either; same assumption OUT already makes
        // for its own multi-packet buffer. UAC1 never uses this path.
        //
        // left_buf/right_buf are STATIC, not stack-allocated: this handler runs
        // in the USB completion-handler/ISR context (Core 0 hard-RT dispatch),
        // where stack budget is small and unverified. cap=USB_AUDIO_INBUF_BYTES/4
        // (512 samples) x 2 buffers x 2 bytes = 2KB -- fine as static storage,
        // fatal as a per-call stack frame (caused total audio loss on real
        // hardware: an 8x stack-frame blowup applied to EVERY completion on
        // EVERY device, not just the UAC2 batched path, overran the completion-
        // handler stack and crashed the whole system). Per-slot arrays because
        // slot 0/1 completions can interleave on UAC2 double-buffering, and
        // (*m_pInHandler) is called synchronously before this function returns
        // (no concurrent reentry into the SAME slot's buffer is possible: a URB
        // is deleted+resubmitted only after its own completion finishes).
        const unsigned cap = USB_AUDIO_INBUF_BYTES / 4;
        static s16 s_left_buf[2][cap];
        static s16 s_right_buf[2][cap];
        s16 *left_buf  = s_left_buf[slot];
        s16 *right_buf = s_right_buf[slot];
        unsigned nSamples = nSourceBytes / frameBytes;
        if (nSamples > cap) nSamples = cap;
        const u8 *pb = (const u8 *) m_InBuf + slot * USB_AUDIO_INBUF_BYTES;
        // Zero-crossing accumulators, per-slot so interleaved slot 0/1
        // completions (UAC2 double-buffering) never race the same static
        // "previous sample" state -- each slot tracks its own continuation.
        static s16 s_zcPrevL[2] = {0, 0};
        static s16 s_zcPrevR[2] = {0, 0};
        s16 prevL = s_zcPrevL[slot];
        s16 prevR = s_zcPrevR[slot];
        unsigned zcL = 0, zcR = 0;
        for (unsigned i = 0; i < nSamples; i++)
        {
            const u8 *f = pb + i * frameBytes;
            s16 L = uac2ToS16 (f, m_uSubslot);
            s16 R = (m_uChannels > 1) ? uac2ToS16 (f + m_uSubslot, m_uSubslot) : L;
            left_buf[i]  = L;
            right_buf[i] = R;
            u32 absL = L < 0 ? (u32)(-L) : (u32)L;
            u32 absR = R < 0 ? (u32)(-R) : (u32)R;
            if (absL > m_nPeakIn) m_nPeakIn = absL;
            if (absR > m_nPeakIn) m_nPeakIn = absR;
            if ((L >= 0) != (prevL >= 0)) zcL++;
            if ((R >= 0) != (prevR >= 0)) zcR++;
            prevL = L;
            prevR = R;
        }
        s_zcPrevL[slot] = prevL;
        s_zcPrevR[slot] = prevR;
        (*m_pInHandler) (left_buf, right_buf, nSamples);
        // Reliable cross-core witnesses for the UAUD verb (input is flowing).
        extern volatile unsigned g_audioInDeliv, g_audioInPeak;
        extern volatile unsigned g_audioInZcL, g_audioInZcR;
        g_audioInDeliv++;
        if (m_nPeakIn > g_audioInPeak) g_audioInPeak = m_nPeakIn;
        g_audioInZcL += zcL;
        g_audioInZcR += zcR;
    }

    // Glitch diag: track the MAX gap between IN completions, mirroring
    // OutCompletion's g_audioOutMaxGapUs -- the original diagnostic signal that
    // exposed the OUT-side microframe-miss bug, now available on IN too.
    extern volatile unsigned g_audioInMaxGapUs, g_audioInLastTick;
    u32 nowt = CTimer::GetClockTicks ();
    if (g_audioInLastTick) { u32 gap = nowt - g_audioInLastTick;
        if (gap > g_audioInMaxGapUs) g_audioInMaxGapUs = gap; }
    g_audioInLastTick = nowt;

    // Re-arm THIS slot; when UAC2-double-buffered, the OTHER slot is still in
    // flight, so the iso pipe never empties (no re-arm gap -> no missing
    // microframe). UAC1 has only slot 0 armed (slot 1 stays null, per Configure).
    delete m_pInURB[slot];
    m_pInURB[slot] = 0;
    StartInRequest (slot);
}

void CUSBAudioDevice::OutCompletion (CUSBRequest *pURB)
{
    assert (pURB != 0);
    unsigned slot = (pURB == m_pOutURB[1]) ? 1 : 0;
    assert (pURB == m_pOutURB[slot]);
    // Glitch diag: track the MAX gap between OUT completions.
    extern volatile unsigned g_audioOutMaxGapUs, g_audioOutLastTick;
    u32 nowt = CTimer::GetClockTicks ();
    if (g_audioOutLastTick) { u32 gap = nowt - g_audioOutLastTick;
        if (gap > g_audioOutMaxGapUs) g_audioOutMaxGapUs = gap; }
    g_audioOutLastTick = nowt;
    // Re-arm THIS slot; the OTHER slot is still in flight, so the iso pipe never
    // empties (no re-arm gap -> no missing microframe).
    delete m_pOutURB[slot];
    m_pOutURB[slot] = 0;
    StartOutRequest (slot);
}

void CUSBAudioDevice::InStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
    CUSBAudioDevice *pThis = (CUSBAudioDevice *) pContext;
    assert (pThis != 0);
    pThis->InCompletion (pURB);
}

void CUSBAudioDevice::OutStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
    CUSBAudioDevice *pThis = (CUSBAudioDevice *) pContext;
    assert (pThis != 0);
    pThis->OutCompletion (pURB);
}

void CUSBAudioDevice::FbStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
    CUSBAudioDevice *pThis = (CUSBAudioDevice *) pContext;
    assert (pThis != 0);
    pThis->FbCompletion (pURB);
}
