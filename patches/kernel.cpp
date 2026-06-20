#include "kernel.h"
#include "abletonLink.h"
#include "wlanDHCP.h"
#include <circle/util.h>
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
static bool s_wlanIsAP = false;
static bool s_wlanJoinPending = false;   // deferred bring-up armed in Initialize()

// WLAN status for the :4445 "WLAN" debug verb (kernel_run.cpp). Lets the
// "must join/host ticker" requirement be verified live without relying on
// syslog. mode: 0=off/failed, 1=joined existing "ticker", 2=hosting AP.
extern "C" int wlanStatusCode(void)
{
	if (!s_wlanOK || !s_wlanJoined) return 0;
	return s_wlanIsAP ? 2 : 1;
}

// Runtime AP fallback: joined the existing "ticker" but it gave no DHCP lease
// (wlanDhcpFailed) -> own IP stuck, Link unroutable. Drop station mode and host
// our own "ticker" AP (where wlanDHCPServer leases peers). Idempotent: no-op once
// we are already the AP. Called from the control tick (kernel_run.cpp).
void wlanFallbackToAP(CBcm4343Device *pWLAN)
{
	if (s_wlanIsAP || !s_wlanOK) return;
	CLogger::Get()->Write("wlan", LogWarning, "ticker gave no DHCP lease -> AP fallback (CreateOpenNet)");
	if (pWLAN->CreateOpenNet(WLAN_SSID, 6, false))
	{
		s_wlanIsAP = true;
		s_wlanJoined = true;
		wlanApInit(pWLAN);
		CLogger::Get()->Write("wlan", LogNotice, "AP fallback up (hosting ticker)");
	}
	else
	{
		CLogger::Get()->Write("wlan", LogWarning, "AP fallback CreateOpenNet FAILED");
	}
}

// AP-yield (symmetric topology: either device hosts if none, joins if one exists).
// While WE host the "ticker" AP, periodically re-scan via JoinOpenNet: if another
// "ticker" AP is up (e.g. the esp came up / re-hosted), YIELD -- become a station
// and join it so exactly one host remains. This is the reciprocal of the esp's own
// tie-break (it drops its AP when a lower-BSSID ticker exists); together they
// converge in either MAC ordering. SAFETY: JoinOpenNet may tear the AP down even on
// failure, so on a failed join we re-CreateOpenNet to guarantee the radio ends
// usable (no silent AP loss). Returns true iff we yielded to a station.
bool wlanApYieldTry(CBcm4343Device *pWLAN)
{
	if (!s_wlanIsAP || !s_wlanOK) return false;
	if (pWLAN->JoinOpenNet(WLAN_SSID))
	{
		s_wlanIsAP = false;
		s_wlanJoined = true;
		CLogger::Get()->Write("wlan", LogNotice, "another ticker AP exists -> yielded, joined as station");
		u8 mac[6];
		pWLAN->GetMACAddress()->CopyTo(mac);
		wlanDhcpSendDiscover(pWLAN, mac);   // re-lease as a station
		return true;
	}
	// No other ticker found (or join failed): make sure OUR AP is still up.
	if (pWLAN->CreateOpenNet(WLAN_SSID, 6, false)) { s_wlanIsAP = true; s_wlanJoined = true; wlanApInit(pWLAN); }
	return false;
}

// Station re-association: when the joined "ticker" AP bounces (esp32 reboot, power
// save, range), the bcm4343 station drops association and JoinOpenNet (boot-only)
// never recovers -> no frames, peers expire, no Link. The control tick calls this
// when station RX has been stale for a while: re-JoinOpenNet and re-arm the DHCP
// discover so we re-lease. No-op while we are the AP. Returns true iff re-joined
// (caller resets its DHCP-done latch so wlanDhcpPoll retries the lease).
bool wlanStationRejoin(CBcm4343Device *pWLAN)
{
	if (s_wlanIsAP || !s_wlanOK) return false;
	CLogger::Get()->Write("wlan", LogWarning, "station RX stale -> re-JoinOpenNet(\"%s\")", WLAN_SSID);
	if (!pWLAN->JoinOpenNet(WLAN_SSID))
	{
		CLogger::Get()->Write("wlan", LogWarning, "re-join FAILED (ticker down?)");
		return false;
	}
	u8 mac[6];
	pWLAN->GetMACAddress()->CopyTo(mac);
	wlanDhcpSendDiscover(pWLAN, mac);   // re-arm lease (OFFER handled by the RX demux)
	CLogger::Get()->Write("wlan", LogNotice, "re-joined ticker; DHCP re-armed");
	return true;
}

