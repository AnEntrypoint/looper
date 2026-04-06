#include "kernel.h"
#include <circle/util.h>
#include <circle/devicenameservice.h>

#define SERIAL_BAUD_RATE	115200
#define DRIVE			"SD:"
#define CDC_DEVICE_NAME		"utty1"

static const char log_name[] = "kernel";

extern void setup(void);
extern void loop(void);
extern void usbMidiProcess(bool bPlugAndPlayUpdated);

CKernel::CKernel(void) :
	m_Timer(&m_Interrupt),
	m_Logger(LogDebug, &m_Timer),
	m_Serial(&m_Interrupt, FALSE),
	m_USBHCI(&m_Interrupt, &m_Timer, TRUE),
	m_CDCGadget(&m_Interrupt),
	m_EMMC(&m_Interrupt, &m_Timer, &m_ActLED)
{
	m_ActLED.On();
}

CKernel::~CKernel(void)
{
}

boolean CKernel::Initialize(void)
{
	boolean bOK = TRUE;

	if (bOK)
		bOK = m_Interrupt.Initialize();
	m_ActLED.Blink(1);

	if (bOK)
		bOK = m_Timer.Initialize();
	m_ActLED.Blink(2);

	if (m_Serial.Initialize(SERIAL_BAUD_RATE))
		m_Logger.Initialize(&m_Serial);
	m_ActLED.Blink(3);

	if (bOK)
		m_CDCGadget.Initialize();
	m_ActLED.Blink(4);

	if (bOK)
		bOK = m_USBHCI.Initialize();
	m_ActLED.Blink(5);

	CDevice *pCDCSerial = CDeviceNameService::Get()->GetDevice(CDC_DEVICE_NAME, FALSE);
	if (pCDCSerial != nullptr)
		m_Logger.SetNewTarget(pCDCSerial);

	m_ActLED.Blink(7);
	m_EMMC.Initialize();  // non-fatal: no SD card in netboot
	m_ActLED.Blink(8);

	f_mount(&m_FileSystem, DRIVE, 1);  // non-fatal
	m_ActLED.Blink(9);

	m_ActLED.Blink(6);

	return bOK;
}

TShutdownMode CKernel::Run(void)
{
	CLogger::Get()->Write(log_name, LogNotice, "Looper starting");

	m_ActLED.Blink(3);

	setup();

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
