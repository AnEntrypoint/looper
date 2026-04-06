#include "kernel.h"
#include <circle/util.h>

#define SERIAL_BAUD_RATE	115200
#define DRIVE			"SD:"

static const char log_name[] = "kernel";

extern void setup(void);
extern void loop(void);
extern void usbMidiProcess(bool bPlugAndPlayUpdated);

CKernel::CKernel(void) :
	m_Serial(&m_Interrupt, FALSE),
	m_Logger(LogDebug, &m_Timer),
	m_USBHCI(&m_Interrupt, &m_Timer, TRUE),
	m_EMMC(&m_Interrupt, &m_Timer, &m_ActLED)
{
	m_ActLED.Toggle();
}

CKernel::~CKernel(void)
{
}

boolean CKernel::Initialize(void)
{
	boolean bOK = TRUE;

	if (bOK)
		bOK = m_Serial.Initialize(SERIAL_BAUD_RATE);
	if (bOK)
		bOK = m_Logger.Initialize(&m_Serial);
	if (bOK)
		bOK = m_Interrupt.Initialize();
	if (bOK)
		bOK = m_Timer.Initialize();
	if (bOK)
		bOK = m_USBHCI.Initialize();
	if (bOK)
		bOK = m_EMMC.Initialize();
	if (bOK)
	{
		if (f_mount(&m_FileSystem, DRIVE, 1) != FR_OK)
		{
			CLogger::Get()->Write(log_name, LogError, "Cannot mount drive: %s", DRIVE);
			bOK = FALSE;
		}
	}

	return bOK;
}

TShutdownMode CKernel::Run(void)
{
	CLogger::Get()->Write(log_name, LogNotice, "Looper starting");

	setup();

	bool bPlugAndPlayUpdated = FALSE;
	while (TRUE)
	{
		bPlugAndPlayUpdated = m_USBHCI.UpdatePlugAndPlay();
		usbMidiProcess(bPlugAndPlayUpdated);
		loop();
		m_Scheduler.Yield();
	}

	return ShutdownHalt;
}
