#include "kernel.h"
#include "abletonLink.h"
#include <circle/util.h>
#include <circle/devicenameservice.h>

#define SERIAL_BAUD_RATE	115200
#define DRIVE			"SD:"
#define CDC_DEVICE_NAME		"utty1"
#define WLAN_FIRMWARE_PATH	"SD:/firmware/"
#define WLAN_SSID		"ticker"
#define LINK_PORT		20808

static const char log_name[] = "kernel";
static const char build_id[] = "BUILD-" __DATE__ "-" __TIME__;

static CActLED *s_pActLED = nullptr;
extern "C" void debug_blink(int n) { if (s_pActLED) s_pActLED->Blink(n); }

extern void setup(void);
extern void loop(void);
extern void usbMidiProcess(bool bPlugAndPlayUpdated);
extern void usbMidiInjectMidi(u8 status, u8 data1, u8 data2);

CKernel::CKernel(void) :
	m_Timer(&m_Interrupt),
	m_Logger(LogDebug, &m_Timer),
	m_Serial(&m_Interrupt, FALSE),
	m_Screen(1920, 1080),
	m_USBHCI(&m_Interrupt, &m_Timer, TRUE),
	m_CDCGadget(&m_Interrupt, 0x2E8A, 0x000A),
	m_EMMC(&m_Interrupt, &m_Timer, &m_ActLED),
	m_WLAN(WLAN_FIRMWARE_PATH),
	m_Net(0, 0, 0, 0, "looper", NetDeviceTypeWLAN),
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
	m_ActLED.Blink(1);

	if (bOK) bOK = m_Timer.Initialize();
	m_ActLED.Blink(1);

	if (m_Screen.Initialize())
		m_Logger.Initialize(&m_Screen);
	m_ActLED.Blink(1);

	m_Serial.Initialize(SERIAL_BAUD_RATE);
	m_ActLED.Blink(1);

	if (bOK) { boolean bCDC = m_CDCGadget.Initialize(); m_Logger.Write(log_name, LogNotice, "CDC gadget init: %s", bCDC ? "OK" : "FAILED"); }
	m_ActLED.Blink(1);

	if (bOK) bOK = m_USBHCI.Initialize();
	m_ActLED.Blink(1);

	m_Timer.MsDelay(500);
	m_CDCGadget.UpdatePlugAndPlay();
	m_ActLED.Blink(1);

	m_EMMC.Initialize();
	m_ActLED.Blink(1);

	f_mount(&m_FileSystem, DRIVE, 1);
	m_ActLED.Blink(1);

	if (bOK)
	{
		if (!m_WLAN.Initialize())
		{
			m_Logger.Write(log_name, LogWarning, "WLAN init failed");
			bOK = FALSE;
		}
	}
	m_ActLED.Blink(1);

	if (bOK)
	{
		if (!m_WLAN.JoinOpenNet(WLAN_SSID))
		{
			m_Logger.Write(log_name, LogWarning, "WLAN join failed");
			bOK = FALSE;
		}
	}
	m_ActLED.Blink(1);

	if (bOK) bOK = m_Net.Initialize(FALSE);
	m_ActLED.Blink(1);

	return bOK;
}

TShutdownMode CKernel::Run(void)
{
	m_Logger.Write(log_name, LogNotice, "Looper starting %s", build_id);

	while (!m_Net.IsRunning())
		m_Scheduler.MsSleep(100);

	m_Logger.Write(log_name, LogNotice, "Network up");
	static const u8 logHostIP[] = { NET_LOG_HOST };
	CIPAddress logHost(logHostIP);
	m_pSysLog = new CSysLogDaemon(&m_Net, logHost);

	CSocket *pLinkSocket = new CSocket(&m_Net, IPPROTO_UDP);
	if (pLinkSocket->Bind(LINK_PORT) < 0)
	{
		m_Logger.Write(log_name, LogWarning, "Link socket bind failed");
		delete pLinkSocket;
		pLinkSocket = nullptr;
	}
	else
	{
		static const u8 grp[] = {224, 76, 78, 75};
		CIPAddress grpAddr(grp);
		if (pLinkSocket->SetOptionAddMembership(grpAddr) < 0)
			m_Logger.Write(log_name, LogWarning, "Link multicast join failed");
		else
			m_Logger.Write(log_name, LogNotice, "Link multicast joined");
	}

	linkInit(&m_Net, pLinkSocket);

	CSocket *pRebootSocket = new CSocket(&m_Net, IPPROTO_UDP);
	if (pRebootSocket->Bind(4444) < 0)
	{
		delete pRebootSocket;
		pRebootSocket = nullptr;
	}

	CSocket *pMidiSocket = new CSocket(&m_Net, IPPROTO_UDP);
	if (pMidiSocket->Bind(4446) < 0)
	{
		delete pMidiSocket;
		pMidiSocket = nullptr;
	}

	setup();

	bool bPlugAndPlayUpdated = FALSE;
	while (TRUE)
	{
		bPlugAndPlayUpdated = m_USBHCI.UpdatePlugAndPlay();
		m_CDCGadget.UpdatePlugAndPlay();
		m_Net.Process();
		usbMidiProcess(bPlugAndPlayUpdated);
		loop();
		linkProcess();
		m_Scheduler.Yield();

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

		if (pMidiSocket)
		{
			u8 buf[16];
			CIPAddress sender;
			u16 senderPort;
			int n = pMidiSocket->ReceiveFrom(buf, sizeof buf, MSG_DONTWAIT, &sender, &senderPort);
			if (n >= 3)
				usbMidiInjectMidi(buf[0], buf[1], buf[2]);
		}

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
