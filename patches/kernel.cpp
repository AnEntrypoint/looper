#include "kernel.h"
#include "abletonLink.h"
#include "wlanDHCP.h"
#include <circle/util.h>
#include <circle/string.h>
#include <circle/devicenameservice.h>
#include <circle/net/in.h>
#include <circle/net/socket.h>
#include "p9chan.h"

extern "C" {
extern const unsigned char wlan_bin[];
extern const unsigned long wlan_bin_size;
extern const unsigned char wlan_txt[];
extern const unsigned long wlan_txt_size;
extern const unsigned char wlan_clm[];
extern const unsigned long wlan_clm_size;
}

#define SERIAL_BAUD_RATE	115200
#define DRIVE			"SD:"
#define CDC_DEVICE_NAME		"utty1"
#define WLAN_FIRMWARE_PATH	"SD:/firmware/"
#define WLAN_SSID		"ticker"

static const char log_name[] = "kernel";
static const char build_id[] = "BUILD-" __DATE__ "-" __TIME__;

static const u8 s_OwnIP[] = { NET_OWN_IP };
static const u8 s_Mask[]  = { NET_NETMASK };
static const u8 s_GW[]    = { NET_GATEWAY };
static const u8 s_DNS[]   = { NET_DNS };

static CActLED *s_pActLED = nullptr;
extern "C" void debug_blink(int n) { if (s_pActLED) s_pActLED->Blink(n); }
static bool s_wlanOK = false;
static bool s_wlanJoined = false;

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

	static const p9fw_entry s_wlanFW[] = {
		{ "brcmfmac43455-sdio.bin",      wlan_bin, wlan_bin_size },
		{ "brcmfmac43455-sdio.txt",      wlan_txt, wlan_txt_size },
		{ "brcmfmac43455-sdio.clm_blob", wlan_clm, wlan_clm_size },
	};
	p9chan_set_firmware(s_wlanFW, 3);

	s_wlanOK = m_WLAN.Initialize();
	if (s_wlanOK)
		s_wlanJoined = m_WLAN.JoinOpenNet(WLAN_SSID);
	m_ActLED.Blink(1);

	if (bOK) bOK = m_Net.Initialize(FALSE);
	m_ActLED.Blink(1);

	return bOK;
}

TShutdownMode CKernel::Run(void)
{
	m_Logger.Write(log_name, LogNotice, "Looper starting %s", build_id);

	static const u8 logHostIP[] = { NET_LOG_HOST };
	CIPAddress logHost(logHostIP);
	m_pSysLog = new CSysLogDaemon(&m_Net, logHost);

	m_Logger.Write(log_name, LogNotice, "WLAN init=%s join=%s",
		s_wlanOK ? "OK" : "FAILED",
		s_wlanOK ? (s_wlanJoined ? "OK" : "FAILED") : "N/A");

	linkInit(&m_WLAN);

	CSocket *pRebootSocket = new CSocket(&m_Net, IPPROTO_UDP);
	if (pRebootSocket->Bind(4444) < 0) { delete pRebootSocket; pRebootSocket = nullptr; }

	CSocket *pDebugSocket = new CSocket(&m_Net, IPPROTO_UDP);
	if (pDebugSocket->Bind(4445) < 0) { delete pDebugSocket; pDebugSocket = nullptr; }

	CSocket *pMidiSocket = new CSocket(&m_Net, IPPROTO_UDP);
	if (pMidiSocket->Bind(4446) < 0) { delete pMidiSocket; pMidiSocket = nullptr; }

	setup();

	m_Logger.Write(log_name, LogNotice, "entering main loop");

	bool bPlugAndPlayUpdated = FALSE;
	unsigned nLastHeartbeat = m_Timer.GetClockTicks();
	bool bDhcpDone = !s_wlanJoined;
	if (s_wlanJoined) {
		u8 mac[6];
		m_WLAN.GetMACAddress()->CopyTo(mac);
		wlanDhcpSendDiscover(&m_WLAN, mac);
	}
	while (TRUE)
	{
		bPlugAndPlayUpdated = m_USBHCI.UpdatePlugAndPlay();
		m_CDCGadget.UpdatePlugAndPlay();
		m_Net.Process();
		usbMidiProcess(bPlugAndPlayUpdated);
		loop();
		if (!bDhcpDone) bDhcpDone = wlanDhcpPoll(&m_WLAN);
		linkProcess();
		m_Scheduler.Yield();

		unsigned nNow = m_Timer.GetClockTicks();
		if (nNow - nLastHeartbeat >= 10 * CLOCKHZ)
		{
			m_Logger.Write(log_name, LogNotice, "heartbeat wlan=%s", s_wlanJoined ? "joined" : "no");
			nLastHeartbeat = nNow;
		}

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

		if (pDebugSocket)
		{
			u8 buf[32];
			CIPAddress sender;
			u16 senderPort;
			int n = pDebugSocket->ReceiveFrom(buf, sizeof buf - 1, MSG_DONTWAIT, &sender, &senderPort);
			if (n > 0)
			{
				buf[n] = 0;
				CString reply;
				reply.Format("wlan=%s link=%s bpm=%d uptime=%u",
					s_wlanJoined ? "joined" : "no",
					linkIsSynced() ? "synced" : "no",
					(int)linkGetBPM(),
					m_Timer.GetClockTicks() / CLOCKHZ);
				pDebugSocket->SendTo((u8 *)(const char *)reply, reply.GetLength(), MSG_DONTWAIT, sender, senderPort);
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
