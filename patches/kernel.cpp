#include "kernel.h"
#include <circle/util.h>
#include <circle/devicenameservice.h>

#define SERIAL_BAUD_RATE	115200
#define DRIVE			"SD:"
#define CDC_DEVICE_NAME		"utty1"

static const char log_name[] = "kernel";
static const char build_id[] = "BUILD-" __DATE__ "-" __TIME__;

static CActLED *s_pActLED = nullptr;
extern "C" void debug_blink(int n) { if (s_pActLED) s_pActLED->Blink(n); }

extern void setup(void);
extern void loop(void);
extern void usbMidiProcess(bool bPlugAndPlayUpdated);

static const u8 s_OwnIP[]  = { NET_OWN_IP };
static const u8 s_Mask[]   = { NET_NETMASK };
static const u8 s_GW[]     = { NET_GATEWAY };
static const u8 s_DNS[]    = { NET_DNS };

CKernel::CKernel(void) :
	m_Timer(&m_Interrupt),
	m_Logger(LogDebug, &m_Timer),
	m_Serial(&m_Interrupt, FALSE),
	m_Screen(1920, 1080),
	m_USBHCI(&m_Interrupt, &m_Timer, TRUE),
	m_CDCGadget(&m_Interrupt, 0x2E8A, 0x000A),
	m_EMMC(&m_Interrupt, &m_Timer, &m_ActLED),
	m_Net(s_OwnIP, s_Mask, s_GW, s_DNS, "looper"),
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

	return bOK;
}

TShutdownMode CKernel::Run(void)
{
	m_Logger.Write(log_name, LogNotice, "Looper starting %s", build_id);
	m_ActLED.Blink(1);  // 10: Run() entered

	// Initialize network with static IP — no DHCP
	m_Logger.Write(log_name, LogNotice, "Starting network (static 192.168.137.100)...");
	if (m_Net.Initialize(FALSE))
	{
		m_Logger.Write(log_name, LogNotice, "Network up");
		static const u8 logHostIP[] = { NET_LOG_HOST };
		CIPAddress logHost(logHostIP);
		m_pSysLog = new CSysLogDaemon(&m_Net, logHost);
		m_Logger.Write(log_name, LogNotice, "Syslog -> 192.168.137.1:514");
	}
	else
	{
		m_Logger.Write(log_name, LogWarning, "Network failed — continuing without syslog");
	}
	m_ActLED.Blink(1);  // 11: net done

	setup();
	m_ActLED.Blink(1);  // 12: setup() done

	// Open UDP socket for remote reboot command (send "REBOOT" to port 4444)
	CSocket *pRebootSocket = nullptr;
	if (m_pSysLog)
	{
		pRebootSocket = new CSocket(&m_Net, IPPROTO_UDP);
		if (pRebootSocket->Bind(4444) < 0)
		{
			delete pRebootSocket;
			pRebootSocket = nullptr;
			m_Logger.Write(log_name, LogWarning, "Could not bind reboot socket");
		}
		else
		{
			m_Logger.Write(log_name, LogNotice, "Reboot socket on UDP:4444 (send 'REBOOT')");
		}
	}

	bool bPlugAndPlayUpdated = FALSE;
	while (TRUE)
	{
		bPlugAndPlayUpdated = m_USBHCI.UpdatePlugAndPlay();
		m_CDCGadget.UpdatePlugAndPlay();
		if (m_pSysLog) m_Net.Process();
		usbMidiProcess(bPlugAndPlayUpdated);
		loop();
		m_Scheduler.Yield();

		// Check UDP reboot command
		if (pRebootSocket)
		{
			u8 buf[16];
			CIPAddress sender;
			u16 senderPort;
			int n = pRebootSocket->ReceiveFrom(buf, sizeof buf - 1, MSG_DONTWAIT, &sender, &senderPort);
			if (n >= 6 && memcmp(buf, "REBOOT", 6) == 0)
			{
				m_Logger.Write(log_name, LogNotice, "Reboot command received via UDP");
				return ShutdownReboot;
			}
		}

		// Check CDC serial reboot command
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