// Deferred WLAN bring-up: the FIRST station join + AP fallback, run ONCE off the
// Core-2 control plane (after sockets bound + cores released) so a slow/absent
// ticker cannot wedge boot. No-op until armed by Initialize() and only ever runs
// once. JoinOpenNet (3x) -> CreateOpenNet fallback -> SetMulticastFilter -> arm
// the station DHCP discover. The JoinOpenNet scan may block Core 2 briefly, but
// the debug/reboot sockets are already live, so this delays only the join.
void wlanServiceBringUp(CBcm4343Device *pWLAN)
{
	if (!s_wlanJoinPending) return;
	s_wlanJoinPending = false;
	if (!s_wlanOK) return;

	CLogger::Get()->Write("wlan", LogNotice, "deferred bring-up: JoinOpenNet(\"%s\") ...", WLAN_SSID);
	for (int attempt = 1; attempt <= 3 && !s_wlanJoined; attempt++)
	{
		s_wlanJoined = pWLAN->JoinOpenNet(WLAN_SSID);
		CLogger::Get()->Write("wlan", LogNotice, "join attempt %d -> %s", attempt, s_wlanJoined ? "OK" : "FAILED");
		if (!s_wlanJoined && attempt < 3) CTimer::Get()->MsDelay(500);
	}
	if (!s_wlanJoined)
	{
		CLogger::Get()->Write("wlan", LogWarning, "join failed -> CreateOpenNet(\"%s\") [AP fallback]", WLAN_SSID);
		if (pWLAN->CreateOpenNet(WLAN_SSID, 6, false))
		{
			s_wlanIsAP = true;
			s_wlanJoined = true;
			wlanApInit(pWLAN);
		}
	}
	static const u8 mcastGroups[][6] = {
		{0x01, 0x00, 0x5e, 0x4c, 0x4e, 0x4b},
		{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
	};
	pWLAN->SetMulticastFilter(mcastGroups);
	// Station: arm the DHCP discover now that we are associated (OFFER handled by
	// the RX demux). AP mode leases peers via the AP DHCP server instead.
	if (s_wlanJoined && !s_wlanIsAP)
	{
		u8 mac[6];
		pWLAN->GetMACAddress()->CopyTo(mac);
		wlanDhcpSendDiscover(pWLAN, mac);
	}
	CLogger::Get()->Write("wlan", LogNotice, "deferred bring-up done (joined=%s ap=%s)",
		s_wlanJoined ? "yes" : "no", s_wlanIsAP ? "yes" : "no");
}

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
	m_AudioGadget(&m_Interrupt),
	m_EMMC(&m_Interrupt, &m_Timer, &m_ActLED),
	m_WLAN(WLAN_FIRMWARE_PATH),
	m_Net(s_OwnIP, s_Mask, s_GW, s_DNS, "looper"),
	m_pSysLog(nullptr)
#ifdef ARM_ALLOW_MULTI_CORE
	,m_CoreTask(this)
#endif
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

#ifdef ARM_ALLOW_MULTI_CORE
	if (bOK) bOK = m_CoreTask.Initialize();
	m_ActLED.Blink(1);
#endif

	if (m_Screen.Initialize())
	{
		m_Logger.Initialize(&m_Screen);
	}
	m_ActLED.Blink(1);

	m_Serial.Initialize(SERIAL_BAUD_RATE);
	m_ActLED.Blink(1);

	if (bOK) { boolean bAudio = m_AudioGadget.Initialize(); m_Logger.Write(log_name, LogNotice, "uac gadget init: %s", bAudio ? "OK" : "FAILED"); }
	m_ActLED.Blink(1);

	if (bOK) bOK = m_USBHCI.Initialize();
	m_ActLED.Blink(1);

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

	// WLAN/plan9 + Ableton-Link is OPT-IN (LOOPER_ENABLE_WLAN). The two failure
	// modes that originally forced it off are now mitigated in code:
	//   (1) the ~90s assert-halt was the p9 error-stack down-counter leak;
	//       patches/p9error.cpp self-heals (clamp/reseed, never asserts) and no
	//       longer does blocking syslog I/O on the error longjmp hot path
	//       (witnessed by scripts/test-p9-errorstack.cpp);
	//   (2) the "Core 2 control plane dead within a tick" SendFrame wedge is
	//       gated on association — abletonLink.cpp::linkProcess() early-returns
	//       unless CBcm4343Device::IsLinkUp(), plus a bounded 64-frame RX drain.
	// Still pending HARDWARE re-validation before it can default on (radio
	// stability under sustained audio load can't be host-proven); until then it
	// stays opt-in. Ethernet (boot/syslog/debug/MIDI sockets) is independent and
	// always up. Observe the live error rate via the :4445 WLAN verb (p9err=).
	// WLAN bring-up is DEFERRED to AFTER m_Net.Initialize() (below) so the
	// Ethernet network + syslog are live first. The radio init (plan9/bcm4343
	// SDIO) previously ran here, BEFORE the network — so a hang/wedge in it was
	// totally silent (no syslog, no :4445, no static .100), exactly the symptom
	// seen on hardware. Moved + instrumented so the last syslog line names the
	// exact call that wedges. See the block after m_Net.Initialize().
	m_ActLED.Blink(1);

	if (bOK) bOK = m_Net.Initialize(FALSE);
	m_ActLED.Blink(1);

#ifdef LOOPER_ENABLE_WLAN
	// Network is up now => these CLogger lines reach syslog (192.168.137.1:514),
	// so a hang in any single radio call is localized to the last line printed.
	m_Logger.Write(log_name, LogNotice, "WLAN: m_WLAN.Initialize() ...");
	s_wlanOK = m_WLAN.Initialize();
	m_Logger.Write(log_name, LogNotice, "WLAN: Initialize -> %s", s_wlanOK ? "OK" : "FAILED");
	// DO NOT join/host here. JoinOpenNet scans for the AP and CreateOpenNet brings
	// up a softAP — both can BLOCK for seconds (or wedge) on a slow/absent/just-
	// rebooted "ticker", and Initialize() runs BEFORE Run() binds the :4444/:4445/
	// :4446 sockets and releases the cores. A block here therefore strands boot
	// with no debug/reboot socket (witnessed: ICMP up but :4445 silent for minutes
	// after a REBOOT; only a power-cycle recovered). The bring-up is DEFERRED to a
	// one-shot on the Core-2 control plane (wlanServiceBringUp, run from
	// coreControlPlaneTick) so the sockets are always live within boot time and a
	// stuck join only delays the join, never the kernel.
	s_wlanJoinPending = s_wlanOK;
#endif

	return bOK;
}

