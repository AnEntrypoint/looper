#include <circle/usb/usbmidi.h>
#include <circle/usb/usbaudio.h>
#include <circle/usb/usb.h>
#include <circle/usb/usbhostcontroller.h>
#include <circle/devicenameservice.h>
#include <circle/logger.h>
#include <assert.h>

#define EVENT_PACKET_SIZE	4

static const char FromMIDI[] = "umidi";

unsigned CUSBMIDIDevice::s_nDeviceNumber = 1;

static const unsigned cin_to_length[] = {
	0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1
};

CUSBMIDIDevice::CUSBMIDIDevice (CUSBFunction *pFunction)
:	CUSBFunction (pFunction),
	m_pEndpointIn (0),
	m_pEndpointOut (0),
	m_pPacketHandler (0),
	m_pURB (0),
	m_pPacketBuffer (0),
	m_hTimer (0)
{
}

CUSBMIDIDevice::~CUSBMIDIDevice (void)
{
	delete [] m_pPacketBuffer;
	m_pPacketBuffer = 0;
	delete m_pEndpointIn;
	m_pEndpointIn = 0;
	delete m_pEndpointOut;
	m_pEndpointOut = 0;
}

boolean CUSBMIDIDevice::Configure (void)
{
	if (GetNumEndpoints () < 1)
	{
		ConfigurationError (FromMIDI);
		return FALSE;
	}

	TUSBAudioEndpointDescriptor *pEndpointDesc;
	while ((pEndpointDesc = (TUSBAudioEndpointDescriptor *) GetDescriptor (DESCRIPTOR_ENDPOINT)) != 0)
	{
		boolean bIsIn  = (pEndpointDesc->bEndpointAddress & 0x80) == 0x80;
		boolean bIsBulk = (pEndpointDesc->bmAttributes & 0x3F) == 0x02;

		if (!bIsBulk) continue;

		if (!bIsIn && m_pEndpointOut == 0)
		{
			m_pEndpointOut = new CUSBEndpoint (GetDevice (), (TUSBEndpointDescriptor *) pEndpointDesc);
			continue;
		}

		if (bIsIn && m_pEndpointIn == 0)
		{
			m_pEndpointIn = new CUSBEndpoint (GetDevice (), (TUSBEndpointDescriptor *) pEndpointDesc);
			m_usBufferSize = pEndpointDesc->wMaxPacketSize;
			m_usBufferSize -= m_usBufferSize % EVENT_PACKET_SIZE;
			m_pPacketBuffer = new u8[m_usBufferSize];
		}
	}

	if (m_pEndpointIn == 0)
	{
		ConfigurationError (FromMIDI);
		return FALSE;
	}

	if (!CUSBFunction::Configure ())
	{
		CLogger::Get ()->Write (FromMIDI, LogError, "Cannot set interface");
		return FALSE;
	}

	CString DeviceName;
	DeviceName.Format ("umidi%u", s_nDeviceNumber++);
	CDeviceNameService::Get ()->AddDevice (DeviceName, this, FALSE);

	return StartRequest ();
}

void CUSBMIDIDevice::RegisterPacketHandler (TMIDIPacketHandler *pPacketHandler)
{
	assert (m_pPacketHandler == 0);
	m_pPacketHandler = pPacketHandler;
}

boolean CUSBMIDIDevice::SendPlainMIDI (unsigned nCable, u8 *pData, unsigned nLength)
{
	if (m_pEndpointOut == 0 || pData == 0 || nLength == 0)
		return FALSE;

	unsigned nPackets = nLength / 3 + (nLength % 3 ? 1 : 0);
	unsigned nBufLen  = nPackets * EVENT_PACKET_SIZE;
	u8 *pBuf = new u8[nBufLen];
	if (!pBuf) return FALSE;

	unsigned i = 0;
	for (unsigned p = 0; p < nPackets; p++)
	{
		unsigned remaining = nLength - p * 3;
		u8 b0 = pData[p * 3];
		u8 cin = (b0 >> 4) & 0x0F;
		if (cin < 2) cin = (remaining >= 3) ? 0x0F : (remaining == 2 ? 0x06 : 0x05);
		pBuf[i++] = (u8)((nCable << 4) | cin);
		pBuf[i++] = b0;
		pBuf[i++] = (p * 3 + 1 < nLength) ? pData[p * 3 + 1] : 0;
		pBuf[i++] = (p * 3 + 2 < nLength) ? pData[p * 3 + 2] : 0;
	}

	CUSBRequest *pURB = new CUSBRequest (m_pEndpointOut, pBuf, nBufLen);
	boolean bOK = FALSE;
	if (pURB)
	{
		bOK = GetHost ()->SubmitBlockingRequest (pURB);
		delete pURB;
	}
	delete [] pBuf;
	return bOK;
}

boolean CUSBMIDIDevice::StartRequest (void)
{
	assert (m_pEndpointIn != 0);
	assert (m_pPacketBuffer != 0);
	assert (m_pURB == 0);
	assert (m_usBufferSize > 0);

	m_pURB = new CUSBRequest (m_pEndpointIn, m_pPacketBuffer, m_usBufferSize);
	assert (m_pURB != 0);
	m_pURB->SetCompletionRoutine (CompletionStub, 0, this);
	m_pURB->SetCompleteOnNAK ();

	return GetHost ()->SubmitAsyncRequest (m_pURB);
}

void CUSBMIDIDevice::CompletionRoutine (CUSBRequest *pURB)
{
	assert (pURB != 0);
	assert (m_pURB == pURB);

	boolean bRestart = FALSE;

	if (pURB->GetStatus () != 0 && pURB->GetResultLength () % EVENT_PACKET_SIZE == 0)
	{
		u8 *pEnd = m_pPacketBuffer + pURB->GetResultLength ();
		for (u8 *pPacket = m_pPacketBuffer; pPacket < pEnd; pPacket += EVENT_PACKET_SIZE)
		{
			if (pPacket[0] != 0)
			{
				if (m_pPacketHandler != 0)
				{
					unsigned nCable  = pPacket[0] >> 4;
					unsigned nLen    = cin_to_length[pPacket[0] & 0x0F];
					(*m_pPacketHandler) (nCable, pPacket + 1, nLen);
				}
				bRestart = TRUE;
			}
		}
	}

	delete m_pURB;
	m_pURB = 0;

	if (bRestart)
		StartRequest ();
	else
	{
		assert (m_hTimer == 0);
		m_hTimer = CTimer::Get ()->StartKernelTimer (MSEC2HZ (10), TimerStub, 0, this);
		assert (m_hTimer != 0);
	}
}

void CUSBMIDIDevice::CompletionStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	((CUSBMIDIDevice *) pContext)->CompletionRoutine (pURB);
}

void CUSBMIDIDevice::TimerHandler (TKernelTimerHandle hTimer)
{
	assert (m_hTimer == hTimer);
	m_hTimer = 0;
	StartRequest ();
}

void CUSBMIDIDevice::TimerStub (TKernelTimerHandle hTimer, void *pParam, void *pContext)
{
	((CUSBMIDIDevice *) pContext)->TimerHandler (hTimer);
}
