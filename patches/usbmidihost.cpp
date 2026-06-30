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

	// Release all OUT slots owned by this device so AllocSlot can reuse them.
	// Without this, each unplug leaves the slot's pOwner pointing to the now-
	// destroyed CUSBMIDIHostDevice (dangling). After 8 unplug/replug cycles all
	// 8 slots are permanently claimed by dead owners; AllocSlot returns nullptr
	// and every LED send drops silently until a full reboot. pOwner is safe to
	// clear here even with an in-flight URB: MIDIOutCompletion only touches the
	// TMIDIOutSlot* (static global storage) and never dereferences pOwner.
	for (int i = 0; i < USBMIDI_OUT_SLOTS; i++)
	{
		if (s_MIDIOutSlots[i].pOwner == this)
			s_MIDIOutSlots[i].pOwner = 0;
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
	u8                 *pBuffer;
};

static TMIDIOutSlot s_MIDIOutSlots[USBMIDI_OUT_SLOTS] = {};
volatile unsigned g_midiOutDropped = 0;
volatile unsigned g_midiOutErrors  = 0;
static volatile unsigned s_MIDIOutInFlight = 0;
// Keep ONE MIDI OUT URB in flight. Raising this to pipeline LED bursts broke
// MIDI entirely on the APC25 — queuing multiple concurrent OUT URBs on its
// single full-speed OUT endpoint overran the device and wedged the transfer
// path (no LEDs AND no input). Cap 1 is the proven-good serialization. The
// stuck-LED problem is fixed instead by the drop-returns-FALSE change below:
// the coalescer leaves the cache stale on a drop and re-sends the SAME update
// the next 33ms tick, so updates self-heal without ever overrunning the
// endpoint. Steady state changes few pads per tick, so cap-1 keeps up; only a
// full-grid color change settles over a few ticks (was the prior behavior).
#define USBMIDI_OUT_MAX_INFLIGHT 1

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
			if (s_MIDIOutSlots[i].pBuffer == 0)
			{
				s_MIDIOutSlots[i].pBuffer = new u8[USBMIDI_OUT_BUFSIZE];
				if (s_MIDIOutSlots[i].pBuffer == 0)
					return 0;
			}
			s_MIDIOutSlots[i].pOwner = pOwner;
			return &s_MIDIOutSlots[i];
		}
	}
	return 0;
}

// Count OUT URBs currently in flight FOR THIS owner. The in-flight cap must be
// per-device, not global: s_MIDIOutInFlight (a single counter) let ONE device's
// stuck OUT endpoint pin the global count at the cap forever, dropping every
// send to EVERY device. Symptom: plug a US-2x2 (its USB-MIDI OUT endpoint never
// drains) alongside the APC -> the APC's LED sends all drop (g_midiOutDropped
// runs to 6 figures, APC dark) even though the APC enumerated fine. Capping per
// owner means a wedged device only starves itself.
static unsigned countOwnerInFlight (CUSBMIDIHostDevice *pOwner)
{
	unsigned n = 0;
	for (int i = 0; i < USBMIDI_OUT_SLOTS; i++)
		if (s_MIDIOutSlots[i].pOwner == pOwner && s_MIDIOutSlots[i].bBusy)
			n++;
	return n;
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
	if (s_MIDIOutInFlight > 0) s_MIDIOutInFlight--;
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

	if (countOwnerInFlight (pThis) >= USBMIDI_OUT_MAX_INFLIGHT)
	{
		// Dropped (transmit pipeline full). Return FALSE, not TRUE: the LED
		// coalescer (sendLedCoalesced) only commits a pad's color to its cache
		// when the send returns TRUE, and retries next tick otherwise. Returning
		// TRUE on a drop made the coalescer cache a color that never went out,
		// so the pad froze at its previous color until the value changed again
		// ("most LED updates reach, some don't"). FALSE leaves the cache stale
		// so the same update is re-sent ~33ms later when a slot frees.
		g_midiOutDropped++;
		return FALSE;
	}

	TMIDIOutSlot *pSlot = AllocSlot (pThis);
	if (!pSlot)
	{
		g_midiOutDropped++;
		return FALSE;
	}

	pSlot->bBusy = TRUE;
	memcpy (pSlot->pBuffer, pData, nLength);

	CUSBRequest *pURB = new CUSBRequest (pThis->m_pEndpointOut, pSlot->pBuffer, nLength);
	if (!pURB)
	{
		pSlot->bBusy = FALSE;
		g_midiOutDropped++;
		return FALSE;
	}
	pURB->SetCompletionRoutine (MIDIOutCompletion, 0, pSlot);
	s_MIDIOutInFlight++;
	if (!pThis->GetHost ()->SubmitAsyncRequest (pURB))
	{
		if (s_MIDIOutInFlight > 0) s_MIDIOutInFlight--;
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
