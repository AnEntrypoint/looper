//
// usbmidihost.cpp (patched: skip CS_ENDPOINT adjacency check for APC Key 25 compat)
//
// Based on rsta2/circle usbmidihost.cpp
//
#include <circle/usb/usbmidihost.h>
#include <circle/usb/usbaudio.h>
#include <circle/usb/usb.h>
#include <circle/usb/usbhostcontroller.h>
#include <circle/synchronize.h>
#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/debug.h>
#include <circle/util.h>
#include <assert.h>

static const char FromMIDI[] = "umidihost";

CUSBMIDIHostDevice::CUSBMIDIHostDevice (CUSBFunction *pFunction)
:	CUSBFunction (pFunction),
	m_pInterface (0),
	m_pEndpointIn (0),
	m_pEndpointOut (0),
	m_pPacketBuffer (0),
	m_hTimer (0)
{
	const TUSBDeviceDescriptor *pDeviceDesc = GetDevice ()->GetDeviceDescriptor ();
	assert (pDeviceDesc != 0);

	if (   pDeviceDesc->idVendor  == 0x0582
	    && pDeviceDesc->idProduct == 0x028C)
	{
		if (!SelectInterfaceByClass (255, 3, 0))
		{
			CLogger::Get ()->Write (FromMIDI, LogError, "Cannot select interface");
		}
	}
}

CUSBMIDIHostDevice::~CUSBMIDIHostDevice (void)
{
	if (m_hTimer != 0)
	{
		CTimer::Get ()->CancelKernelTimer (m_hTimer);
		m_hTimer = 0;
	}

	delete m_pInterface;
	m_pInterface = 0;

	delete [] m_pPacketBuffer;
	m_pPacketBuffer = 0;

	delete m_pEndpointIn;
	m_pEndpointIn = 0;

	delete m_pEndpointOut;
	m_pEndpointOut = 0;
}

