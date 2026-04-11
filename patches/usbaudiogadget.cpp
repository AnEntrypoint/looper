#include "usbaudiogadget.h"
#include "usbaudiogadgetendpoint.h"
#include <circle/logger.h>
#include <circle/string.h>
#include <circle/util.h>
#include <assert.h>

static const char FromAudioGadget[] = "uac gadget";

CUSBAudioGadget *CUSBAudioGadget::s_pThis = nullptr;

const CUSBAudioGadget::TUSBAudioGadgetDeviceDescriptor CUSBAudioGadget::s_DeviceDescriptor =
{
	{ sizeof (TUSBDeviceDescriptor), DESCRIPTOR_DEVICE,
	  0x0200, 0x00, 0x00, 0x00, 64, 0, 0, 0x0100, 1, 2, 0, 1 }
};

const CUSBAudioGadget::TUSBAudioGadgetConfigurationDescriptor CUSBAudioGadget::s_ConfigDescriptor =
{
	{ sizeof (TUSBConfigurationDescriptor), DESCRIPTOR_CONFIGURATION,
	  sizeof (TUSBAudioGadgetConfigurationDescriptor), 3, 1, 0,
	  0x80, 250 },
	{ sizeof (TUSBInterfaceDescriptor), DESCRIPTOR_INTERFACE,
	  0, 0, 0, 0x01, 0x01, 0x00, 0 },
	{ 10, 0x24, 0x01, 0x00, 0x01, 9+8+8, 0x00, 2, 1, 2 },
	{ sizeof (TUSBInterfaceDescriptor), DESCRIPTOR_INTERFACE,
	  1, 0, 0, 0x01, 0x02, 0x00, 0 },
	{ sizeof (TUSBInterfaceDescriptor), DESCRIPTOR_INTERFACE,
	  1, 1, 1, 0x01, 0x02, 0x00, 0 },
	{ 7, 0x24, 0x01, 0x00, 0x00, 7+11, 0x00 },
	{ 11, 0x24, 0x02, 0x01, 0x02, 2, 0x10, 1, 0x80, 0xBB, 0x00 },
	{ 9, 0x05, (u8)(EPOut | 0x00), 0x01, 0xC0, 0x00, 0x01, 0x00, 0x00 },
	{ 7, 0x25, 0x01, 0x01, 0x00, 0x00, 0x00 },
	{ sizeof (TUSBInterfaceDescriptor), DESCRIPTOR_INTERFACE,
	  2, 0, 0, 0x01, 0x02, 0x00, 0 },
	{ sizeof (TUSBInterfaceDescriptor), DESCRIPTOR_INTERFACE,
	  2, 1, 1, 0x01, 0x02, 0x00, 0 },
	{ 7, 0x24, 0x01, 0x00, 0x00, 7+11, 0x00 },
	{ 11, 0x24, 0x02, 0x01, 0x02, 2, 0x10, 1, 0x80, 0xBB, 0x00 },
	{ 9, 0x05, (u8)(EPIn | 0x80), 0x01, 0xC0, 0x00, 0x01, 0x00, 0x00 },
	{ 7, 0x25, 0x01, 0x01, 0x00, 0x00, 0x00 },
};

const char *const CUSBAudioGadget::s_StringDescriptor[] =
{
	"\x09\x04",
	"Circle",
	"Looper Audio",
	nullptr
};

CUSBAudioGadget::CUSBAudioGadget (CInterruptSystem *pInterruptSystem,
				  u16 usVendorID, u16 usProductID)
:	CDWUSBGadget (pInterruptSystem, FullSpeed),
	m_pEPOut (nullptr),
	m_pEPIn (nullptr)
{
	const_cast<TUSBAudioGadgetDeviceDescriptor &>(s_DeviceDescriptor)
		.Device.idVendor  = usVendorID;
	const_cast<TUSBAudioGadgetDeviceDescriptor &>(s_DeviceDescriptor)
		.Device.idProduct = usProductID;
	assert (!s_pThis);
	s_pThis = this;
}

CUSBAudioGadget::~CUSBAudioGadget (void)
{
	s_pThis = nullptr;
}

