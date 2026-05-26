#include "kernel.h"
#include "abletonLink.h"
#include "wlanDHCP.h"
#include <circle/util.h>
#include <circle/string.h>
#include <circle/devicenameservice.h>
#include <circle/net/in.h>
#include <circle/net/socket.h>

extern void usbMidiInjectMidi(u8 status, u8 data1, u8 data2);
extern "C" int engineQueryDispatch(const char *req, char *out, int outsz);
extern void usbMidiProcess(bool bPlugAndPlayUpdated);
extern void loop(void);

static const char kCDCDev[] = "utty1";
static const char kLog[]    = "kernel";

#ifdef ARM_ALLOW_MULTI_CORE

// Core 2 control plane. Owns USB plug-and-play poll, network stack,
// usbMidi update, audio.cpp::loop (telemetry+watchdog+APC update),
// Ableton Link RX/TX, WiFi DHCP, scheduler yield. Reboot socket poll
// stays on Core 0 (CKernel::Run) so the shutdown return path still
// works without IPC.

static CKernel *s_pKernel = nullptr;
static bool     s_dhcpDone = false;
static bool     s_dhcpDiscoverSent = false;

// Socket polling MUST run on the SAME core as m_Net.Process() (Core 2). The
// net buffer queues are Enqueued by Process() (IP demux -> per-socket RX queue)
// and Dequeued by CSocket::ReceiveFrom(); doing those on two different cores
// (Process on Core 2, pollSockets on Core 0) is a cross-core race that frees a
// still-linked CNetBuffer -> netbuffer.cpp(102) assert(!m_pNext) crash (seen at
// 17s with v2, pushed to 66s with the Flush v3 lock fix but never eliminated,
// because the locking can't make a buffer dequeued-on-Core-0 safe against a
// concurrent Process-on-Core-2). Pinning ALL net access to Core 2 removes the
// race at the source. Core 0 keeps only WFE + USB completion ISRs + the reboot
// return; Core 2 sets s_rebootRequested which Core 0's loop polls.
static CSocket *s_pReboot = nullptr;
static CSocket *s_pDebug  = nullptr;
static CSocket *s_pMidi   = nullptr;
volatile bool   g_netRebootRequested = false;

void coreControlPlaneSetSockets(CSocket *pReboot, CSocket *pDebug, CSocket *pMidi)
{
	s_pReboot = pReboot;
	s_pDebug  = pDebug;
	s_pMidi   = pMidi;
}

void coreControlPlaneSetKernel(CKernel *pKernel)
{
	s_pKernel = pKernel;
}

void coreControlPlaneTick(void)
{
	CKernel *k = s_pKernel;
	if (!k) return;

	bool bPnP = k->m_USBHCI.UpdatePlugAndPlay();
	k->m_AudioGadget.UpdatePlugAndPlay();
	k->m_Net.Process();
	// Poll the UDP sockets on THIS core (same as Process) so all net-queue
	// Enqueue/Dequeue is single-core — no cross-core ~CNetBuffer race.
	if (k->pollSockets(s_pReboot, s_pDebug, s_pMidi) == ShutdownReboot)
		g_netRebootRequested = true;
	usbMidiProcess(bPnP);
	loop();
#ifdef LOOPER_ENABLE_WLAN
	// WLAN/plan9 + Link disabled by default (p9 stack-assert crash ~90s in).
	if (!s_dhcpDone) s_dhcpDone = wlanDhcpPoll(&k->m_WLAN);
	wlanDhcpServe();
	linkProcess();
#endif
	k->m_Scheduler.Yield();
}

#endif

TShutdownMode CKernel::pollSockets(CSocket *pReboot, CSocket *pDebug, CSocket *pMidi)
{
	if (pReboot)
	{
		u8 buf[16];
		CIPAddress sender;
		u16 port;
		int n = pReboot->ReceiveFrom(buf, sizeof buf - 1, MSG_DONTWAIT, &sender, &port);
		if (n >= 6 && memcmp(buf, "REBOOT", 6) == 0)
		{
			m_Logger.Write(kLog, LogNotice, "Reboot command received via UDP");
			return ShutdownReboot;
		}
	}

	if (pDebug)
	{
		u8 buf[32];
		CIPAddress sender;
		u16 port;
		int n = pDebug->ReceiveFrom(buf, sizeof buf - 1, MSG_DONTWAIT, &sender, &port);
		if (n > 0)
		{
			buf[n] = 0;
			// On-demand engine observability/control (Core 2 plane, zero audio
			// impact — only reads/writes plain fields when a query arrives).
			// Routed via a thin extern in audio.cpp so this file needn't include
			// the audio engine header.
			char rep[256];
			int rn = engineQueryDispatch((const char *)buf, rep, sizeof rep);
			if (rn <= 0) {
				CString s;
				s.Format("link=%s bpm=%d uptime=%u",
					linkIsSynced() ? "synced" : "no",
					(int)linkGetBPM(),
					m_Timer.GetClockTicks() / CLOCKHZ);
				pDebug->SendTo((u8 *)(const char *)s, s.GetLength(), MSG_DONTWAIT, sender, port);
			} else {
				pDebug->SendTo((u8 *)rep, rn, MSG_DONTWAIT, sender, port);
			}
		}
	}

	if (pMidi)
	{
		u8 buf[16];
		CIPAddress sender;
		u16 port;
		int n = pMidi->ReceiveFrom(buf, sizeof buf, MSG_DONTWAIT, &sender, &port);
		if (n >= 3)
			usbMidiInjectMidi(buf[0], buf[1], buf[2]);
	}

	CDevice *pCDC = CDeviceNameService::Get()->GetDevice(kCDCDev, FALSE);
	if (pCDC != nullptr)
	{
		u8 c;
		if (pCDC->Read(&c, 1) == 1 && c == 'R')
			return ShutdownReboot;
	}

	return ShutdownNone;
}
