#include "kernel.h"
#include <circle/util.h>
#include <circle/devicenameservice.h>

#define SERIAL_BAUD_RATE	115200
#define DRIVE			"SD:"
#define CDC_DEVICE_NAME		"utty1"

static const char log_name[] = "kernel";

static CActLED *s_pActLED = nullptr;
extern "C" void debug_blink(int n) { if (s_pActLED) s_pActLED->Blink(n); }

extern void setup(void);
extern void loop(void);
extern void usbMidiProcess(bool bPlugAndPlayUpdated);

CKernel::CKernel(void) :
	m_Timer(&m_Interrupt),
	m_Logger(LogDebug, &m_Timer),
	m_Serial(&m_Interrupt, FALSE),
	m_USBHCI(&m_Interrupt, &m_Timer, TRUE),
	m_CDCGadget(&m_Interrupt, 0x2E8A, 0x000A),  // RPi vendor ID, CDC serial device ID
	m_EMMC(&m_Interrupt, &m_Timer, &m_ActLED)
{
	m_ActLED.On();
	s_pActLED = &m_ActLED;
}

CKernel::~CKernel(void)
{
}

boolean CKernel::Initialize(void)
{
	boolean bOK = TRUE;

	if (bOK) bOK = m_Interrupt.Initialize();
	m_ActLED.Blink(1);  // 1: interrupt ok

	if (bOK) bOK = m_Timer.Initialize();
	m_ActLED.Blink(1);  // 2: timer ok

	if (m_Serial.Initialize(SERIAL_BAUD_RATE))
		m_Logger.Initialize(&m_Serial);
	m_ActLED.Blink(1);  // 3: serial ok

	if (bOK) { boolean bCDC = m_CDCGadget.Initialize(); m_Logger.Write(log_name, LogNotice, "CDC gadget init: %s", bCDC ? "OK" : "FAILED"); }
	m_ActLED.Blink(1);  // 4: cdc ok

	if (bOK) bOK = m_USBHCI.Initialize();
	m_ActLED.Blink(1);  // 5: usbhci ok

	CDevice *pCDCSerial = CDeviceNameService::Get()->GetDevice(CDC_DEVICE_NAME, FALSE);
	if (pCDCSerial != nullptr)
		m_Logger.SetNewTarget(pCDCSerial);
	m_ActLED.Blink(1);  // 6: cdc device lookup ok

	m_EMMC.Initialize();  // non-fatal: no SD card in netboot
	m_ActLED.Blink(1);  // 7: emmc ok

	f_mount(&m_FileSystem, DRIVE, 1);  // non-fatal
	m_ActLED.Blink(1);  // 8: fmount ok

	return bOK;
}

TShutdownMode CKernel::Run(void)
{
	CLogger::Get()->Write(log_name, LogNotice, "Looper starting");
	m_ActLED.Blink(1);  // 9: Run() entered

	setup();
	m_ActLED.Blink(1);  // 10: setup() done

	bool bPlugAndPlayUpdated = FALSE;
	while (TRUE)
	{
		bPlugAndPlayUpdated = m_USBHCI.UpdatePlugAndPlay();
		usbMidiProcess(bPlugAndPlayUpdated);
		loop();
		m_Scheduler.Yield();

		CDevice *pCDCSerial = CDeviceNameService::Get()->GetDevice(CDC_DEVICE_NAME, FALSE);
		if (pCDCSerial != nullptr)
		{
			u8 buf;
			if (pCDCSerial->Read(&buf, 1) == 1 && buf == 'R')
				return ShutdownReboot;
		}
	}

	return ShutdownHalt;
}
