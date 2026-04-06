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
    // PCM2902/UCA222: default alt-setting 0 has no endpoints; select alt-setting with endpoints
    if (!SelectInterfaceByClass (1, 2, 0, 1))
    {
        CLogger::Get ()->Write (FromAudio, LogWarning, "No audio streaming alt-setting with endpoints found");
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
    m_pOu