void CUSBAudioGadget::RegisterInHandler (TAudioInHandler *pHandler)
{
	if (m_pEPIn) m_pEPIn->RegisterInHandler (pHandler);
}

void CUSBAudioGadget::RegisterOutHandler (TAudioOutHandler *pHandler)
{
	if (m_pEPOut) m_pEPOut->RegisterOutHandler (pHandler);
}

const void *CUSBAudioGadget::GetDescriptor (u16 wValue, u16 wIndex, size_t *pLength)
{
	assert (pLength);
	switch (wValue >> 8)
	{
	case DESCRIPTOR_DEVICE:
		*pLength = sizeof s_DeviceDescriptor;
		return &s_DeviceDescriptor;
	case DESCRIPTOR_CONFIGURATION:
		*pLength = sizeof s_ConfigDescriptor;
		return &s_ConfigDescriptor;
	case DESCRIPTOR_STRING:
		{
			unsigned nIndex = wValue & 0xFF;
			if (!s_StringDescriptor[nIndex]) return nullptr;
			if (nIndex == 0)
			{
				*pLength = 4;
				return "\x04\x03\x09\x04";
			}
			return ToStringDescriptor (s_StringDescriptor[nIndex], pLength);
		}
	}
	return nullptr;
}

void CUSBAudioGadget::AddEndpoints (void)
{
	assert (!m_pEPOut && !m_pEPIn);
	TUSBEndpointDescriptor descOut;
	memset (&descOut, 0, sizeof descOut);
	descOut.bLength          = sizeof descOut;
	descOut.bDescriptorType  = DESCRIPTOR_ENDPOINT;
	descOut.bEndpointAddress = EPOut;
	descOut.bmAttributes     = 0x01;
	descOut.wMaxPacketSize   = 192;
	descOut.bInterval        = 1;
	m_pEPOut = new CUSBAudioGadgetEndpoint (&descOut, this);

	TUSBEndpointDescriptor descIn;
	memset (&descIn, 0, sizeof descIn);
	descIn.bLength          = sizeof descIn;
	descIn.bDescriptorType  = DESCRIPTOR_ENDPOINT;
	descIn.bEndpointAddress = EPIn | 0x80;
	descIn.bmAttributes     = 0x01;
	descIn.wMaxPacketSize   = 192;
	descIn.bInterval        = 1;
	m_pEPIn = new CUSBAudioGadgetEndpoint (&descIn, this);

	CLogger::Get ()->Write (FromAudioGadget, LogNotice, "iso ep configured 48kHz stereo");
}

void CUSBAudioGadget::CreateDevice (void)
{
	CLogger::Get ()->Write (FromAudioGadget, LogNotice, "alt=1 streaming started");
	if (m_pEPOut) m_pEPOut->OnActivate ();
	if (m_pEPIn)  m_pEPIn->OnActivate ();
}

void CUSBAudioGadget::OnSuspend (void)
{
	CLogger::Get ()->Write (FromAudioGadget, LogNotice, "alt=0 streaming stopped");
	if (m_pEPOut) { m_pEPOut->OnSuspend (); delete m_pEPOut; m_pEPOut = nullptr; }
	if (m_pEPIn)  { m_pEPIn->OnSuspend ();  delete m_pEPIn;  m_pEPIn  = nullptr; }
}

const void *CUSBAudioGadget::ToStringDescriptor (const char *pString, size_t *pLength)
{
	unsigned nLen = 0;
	while (pString[nLen]) nLen++;
	unsigned nDescLen = 2 + nLen * 2;
	assert (nDescLen <= sizeof m_StringDescBuf);
	m_StringDescBuf[0] = (u8) nDescLen;
	m_StringDescBuf[1] = DESCRIPTOR_STRING;
	for (unsigned i = 0; i < nLen; i++)
	{
		m_StringDescBuf[2 + i*2]     = (u8) pString[i];
		m_StringDescBuf[2 + i*2 + 1] = 0;
	}
	*pLength = nDescLen;
	return m_StringDescBuf;
}
