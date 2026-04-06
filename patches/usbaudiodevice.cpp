#include "usbaudiodevice.h"
#include <circle/usb/usb.h>
#include <circle/usb/usbhostcontroller.h>
#include <circle/devicenameservice.h>
#include <circle/logger.h>
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
    m_pOutURB      (0)
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
    // PCM2902/UCA222: default alt-setting 0 has no endpoints (zero-bandwidth).
    // Select the first Audio Streaming alt-setting that has at least 1 endpoint.
    if (!SelectInterfaceByClass (1, 2, 0, 1))
    {
        CLogger::Get ()->Write (FromAudio, LogWarning,
            "No audio streaming alt-setting with endpoints; trying anyway");
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
        const s16 *pBuf = (const s16 *) m_InBuf;
        s16 left_buf[nSamples], right_buf[nSamples];
        for (unsigned i = 0; i < nSamples; i++)
        {
            left_buf[i]  = pBuf[i*2];
            right_buf[i] = pBuf[i*2+1];
        }
        (*m_pInHandler) (left_buf, right_buf, nSamples);
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

    if (m_pOutHandler)
    {
        unsigned nSamples = sizeof m_OutBuf / 4;
        s16 left_buf[nSamples], right_buf[nSamples];
        (*m_pOutHandler) (left_buf, right_buf, nSamples);
        s16 *pBuf = (s16 *) m_OutBuf;
        for (unsigned i = 0; i < nSamples; i++)
        {
            pBuf[i*2]   = left_buf[i];
            pBuf[i*2+1] = right_buf[i];
        }
        StartOutRequest ();
    }
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