boolean CUSBMIDIHostDevice::Configure (void)
{
	if (GetNumEndpoints () < 1)
	{
		ConfigurationError (FromMIDI);
		return FALSE;
	}

	TUSBAudioEndpointDescriptor *pEndpointDesc;
	while ((pEndpointDesc = (TUSBAudioEndpointDescriptor *) GetDescriptor (DESCRIPTOR_ENDPOINT)) != 0)
	{
		if ((pEndpointDesc->bmAttributes & 0x3E) != 0x02)
		{
			continue;
		}

		if ((pEndpointDesc->bEndpointAddress & 0x80) == 0x80)
		{
			if (m_pEndpointIn != 0)
			{
				ConfigurationError (FromMIDI);
				return FALSE;
			}

			m_pEndpointIn = new CUSBEndpoint (GetDevice (), (TUSBEndpointDescriptor *) pEndpointDesc);
			assert (m_pEndpointIn != 0);

			m_usBufferSize  = pEndpointDesc->wMaxPacketSize;
			m_usBufferSize -=   pEndpointDesc->wMaxPacketSize
					  % CUSBMIDIDevice::EventPacketSize;

			assert (m_pPacketBuffer == 0);
			m_pPacketBuffer = new u8[m_usBufferSize];
			assert (m_pPacketBuffer != 0);
		}
		else
		{
			if (m_pEndpointOut != 0)
			{
				ConfigurationError (FromMIDI);
				return FALSE;
			}

			m_pEndpointOut = new CUSBEndpoint (GetDevice (), (TUSBEndpointDescriptor *) pEndpointDesc);
			assert (m_pEndpointOut != 0);
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

	assert (m_pInterface == 0);
	m_pInterface = new CUSBMIDIDevice;
	assert (m_pInterface != 0);
	m_pInterface->RegisterSendEventsHandler (SendEventsHandler, this);

	return StartRequest ();
}

#define USBMIDI_OUT_SLOTS      8
#define USBMIDI_OUT_BUFSIZE    64

struct TMIDIOutSlot
{
	CUSBMIDIHostDevice *pOwner;
	volatile boolean    bBusy;
	unsigned            nErrors;
	u8                  Buffer[USBMIDI_OUT_BUFSIZE] __attribute__((aligned(64)));
};

static TMIDIOutSlot s_MIDIOutSlots[USBMIDI_OUT_SLOTS] = {};
volatile unsigned g_midiOutDropped = 0;
volatile unsigned g_midiOutErrors  = 0;

static TMIDIOutSlot *AllocSlot (CUSBMIDIHostDevice *pOwner)
{
	for (int i = 0; i < USBMIDI_OUT_SLOTS; i++)
	{
		if (s_MIDIOutSlots[i].pOwner == pOwner && !s_MIDIOutSlots[i].bBusy)
			return &s_MIDIOutSlots[i];
	}
	for (int i = 0; i < USBMIDI_OUT_SLOTS; i++)
	{
		if (s_MIDIOutSlots[i].pOwner == 0 && !s_MIDIOutSlots[i].bBusy)
		{
			s_MIDIOutSlots[i].pOwner = pOwner;
			return &s_MIDIOutSlots[i];
		}
	}
	return 0;
}

static void MIDIOutCompletion (CUSBRequest *pURB, void *pParam, void *pContext)
{
	TMIDIOutSlot *pSlot = (TMIDIOutSlot *) pContext;
	assert (pSlot);
	if (pURB->GetStatus () == 0)
	{
		pSlot->nErrors++;
		g_midiOutErrors++;
	}
	delete pURB;
	pSlot->bBusy = FALSE;
}

boolean CUSBMIDIHostDevice::SendEventsHandler (const u8 *pData, unsigned nLength, void *pParam)
{
	CUSBMIDIHostDevice *pThis = static_cast<CUSBMIDIHostDevice *> (pParam);
	assert (pThis);
	assert (pData != 0);
	assert (nLength > 0);
	assert ((nLength & 3) == 0);

	if (pThis->m_pEndpointOut == 0)
		return FALSE;

	if (nLength > USBMIDI_OUT_BUFSIZE)
	{
		g_midiOutDropped++;
		return FALSE;
	}

	TMIDIOutSlot *pSlot = AllocSlot (pThis);
	if (!pSlot)
	{
		g_midiOutDropped++;
		return TRUE;
	}

	pSlot->bBusy = TRUE;
	memcpy (pSlot->Buffer, pData, nLength);

	CUSBRequest *pURB = new CUSBRequest (pThis->m_pEndpointOut, pSlot->Buffer, nLength);
	if (!pURB)
	{
		pSlot->bBusy = FALSE;
		g_midiOutDropped++;
		return FALSE;
	}
	pURB->SetCompletionRoutine (MIDIOutCompletion, 0, pSlot);
	if (!pThis->GetHost ()->SubmitAsyncRequest (pURB))
	{
		delete pURB;
		pSlot->bBusy = FALSE;
		g_midiOutDropped++;
		return FALSE;
	}
	return TRUE;
}

boolean CUSBMIDIHostDevice::StartRequest (void)
{
	assert (m_pEndpointIn != 0);
	assert (m_pPacketBuffer != 0);
	assert (m_usBufferSize > 0);

	CUSBRequest *pURB = new CUSBRequest (m_pEndpointIn, m_pPacketBuffer, m_usBufferSize);
	assert (pURB != 0);
	pURB->SetCompletionRoutine (CompletionStub, 0, this);
	pURB->SetCompleteOnNAK ();

	return GetHost ()->SubmitAsyncRequest (pURB);
}

void CUSBMIDIHostDevice::CompletionRoutine (CUSBRequest *pURB)
{
	assert (pURB != 0);
	assert (m_pInterface != 0);

	boolean bRestart = FALSE;

	if (   pURB->GetStatus () != 0
	    && pURB->GetResultLength () % CUSBMIDIDevice::EventPacketSize == 0)
	{
		assert (m_pPacketBuffer != 0);
		bRestart = m_pInterface->CallPacketHandler (m_pPacketBuffer, pURB->GetResultLength ());
	}
	else if (   m_pInterface->GetAllSoundOffOnUSBError ()
		 && !pURB->GetStatus ()
		 && pURB->GetUSBError () != USBErrorUnknown)
	{
		for (u8 nChannel = 0; nChannel < 16; nChannel++)
		{
			u8 AllSoundOff[] = {0x0B, (u8) (0xB0 | nChannel), 120, 0};
			m_pInterface->CallPacketHandler (AllSoundOff, sizeof AllSoundOff);
		}
	}

	delete pURB;

	if (   bRestart
	    || CKernelOptions::Get ()->GetUSBBoost ())
	{
		StartRequest ();
	}
	else
	{
		assert (m_hTimer == 0);
		m_hTimer = CTimer::Get ()->StartKernelTimer (MSEC2HZ (10), TimerStub, 0, this);
		assert (m_hTimer != 0);
	}
}

void CUSBMIDIHostDevice::CompletionStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	CUSBMIDIHostDevice *pThis = (CUSBMIDIHostDevice *) pContext;
	assert (pThis != 0);
	pThis->CompletionRoutine (pURB);
}

void CUSBMIDIHostDevice::TimerHandler (TKernelTimerHandle hTimer)
{
	assert (m_hTimer == hTimer);
	m_hTimer = 0;
	StartRequest ();
}

void CUSBMIDIHostDevice::TimerStub (TKernelTimerHandle hTimer, void *pParam, void *pContext)
{
	CUSBMIDIHostDevice *pThis = (CUSBMIDIHostDevice *) pContext;
	assert (pThis != 0);
	pThis->TimerHandler (hTimer);
}