TShutdownMode CKernel::Run(void)
{
	m_Logger.Write(log_name, LogNotice, "Looper starting %s", build_id);

	m_pSysLog = nullptr;

	m_Logger.Write(log_name, LogNotice, "WLAN init=%s join=%s ap=%s",
		s_wlanOK ? "OK" : "FAILED",
		s_wlanOK ? (s_wlanJoined ? "OK" : "FAILED") : "N/A",
		s_wlanIsAP ? "yes" : "no");

#ifdef LOOPER_ENABLE_WLAN
	linkInit(&m_WLAN);
#endif

	CSocket *pRebootSocket = new CSocket(&m_Net, IPPROTO_UDP);
	if (pRebootSocket->Bind(4444) < 0)
	{
		m_Logger.Write(log_name, LogWarning, "reboot socket bind failed on port 4444");
		delete pRebootSocket;
		pRebootSocket = nullptr;
	}

	CSocket *pDebugSocket = new CSocket(&m_Net, IPPROTO_UDP);
	if (pDebugSocket->Bind(4445) < 0)
	{
		m_Logger.Write(log_name, LogWarning, "debug socket bind failed on port 4445");
		delete pDebugSocket;
		pDebugSocket = nullptr;
	}

	CSocket *pMidiSocket = new CSocket(&m_Net, IPPROTO_UDP);
	if (pMidiSocket->Bind(4446) < 0)
	{
		m_Logger.Write(log_name, LogWarning, "midi socket bind failed on port 4446");
		delete pMidiSocket;
		pMidiSocket = nullptr;
	}

	setup();

	// WLAN station join + initial DHCP discover are NOT done here anymore — they
	// run in the deferred control-plane bring-up (wlanServiceBringUp) so a stuck
	// join cannot block boot before the sockets below start being polled.

#ifdef ARM_ALLOW_MULTI_CORE
	// Hand off control plane to Core 2. Socket polling now ALSO runs on Core 2
	// (alongside m_Net.Process) so all net-queue access is single-core — fixes
	// the cross-core ~CNetBuffer assert(!m_pNext) crash. Core 0 keeps only WFE +
	// USB completion ISRs + the reboot return (Core 2 sets g_netRebootRequested).
	extern void coreControlPlaneSetKernel(CKernel *pKernel);
	extern void coreControlPlaneSetSockets(CSocket *, CSocket *, CSocket *);
	extern volatile bool g_netRebootRequested;
	coreControlPlaneSetKernel(this);
	coreControlPlaneSetSockets(pRebootSocket, pDebugSocket, pMidiSocket);

	m_Logger.Write(log_name, LogNotice, "audio ready, releasing cores 1+2");
	coreSignalAudioReady();
	m_Logger.Write(log_name, LogNotice, "core 0 idle dispatch loop");

	while (TRUE)
	{
		if (g_netRebootRequested) return ShutdownReboot;
		asm volatile ("wfe" ::: "memory");
	}
#else
	m_Logger.Write(log_name, LogNotice, "entering single-core main loop");

	bool bPlugAndPlayUpdated = FALSE;
	bool bDhcpDone = !s_wlanJoined || s_wlanIsAP;
	while (TRUE)
	{
		bPlugAndPlayUpdated = m_USBHCI.UpdatePlugAndPlay();
		m_AudioGadget.UpdatePlugAndPlay();
		m_Net.Process();
		usbMidiProcess(bPlugAndPlayUpdated);
		loop();
		if (!bDhcpDone) bDhcpDone = wlanDhcpPoll(&m_WLAN);
		linkProcess();   // single RX demux: Link + AP-DHCP + client-DHCP frames
		m_Scheduler.Yield();

		TShutdownMode mode = pollSockets(pRebootSocket, pDebugSocket, pMidiSocket);
		if (mode != ShutdownNone) return mode;
	}
#endif

	return ShutdownHalt;
}
