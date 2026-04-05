#ifndef _circle_usb_usbmidi_h
#define _circle_usb_usbmidi_h

#include <circle/usb/usbfunction.h>
#include <circle/usb/usbendpoint.h>
#include <circle/usb/usbrequest.h>
#include <circle/timer.h>
#include <circle/types.h>

typedef void TMIDIPacketHandler (unsigned nCable, u8 *pPacket, unsigned nLength);

class CUSBMIDIDevice : public CUSBFunction
{
public:
	CUSBMIDIDevice (CUSBFunction *pFunction);
	~CUSBMIDIDevice (void);

	boolean Configure (void);

	void RegisterPacketHandler (TMIDIPacketHandler *pPacketHandler);
	boolean SendPlainMIDI (unsigned nCable, u8 *pData, unsigned nLength);

private:
	boolean StartRequest (void);

	void CompletionRoutine (CUSBRequest *pURB);
	static void CompletionStub (CUSBRequest *pURB, void *pParam, void *pContext);

	void TimerHandler (TKernelTimerHandle hTimer);
	static void TimerStub (TKernelTimerHandle hTimer, void *pParam, void *pContext);

private:
	CUSBEndpoint *m_pEndpointIn;
	CUSBEndpoint *m_pEndpointOut;

	TMIDIPacketHandler *m_pPacketHandler;

	CUSBRequest *m_pURB;
	u16 m_usBufferSize;
	u8 *m_pPacketBuffer;

	TKernelTimerHandle m_hTimer;

	static unsigned s_nDeviceNumber;
};

#endif
