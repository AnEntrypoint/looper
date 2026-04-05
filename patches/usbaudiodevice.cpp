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
    if (!m_pEndpointIn || m_pInURB) return FALSE;
    m_pInURB = new CUSBRequest (m_pEndpointIn, m_InBuf, USB_AUDIO_BLOCK_BYTES);
    if (!m_pInURB) return FALSE;
    m_pInURB->SetCompletionRoutine (InStub, 0, this);
    return GetHost ()->SubmitAsyncRequest (m_pInURB);
}

boolean CUSBAudioDevice::StartOutRequest (void)
{
    if (!m_pEndpointOut || m_pOutURB) return FALSE;
    if (m_pOutHandler)
    {
        s16 *pL = (s16 *) m_OutBuf;
        s16 *pR = pL + USB_AUDIO_BLOCK_BYTES / 4;
        m_pOutHandler (pL, pR, USB_AUDIO_BLOCK_BYTES / 4);
        s16 *pInterleaved = (s16 *) m_OutBuf;
        for (int i = USB_AUDIO_BLOCK_BYTES / 4 - 1; i >= 0; i--)
        {
            pInterleaved[i * 2 + 1] = pR[i];
            pInterleaved[i * 2]     = pL[i];
        }
    }
    else
        memset (m_OutBuf, 0, USB_AUDIO_BLOCK_BYTES);

    m_pOutURB = new CUSBRequest (m_pEndpointOut, m_OutBuf, USB_AUDIO_BLOCK_BYTES);
    if (!m_pOutURB) return FALSE;
    m_pOutURB->SetCompletionRoutine (OutStub, 0, this);
    return GetHost ()->SubmitAsyncRequest (m_pOutURB);
}

void CUSBAudioDevice::InCompletion (CUSBRequest *pURB)
{
    assert (pURB == m_pInURB);
    unsigned nLen = pURB->GetResultLength ();
    delete m_pInURB;
    m_pInURB = 0;

    unsigned nSamples = nLen / 4;
    if (nSamples > 0 && m_pInHandler)
    {
        s16 tmp[USB_AUDIO_BLOCK_BYTES / 4];
        s16 *pSrc = (s16 *) m_InBuf;
        s16 *pL   = tmp;
        s16 *pR   = tmp + nSamples;
        for (unsigned i = 0; i < nSamples; i++)
        {
            pL[i] = pSrc[i * 2];
            pR[i] = pSrc[i * 2 + 1];
        }
        m_pInHandler (pL, pR, nSamples);
    }

    StartInRequest ();
    StartOutRequest ();
}

void CUSBAudioDevice::OutCompletion (CUSBRequest *pURB)
{
    assert (pURB == m_pOutURB);
    delete m_pOutURB;
    m_pOutURB = 0;
}

void CUSBAudioDevice::InStub  (CUSBRequest *pURB, void *pParam, void *pContext)
{
    ((CUSBAudioDevice *) pContext)->InCompletion (pURB);
}

void CUSBAudioDevice::OutStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
    ((CUSBAudioDevice *) pContext)->OutCompletion (pURB);
}
