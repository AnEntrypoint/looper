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
    m_pInURB       (0),
    m_pOutURB      (0),
    m_pFbURB       (0),
    m_fbRate       (6u << 16),   // nominal 48000/8000 (high-speed) until feedback
    m_fbAccum      (0),
    m_nPeakIn      (0),
    m_nLastMonitorTick (0),
    m_bUAC2        (FALSE),
    m_uRate        (48000),
    m_uSubslot     (2),
    m_uChannels    (2)
{
}

CUSBAudioDevice::~CUSBAudioDevice (void)
{
    delete m_pEndpointIn;
    delete m_pEndpointOut;
    delete m_pEndpointFb;
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

    if (m_pEndpointIn && !s_pThis)
    {
        s_pThis = this;
        g_audioInBound  = 1;            // mirror for the cross-core UAUD verb
        g_audioUAC2     = m_bUAC2 ? 1 : 0;
        g_audioChannels = m_uChannels;
        g_audioSubslot  = m_uSubslot;
        g_audioRate     = m_uRate;
    }
    if (m_pEndpointOut && !s_pOut)
    {
        s_pOut = this;
        g_audioOutBound = 1;
    }

    CLogger::Get ()->Write (FromAudio, LogNotice, "USB audio configured (in=%s out=%s)",
        m_pEndpointIn ? "yes" : "no", m_pEndpointOut ? "yes" : "no");

    if (m_pEndpointIn)
        StartInRequest ();
    if (m_pEndpointOut)
        StartOutRequest ();
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

boolean CUSBAudioDevice::StartInRequest (void)
{
    assert (m_pEndpointIn != 0);
    assert (m_pInURB == 0);

    u16 usPacketSize = (u16) m_pEndpointIn->GetMaxPacketSize ();
    if (usPacketSize > sizeof m_InBuf) usPacketSize = sizeof m_InBuf;

    m_pInURB = new CUSBRequest (m_pEndpointIn, m_InBuf, usPacketSize);
    assert (m_pInURB != 0);
    m_pInURB->AddIsoPacket (usPacketSize);
    m_pInURB->SetCompletionRoutine (InStub, 0, this);
    return GetHost ()->SubmitAsyncRequest (m_pInURB);
}

boolean CUSBAudioDevice::StartOutRequest (void)
{
    assert (m_pEndpointOut != 0);
    assert (m_pOutURB == 0);

    u16 usMaxPacket = (u16) m_pEndpointOut->GetMaxPacketSize ();
    if (usMaxPacket > sizeof m_OutBuf) usMaxPacket = sizeof m_OutBuf;
    unsigned frameBytes = m_uSubslot * m_uChannels;
    if (frameBytes == 0) frameBytes = 4;
    const unsigned cap = USB_AUDIO_BLOCK_BYTES / 4;

    // Frames to send THIS packet. For UAC2 async OUT, pace by the feedback rate
    // (Q16.16 accumulator) so we send exactly what the device's clock wants;
    // sending the full maxPacket every microframe over-feeds it ~2x and the FIFO
    // overflows -> silence. UAC1 keeps the legacy fixed maxPacket fill.
    unsigned nSamples;
    if (m_bUAC2 && m_pEndpointFb)
    {
        m_fbAccum += m_fbRate;
        nSamples = m_fbAccum >> 16;
        m_fbAccum &= 0xFFFF;
    }
    else
    {
        nSamples = usMaxPacket / frameBytes;
    }
    if (nSamples > cap) nSamples = cap;
    if (nSamples * frameBytes > usMaxPacket) nSamples = usMaxPacket / frameBytes;
    unsigned bytes = nSamples * frameBytes;

    if (m_pOutHandler && nSamples > 0)
    {
        s16 left_buf[cap], right_buf[cap];
        (*m_pOutHandler) (left_buf, right_buf, nSamples);
        u8 *pb = (u8 *) m_OutBuf;
        // Multi-channel device (>2 out): zero the frame so unused channels carry
        // silence, not stale bytes. We only drive L (ch0) + R (ch1).
        if (m_uChannels > 2) memset (pb, 0, bytes);
        extern volatile unsigned g_audioOutDeliv, g_audioOutPeak;
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
        memset (m_OutBuf, 0, bytes ? bytes : frameBytes);
        if (!bytes) bytes = frameBytes;
    }

    m_pOutURB = new CUSBRequest (m_pEndpointOut, m_OutBuf, bytes);
    assert (m_pOutURB != 0);
    m_pOutURB->AddIsoPacket (bytes);
    m_pOutURB->SetCompletionRoutine (OutStub, 0, this);
    boolean ok = GetHost ()->SubmitAsyncRequest (m_pOutURB);
    if (!ok)
    {
        extern volatile unsigned g_audioOutSubmitFail;
        g_audioOutSubmitFail++;
    }
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
    assert (pURB == m_pInURB);

    unsigned frameBytes = m_uSubslot * m_uChannels;   // UAC1: 2*2 = 4
    if (frameBytes == 0) frameBytes = 4;
    if (pURB->GetStatus () && pURB->GetResultLength () >= frameBytes && m_pInHandler != 0)
    {
        const unsigned cap = USB_AUDIO_BLOCK_BYTES / 4;
        unsigned nSamples = pURB->GetResultLength () / frameBytes;
        if (nSamples > cap) nSamples = cap;
        const u8 *pb = (const u8 *) m_InBuf;
        s16 left_buf[cap], right_buf[cap];
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
        }
        (*m_pInHandler) (left_buf, right_buf, nSamples);
        // Reliable cross-core witnesses for the UAUD verb (input is flowing).
        extern volatile unsigned g_audioInDeliv, g_audioInPeak;
        g_audioInDeliv++;
        if (m_nPeakIn > g_audioInPeak) g_audioInPeak = m_nPeakIn;
    }

    delete m_pInURB;
    m_pInURB = 0;
    StartInRequest ();
}

void CUSBAudioDevice::OutCompletion (CUSBRequest *pURB)
{
    assert (pURB != 0);
    assert (pURB == m_pOutURB);
    // The audio pull + 24-bit pack + feedback-paced sizing all live in
    // StartOutRequest now; the completion just frees the URB and re-arms.
    delete m_pOutURB;
    m_pOutURB = 0;
    StartOutRequest ();
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
