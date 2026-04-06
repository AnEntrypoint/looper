#include "kernel.h"
#include <circle/net/ipaddress.h>
#include <circle/util.h>
#include <circle/devicenameservice.h>

#define SERIAL_BAUD_RATE	115200
#define DRIVE			"SD:"
#define CDC_DEVICE_NAME		"utty1"
#define LOG_HOST		"192.168.137.1"

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
	m_Screen(1920, 1080),
	m_USBHCI(&m_Interrupt, &m_Timer, TRUE),
	m_CDCGadget(&m_Interrupt, 0x2E8A, 0x000A),
	m_EMMC(&m_Interrupt, &m_Timer, &m_ActLED),
	m_Net(),
	m_pSysLog(nullptr)
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

	if (m_Screen.Initialize())
		m_Logger.Initialize(&m_Screen);
	m_ActLED.Blink(1);  // 3: screen ok

	m_Serial.Initialize(SERIAL_BAUD_RATE);
	m_ActLED.Blink(1);  // 4: serial ok

	if (bOK) { boolean bCDC = m_CDCGadget.Initialize(); m_Logger.Write(log_name, LogNotice, "CDC gadget init: %s", bCDC ? "OK" : "FAILED"); }
	m_ActLED.Blink(1);  // 5: cdc ok

	if (bOK) bOK = m_USBHCI.Initialize();
	m_ActLED.Blink(1);  // 6: usbhci ok

	m_Timer.MsDelay(500);
	m_CDCGadget.UpdatePlugAndPlay();
	m_ActLED.Blink(1);  // 7: cdc ready

	m_EMMC.Initialize();
	m_ActLED.Blink(1);  // 8: emmc ok

	f_mount(&m_FileSystem, DRIVE, 1);
	m_ActLED.Blink(1);  // 9: fmount ok

	if (bOK) bOK = m_Net.Initialize();
	m_ActLED.Blink(1);  // 10: net ok

	return bOK;
}

TShutdownMode CKernel::Run(void)
{
	m_Logger.Write(log_name, LogNotice, "Looper starting");
	m_ActLED.Blink(1);  // 11: Run() entered

	// Wait for DHCP
	m_Logger.Write(log_name, LogNotice, "Waiting for network...");
	while (!m_Net.IsRunning())
	{
		m_Scheduler.Yield();
		m_Net.Process();
	}
	m_Logger.Write(log_name, LogNotice, "Network up");

	// Start syslog to PC
	CIPAddress LogHost;
	LogHost.Set(LOG_HOST);
	m_pSysLog = new CSysLogDaemon(&m_Net, LogHost);
	m_Logger.Write(log_name, LogNotice, "Syslog -> %s:514", LOG_HOST);

	setup();
	m_ActLED.Blink(1);  // 12: setup() done

	bool bPlugAndPlayUpdated = FALSE;
	while (TRUE)
	{
		bPlugAndPlayUpdated = m_USBHCI.UpdatePlugAndPlay();
		m_CDCGadget.UpdatePlugAndPlay();
		m_Net.Process();
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
