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

CUSBAudioDevice::CUSBAudioDevice (CUSBFunction *pFunction)
:   CUSBFunction (pFunction),
    m_pEndpointIn  (0),
    m_pEndpointOut (0),
    m_pInHandler   (0),
    m_pOutHandler  (0),
    m_pInURB       (0),
    m_pOutURB      (0),
    m_nPeakIn      (0),
    m_nLastMonitorTick (0),
    m_bUAC2        (FALSE),
    m_uSubslot     (2),
    m_uChannels    (2)
{
}

CUSBAudioDevice::~CUSBAudioDevice (void)
{
    delete m_pEndpointIn;
    delete m_pEndpointOut;
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
        // Skip a UAC2 explicit feedback endpoint (iso, usage type 0x10): it is
        // IN-direction but carries rate feedback, not audio -- grabbing it as the
        // data endpoint would submit audio URBs on the wrong pipe.
        if ((pDesc->bmAttributes & 0x30) == 0x10) continue;

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
            // 48000 = 0x0000BB80, little-endian, in a DMA-capable buffer.
            m_OutBuf[0] = 0x80; m_OutBuf[1] = 0xBB; m_OutBuf[2] = 0x00; m_OutBuf[3] = 0x00;
            int r = GetHost ()->ControlMessage (GetEndpoint0 (),
                        0x21,                                    // OUT | class | interface
                        0x01,                                    // CUR
                        (u16) (0x01 << 8),                       // CS_SAM_FREQ_CONTROL, ch 0
                        (u16) ((info.clockId << 8) | info.acInterfaceNum),
                        m_OutBuf, 4);
            CLogger::Get ()->Write (FromAudio, LogNotice,
                "UAC2 set 48000 clock=%u if=%d rc=%d fmt=%uch/%ubit",
                (unsigned) info.clockId, info.acInterfaceNum, r,
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

    u16 usPacketSize = (u16) m_pEndpointOut->GetMaxPacketSize ();
    if (usPacketSize > sizeof m_OutBuf) usPacketSize = sizeof m_OutBuf;

    m_pOutURB = new CUSBRequest (m_pEndpointOut, m_OutBuf, usPacketSize);
    assert (m_pOutURB != 0);
    m_pOutURB->AddIsoPacket (usPacketSize);
    m_pOutURB->SetCompletionRoutine (OutStub, 0, this);
    return GetHost ()->SubmitAsyncRequest (m_pOutURB);
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

    delete m_pOutURB;
    m_pOutURB = 0;

    u16 usPacketSize = (u16) m_pEndpointOut->GetMaxPacketSize ();
    if (usPacketSize > sizeof m_OutBuf) usPacketSize = sizeof m_OutBuf;
    unsigned frameBytes = m_uSubslot * m_uChannels;   // UAC1: 4
    if (frameBytes == 0) frameBytes = 4;
    const unsigned cap = USB_AUDIO_BLOCK_BYTES / 4;
    unsigned nSamples = usPacketSize / frameBytes;
    if (nSamples > cap) nSamples = cap;
    if (m_pOutHandler)
    {
        s16 left_buf[cap], right_buf[cap];
        (*m_pOutHandler) (left_buf, right_buf, nSamples);
        u8 *pb = (u8 *) m_OutBuf;
        for (unsigned i = 0; i < nSamples; i++)
        {
            u8 *f = pb + i * frameBytes;
            uac2FromS16 (f, m_uSubslot, left_buf[i]);
            if (m_uChannels > 1) uac2FromS16 (f + m_uSubslot, m_uSubslot, right_buf[i]);
        }
    }
    else
    {
        memset (m_OutBuf, 0, usPacketSize);
    }
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
