#include "usbaudiodevice.h"
#include <circle/usb/usb.h>
#include <circle/usb/usbhostcontroller.h>
#include <circle/devicenameservice.h>
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <circle/string.h>
#include <assert.h>

static const char FromAudio[] = "uaudio";

CUSBAudioDevice *CUSBAudioDevice::s_pThis = 0;
unsigned         CUSBAudioDevice::s_nDeviceNumber = 1;

CUSBAudioDevice::CUSBAudioDevice (CUSBFunction *pFunction)
:   CUSBFunction (pFunction),
    m_pEndpointIn  (0),
    m_pEndpointOut (0),
    m_pInHandler   (0),
    m_pOutHandler  (0),
    m_pInURB       (0),
    m_pOutURB      (0),
    m_nPeakIn      (0),
    m_nLastMonitorTick (0)
{
}

CUSBAudioDevice::~CUSBAudioDevice (void)
{
    delete m_pEndpointIn;
    delete m_pEndpointOut;
    if (s_pThis == this) s_pThis = 0;
}

boolean CUSBAudioDevice::Configure (void)
{
    // PCM2902/UCA222: try alt-settings 1..4 to find isochronous endpoints.
    // Alt-setting 0 is zero-bandwidth (no endpoints) per USB Audio Class 1.0 spec.
    boolean bSelected = FALSE;
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
            "Could not select any audio streaming alt-setting; scanning anyway");
    }

    const TUSBEndpointDescriptor *pDesc;
    while ((pDesc = (const TUSBEndpointDescriptor *) GetDescriptor (DESCRIPTOR_ENDPOINT)) != 0)
    {
        boolean bIsIn   = (pDesc->bEndpointAddress & 0x80) == 0x80;
        boolean bIsIso  = (pDesc->bmAttributes & 0x03) == 0x01;
        CLogger::Get ()->Write (FromAudio, LogDebug,
            "EP addr=0x%02X attr=0x%02X isIn=%d isIso=%d",
            pDesc->bEndpointAddress, pDesc->bmAttributes, bIsIn, bIsIso);
        if (!bIsIso) continue;

        if (bIsIn && !m_pEndpointIn)
            m_pEndpointIn  = new CUSBEndpoint (GetDevice (), pDesc);
        else if (!bIsIn && !m_pEndpointOut)
            m_pEndpointOut = new CUSBEndpoint (GetDevice (), pDesc);
    }

    if (!m_pEndpointIn && !m_pEndpointOut)
    {
        CLogger::Get ()->Write (FromAudio, LogError,
            "No isochronous endpoints found on UCA222 — giving up");
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
    s_pThis = this;

    CLogger::Get ()->Write (FromAudio, LogNotice, "USB audio device configured (in=%s out=%s)",
        m_pEndpointIn ? "yes" : "no", m_pEndpointOut ? "yes" : "no");

    if (m_pEndpointIn)
        StartInRequest ();
    if (m_pEndpointOut)
        StartOutRequest ();

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

    m_pInURB = new CUSBRequest (m_pEndpointIn, m_InBuf, sizeof m_InBuf);
    assert (m_pInURB != 0);
    m_pInURB->SetCompletionRoutine (InStub, 0, this);
    return GetHost ()->SubmitAsyncRequest (m_pInURB);
}

boolean CUSBAudioDevice::StartOutRequest (void)
{
    assert (m_pEndpointOut != 0);
    assert (m_pOutURB == 0);

    m_pOutURB = new CUSBRequest (m_pEndpointOut, m_OutBuf, sizeof m_OutBuf);
    assert (m_pOutURB != 0);
    m_pOutURB->SetCompletionRoutine (OutStub, 0, this);
    return GetHost ()->SubmitAsyncRequest (m_pOutURB);
}

void CUSBAudioDevice::InCompletion (CUSBRequest *pURB)
{
    assert (pURB != 0);
    assert (pURB == m_pInURB);

    if (pURB->GetStatus () && pURB->GetResultLength () >= 4 && m_pInHandler != 0)
    {
        unsigned nSamples = pURB->GetResultLength () / 4;
        if (nSamples > USB_AUDIO_BLOCK_BYTES / 4) nSamples = USB_AUDIO_BLOCK_BYTES / 4;
        const s16 *pBuf = (const s16 *) m_InBuf;
        s16 left_buf[USB_AUDIO_BLOCK_BYTES / 4], right_buf[USB_AUDIO_BLOCK_BYTES / 4];
        for (unsigned i = 0; i < nSamples; i++)
        {
            left_buf[i]  = pBuf[i*2];
            right_buf[i] = pBuf[i*2+1];
            u32 absL = left_buf[i]  < 0 ? (u32)(-left_buf[i])  : (u32)left_buf[i];
            u32 absR = right_buf[i] < 0 ? (u32)(-right_buf[i]) : (u32)right_buf[i];
            if (absL > m_nPeakIn) m_nPeakIn = absL;
            if (absR > m_nPeakIn) m_nPeakIn = absR;
        }
        (*m_pInHandler) (left_buf, right_buf, nSamples);
    }

    u32 nNow = CTimer::Get ()->GetClockTicks ();
    if (nNow - m_nLastMonitorTick >= 1000000)
    {
        CLogger::Get ()->Write (FromAudio, LogNotice, "input peak=%u", m_nPeakIn);
        m_nPeakIn = 0;
        m_nLastMonitorTick = nNow;
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

    unsigned nSamples = sizeof m_OutBuf / 4;
    if (m_pOutHandler)
    {
        s16 left_buf[USB_AUDIO_BLOCK_BYTES / 4], right_buf[USB_AUDIO_BLOCK_BYTES / 4];
        (*m_pOutHandler) (left_buf, right_buf, nSamples);
        s16 *pBuf = (s16 *) m_OutBuf;
        for (unsigned i = 0; i < nSamples; i++)
        {
            pBuf[i*2]   = left_buf[i];
            pBuf[i*2+1] = right_buf[i];
        }
    }
    else
    {
        memset (m_OutBuf, 0, nSamples * 4);
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
