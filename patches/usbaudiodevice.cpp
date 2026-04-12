#include "usbaudiodevice.h"
#include <circle/usb/usb.h>
#include <circle/usb/usbhostcontroller.h>
#include <circle/devicenameservice.h>
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <circle/string.h>
#include <circle/synchronize.h>
#include <assert.h>

static const char FromAudio[] = "uaudio";

CUSBAudioDevice *CUSBAudioDevice::s_pThis = 0;
CUSBAudioDevice *CUSBAudioDevice::s_pOut  = 0;
unsigned         CUSBAudioDevice::s_nDeviceNumber = 1;

CUSBAudioDevice::CUSBAudioDevice (CUSBFunction *pFunction)
:   CUSBFunction (pFunction),
    m_pEndpointIn  (0),
    m_pEndpointOut (0),
    m_pInHandler   (0),
    m_pOutHandler  (0),
    m_nPeakIn      (0),
    m_nLastMonitorTick (0)
{
    for (unsigned i = 0; i < USB_AUDIO_NUM_BUFS; i++)
    {
        m_pInURB[i]  = 0;
        m_pOutURB[i] = 0;
    }
}

CUSBAudioDevice::~CUSBAudioDevice (void)
{
    for (unsigned i = 0; i < USB_AUDIO_NUM_BUFS; i++)
    {
        delete m_pInURB[i];
        delete m_pOutURB[i];
    }
    delete m_pEndpointIn;
    delete m_pEndpointOut;
    if (s_pThis == this) s_pThis = 0;
    if (s_pOut == this) s_pOut = 0;
}

boolean CUSBAudioDevice::Configure (void)
{
    CLogger::Get ()->Write (FromAudio, LogNotice, "Configure() called");
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
        if (!bIsIso) continue;

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
        s_pThis = this;
    if (m_pEndpointOut && !s_pOut)
        s_pOut = this;

    CLogger::Get ()->Write (FromAudio, LogNotice, "USB audio configured (in=%s out=%s)",
        m_pEndpointIn ? "yes" : "no", m_pEndpointOut ? "yes" : "no");

    if (m_pEndpointIn)
        for (unsigned i = 0; i < USB_AUDIO_NUM_BUFS; i++)
            StartInRequest (i);
    if (m_pEndpointOut)
        for (unsigned i = 0; i < USB_AUDIO_NUM_BUFS; i++)
            StartOutRequest (i);

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

boolean CUSBAudioDevice::StartInRequest (unsigned nBuf)
{
    assert (m_pEndpointIn != 0);
    assert (nBuf < USB_AUDIO_NUM_BUFS);

    u16 usPacketSize = (u16) m_pEndpointIn->GetMaxPacketSize ();
    if (usPacketSize > USB_AUDIO_BLOCK_BYTES) usPacketSize = USB_AUDIO_BLOCK_BYTES;

    delete m_pInURB[nBuf];
    m_pInURB[nBuf] = new CUSBRequest (m_pEndpointIn, m_InBuf[nBuf], usPacketSize);
    assert (m_pInURB[nBuf] != 0);
    m_pInURB[nBuf]->AddIsoPacket (usPacketSize);
    m_pInURB[nBuf]->SetCompletionRoutine (InStub, (void *)(unsigned long)nBuf, this);
    return GetHost ()->SubmitAsyncRequest (m_pInURB[nBuf]);
}

boolean CUSBAudioDevice::StartOutRequest (unsigned nBuf)
{
    assert (m_pEndpointOut != 0);
    assert (nBuf < USB_AUDIO_NUM_BUFS);

    u16 usPacketSize = (u16) m_pEndpointOut->GetMaxPacketSize ();
    if (usPacketSize > USB_AUDIO_BLOCK_BYTES) usPacketSize = USB_AUDIO_BLOCK_BYTES;

    delete m_pOutURB[nBuf];
    m_pOutURB[nBuf] = new CUSBRequest (m_pEndpointOut, m_OutBuf[nBuf], usPacketSize);
    assert (m_pOutURB[nBuf] != 0);
    m_pOutURB[nBuf]->AddIsoPacket (usPacketSize);
    m_pOutURB[nBuf]->SetCompletionRoutine (OutStub, (void *)(unsigned long)nBuf, this);
    return GetHost ()->SubmitAsyncRequest (m_pOutURB[nBuf]);
}

void CUSBAudioDevice::InCompletion (CUSBRequest *pURB, unsigned nBuf)
{
    assert (pURB != 0);
    assert (nBuf < USB_AUDIO_NUM_BUFS);

    if (pURB->GetStatus () && pURB->GetResultLength () >= 4 && m_pInHandler != 0)
    {
        unsigned nSamples = pURB->GetResultLength () / 4;
        if (nSamples > USB_AUDIO_BLOCK_BYTES / 4) nSamples = USB_AUDIO_BLOCK_BYTES / 4;
        const s16 *pBuf = (const s16 *) m_InBuf[nBuf];
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

    StartInRequest (nBuf);
}

void CUSBAudioDevice::OutCompletion (CUSBRequest *pURB, unsigned nBuf)
{
    assert (pURB != 0);
    assert (nBuf < USB_AUDIO_NUM_BUFS);

    u16 usPacketSize = (u16) m_pEndpointOut->GetMaxPacketSize ();
    if (usPacketSize > USB_AUDIO_BLOCK_BYTES) usPacketSize = USB_AUDIO_BLOCK_BYTES;
    unsigned nSamples = usPacketSize / 4;
    if (m_pOutHandler)
    {
        s16 left_buf[USB_AUDIO_BLOCK_BYTES / 4], right_buf[USB_AUDIO_BLOCK_BYTES / 4];
        (*m_pOutHandler) (left_buf, right_buf, nSamples);
        s16 *pBuf = (s16 *) m_OutBuf[nBuf];
        for (unsigned i = 0; i < nSamples; i++)
        {
            pBuf[i*2]   = left_buf[i];
            pBuf[i*2+1] = right_buf[i];
        }
    }
    else
    {
        memset (m_OutBuf[nBuf], 0, usPacketSize);
    }
    StartOutRequest (nBuf);
}

void CUSBAudioDevice::InStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
    CUSBAudioDevice *pThis = (CUSBAudioDevice *) pContext;
    assert (pThis != 0);
    pThis->InCompletion (pURB, (unsigned)(unsigned long) pParam);
}

void CUSBAudioDevice::OutStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
    CUSBAudioDevice *pThis = (CUSBAudioDevice *) pContext;
    assert (pThis != 0);
    pThis->OutCompletion (pURB, (unsigned)(unsigned long) pParam);
}
