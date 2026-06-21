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
extern "C" void loopClipTelemetry(int, int*, unsigned*, unsigned*, unsigned*, unsigned*, unsigned*);
extern "C" void loopMonitorTelemetry(int*, int*);
extern "C" unsigned p9ErrorCount(void);   // libwlan.a (patches/p9error.cpp)
extern "C" int wlanStatusCode(void);   // kernel.cpp: 0 off/fail, 1 joined, 2 AP
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
	{ extern void usbWavTick(void); usbWavTick(); }   // continuous ring-WAV dump to USB (Core 2)
#ifdef LOOPER_ENABLE_WLAN
	// WLAN/plan9 + Link is opt-in (see kernel.cpp). linkProcess() is now the
	// SINGLE radio-RX drainer and demuxes Link + AP-DHCP + client-DHCP frames, so
	// wlanDhcpServe()'s separate drain is gone (it used to eat inbound Link
	// packets). wlanDhcpPoll() is timeout-only bookkeeping now (no RX).
	// Deferred WLAN bring-up (FIRST join/AP) runs here, off the boot path, so a
	// slow/absent ticker delays only the join — the :4445/:4444 sockets are
	// already live. One-shot: no-ops after the first call.
	{
		extern void wlanServiceBringUp(CBcm4343Device *);
		wlanServiceBringUp(&k->m_WLAN);
	}
	if (!s_dhcpDone) {
		s_dhcpDone = wlanDhcpPoll(&k->m_WLAN);
		// Edge: joined the existing ticker but it leased nothing (capped retries).
		// Drop to hosting our own ticker AP so peers still rendezvous + get a lease.
		if (s_dhcpDone && wlanDhcpFailed() && wlanStatusCode() == 1) {
			extern void wlanFallbackToAP(CBcm4343Device *);
			wlanFallbackToAP(&k->m_WLAN);
		}
	}
	// Station re-association watchdog: when joined as a station, the esp32 "ticker"
	// AP broadcasts ALIVE ~1/s, so the radio RX count climbs steadily. If it goes
	// stale for >STALE_US while we are a station, association dropped (AP bounce) ->
	// re-join + re-lease. Throttled by resetting the timer each attempt.
	{
		extern unsigned linkRxFrameCount(void);
		extern bool wlanStationRejoin(CBcm4343Device *);
		static const unsigned STALE_US = 10u * CLOCKHZ;
		static unsigned s_lastRx = 0;
		static unsigned s_lastRxTicks = 0;
		unsigned rx  = linkRxFrameCount();
		unsigned now = CTimer::GetClockTicks();
		if (rx != s_lastRx || s_lastRxTicks == 0) { s_lastRx = rx; s_lastRxTicks = now; }
		else if (wlanStatusCode() == 1 && (now - s_lastRxTicks) > STALE_US) {
			if (wlanStationRejoin(&k->m_WLAN)) s_dhcpDone = false;  // let wlanDhcpPoll re-lease
			s_lastRxTicks = now;                                   // throttle re-attempts
		}
		// AP-yield: while WE host, if no station has joined and our RX is quiet (no
		// peer traffic), another ticker AP may be up elsewhere. Every ~20s try to
		// JoinOpenNet it and yield to a single host (symmetric any-config topology).
		// Only when hosting (status 2) and RX is stale (no joined peer talking to us).
		else if (wlanStatusCode() == 2 && (now - s_lastRxTicks) > 2u * STALE_US) {
			extern bool wlanApYieldTry(CBcm4343Device *);
			if (wlanApYieldTry(&k->m_WLAN)) s_dhcpDone = false;    // became station -> re-lease
			s_lastRxTicks = now;                                   // throttle
		}
	}
	// AP-fallback safety net (load-bearing for "always host when can't join"):
	// guarantee the radio ends USABLE. If ~40s after the first control tick (join
	// + DHCP window elapsed) we are NEITHER hosting our own AP (status 2) NOR a
	// station with a lease (status 1 + dhcpOK), force-host the AP. Covers
	// join-falsely-succeeded-no-lease, join-failed-and-CreateOpenNet-failed, and
	// off/failed states the existing dhcpFailed path misses. No-op if already AP.
	{
		extern void wlanFallbackToAP(CBcm4343Device *);
		extern bool wlanDhcpOK(void);
		static unsigned s_apCheck = 0;
		unsigned now2 = CTimer::GetClockTicks();
		if (s_apCheck == 0) s_apCheck = now2;
		else if ((now2 - s_apCheck) > 40u * CLOCKHZ) {
			int sc = wlanStatusCode();
			bool usable = (sc == 2) || (sc == 1 && wlanDhcpOK());
			if (!usable) wlanFallbackToAP(&k->m_WLAN);
			s_apCheck = now2;
		}
	}
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
			// "WLAN" verb: report ticker join/AP state + Link sync (verifies the
			// "must join or host ticker" requirement live, no syslog needed).
			if (buf[0] == 'W' && buf[1] == 'L' && buf[2] == 'A' && buf[3] == 'N')
			{
#ifdef LOOPER_ENABLE_WLAN
				extern int  wlanDhcpAttempts(void);
				extern bool wlanDhcpOK(void);
				extern unsigned wlanDhcpRxSeen(void);
				extern unsigned wlanDhcpOffersSeen(void);
				extern unsigned linkRxFrameCount(void);
				int wc = wlanStatusCode();
				const char *mode = wc == 2 ? "hosting-ticker"
				                 : wc == 1 ? "joined-ticker" : "off/failed";
				int dhcpOK = wlanDhcpOK() ? 1 : 0;
				int dhcpAtt = wlanDhcpAttempts();
				extern unsigned linkUniRxToUs(void);
				extern unsigned linkClkRx(void);
				unsigned rxF = linkRxFrameCount();
				unsigned dRx = wlanDhcpRxSeen();
				unsigned dOf = wlanDhcpOffersSeen();
				unsigned uni = linkUniRxToUs();
				unsigned clk = linkClkRx();
#else
				const char *mode = "disabled";
				int dhcpOK = 0, dhcpAtt = 0;
				unsigned rxF = 0, dRx = 0, dOf = 0, uni = 0, clk = 0;
#endif
				CString s;
				s.Format("wlan=%s dhcpOK=%d dhcpAtt=%d rxFrames=%u uniRx=%u clkRx=%u dhcpRx=%u dhcpOffers=%u link=%s bpm=%d p9err=%u", mode,
					dhcpOK, dhcpAtt, rxF, uni, clk, dRx, dOf,
					linkIsSynced() ? "synced" : "no", (int)linkGetBPM(),
					p9ErrorCount());
				pDebug->SendTo((u8 *)(const char *)s, s.GetLength(), MSG_DONTWAIT, sender, port);
			}
			else if (buf[0]=='L' && buf[1]=='T' && buf[2]=='X')
			{
				// Toggle proactive Link WiFi TX live to A/B-test the 1Hz audio glitch.
				extern volatile bool g_linkProactiveTx;
				if (n >= 4 && buf[3]=='0') g_linkProactiveTx = false;
				else if (n >= 4 && buf[3]=='1') g_linkProactiveTx = true;
				CString s; s.Format("linkProactiveTx=%d", g_linkProactiveTx ? 1 : 0);
				pDebug->SendTo((u8 *)(const char *)s, s.GetLength(), MSG_DONTWAIT, sender, port);
			}
			else if (buf[0]=='L' && buf[1]=='I' && buf[2]=='N' && buf[3]=='K')
			{
				// Full Ableton Link state, capture-free (OUR view; no Live-side
				// capture needed): peers, who owns, measured owner offset,
				// ping/pong counters, shared beat phase, tempo.
				extern void linkTelemetry(unsigned*, s64*, unsigned*, unsigned*, int*);
				extern void linkNetTelemetry(u8*, int*, u8*, int*, int*);
				extern void linkMeasCounters(unsigned*, unsigned*);
				extern bool linkGhostPhase(s64*, s64*);
				unsigned peers=0, pingsTx=0, pongsRx=0; s64 offUs=0; int selfOwns=1;
				linkTelemetry(&peers, &offUs, &pingsTx, &pongsRx, &selfOwns);
				unsigned pingsRx=0, pongsTx=0; linkMeasCounters(&pingsRx, &pongsTx);
				u8 ownIp[4]={0}, peerIp[4]={0}; int dhcp=0, peerPort=0, peerHasEp=0;
				linkNetTelemetry(ownIp, &dhcp, peerIp, &peerPort, &peerHasEp);
				s64 ph=0, q=4000000; int phaseValid = linkGhostPhase(&ph, &q) ? 1 : 0;
				int beatPhase100 = (q > 0) ? (int)((ph * 100) / q) : 0;
				CString s;
				s.Format("synced=%d peers=%u owner=%s offsetUs=%d pingsTx=%u pongsRx=%u pingsRx=%u pongsTx=%u phaseValid=%d beatPhase100=%d bpm=%d ownip=%u.%u.%u.%u dhcp=%d peerip=%u.%u.%u.%u:%d peerEp=%d",
					linkIsSynced() ? 1 : 0, peers, selfOwns ? "self" : "peer",
					(int)offUs, pingsTx, pongsRx, pingsRx, pongsTx, phaseValid, beatPhase100, (int)linkGetBPM(),
					ownIp[0],ownIp[1],ownIp[2],ownIp[3], dhcp,
					peerIp[0],peerIp[1],peerIp[2],peerIp[3], peerPort, peerHasEp);
				pDebug->SendTo((u8 *)(const char *)s, s.GetLength(), MSG_DONTWAIT, sender, port);
			}
			else if (buf[0]=='L' && buf[1]=='M' && buf[2]=='S' && buf[3]=='G')
			{
				// Hex of the last measurement (_link_v) frame a peer sent us =
				// ground-truth ping/pong wire format to diff against our codec.
				extern int linkLastMeas(u8*, int, int*);
				u8 raw[64]; int mtype=-1; int n = linkLastMeas(raw, sizeof raw, &mtype);
				CString s; CString h;
				s.Format("lastMeasType=%d len=%d hex=", mtype, n);
				for (int i = 0; i < n; i++) { h.Format("%02x", raw[i]); s.Append(h); }
				pDebug->SendTo((u8 *)(const char *)s, s.GetLength(), MSG_DONTWAIT, sender, port);
			}
			else if (buf[0]=='T' && buf[1]=='A' && buf[2]=='L' && buf[3]=='V')
			{
				// Our last transmitted ALIVE (hex) — diff vs RALV (Live's ALIVE).
				extern int linkLastTxAlive(u8*, int);
				u8 raw[128]; int n = linkLastTxAlive(raw, sizeof raw);
				CString s; CString h; s.Format("txAlive len=%d hex=", n);
				for (int i = 0; i < n; i++) { h.Format("%02x", raw[i]); s.Append(h); }
				pDebug->SendTo((u8 *)(const char *)s, s.GetLength(), MSG_DONTWAIT, sender, port);
			}
			else if (buf[0]=='R' && buf[1]=='A' && buf[2]=='L' && buf[3]=='V')
			{
				// Last ALIVE received from a peer (Live) = ground-truth valid frame.
				extern int linkLastRxAlive(u8*, int);
				u8 raw[128]; int n = linkLastRxAlive(raw, sizeof raw);
				CString s; CString h; s.Format("rxAlive len=%d hex=", n);
				for (int i = 0; i < n; i++) { h.Format("%02x", raw[i]); s.Append(h); }
				pDebug->SendTo((u8 *)(const char *)s, s.GetLength(), MSG_DONTWAIT, sender, port);
			}
			else if (buf[0]=='R' && buf[1]=='F' && buf[2]=='R' && buf[3]=='M')
			{
				// Full received discovery frame (Eth+IP+UDP) — read Live's IP TTL
				// (offset 22), UDP checksum (offset 40), flags etc. vs ours.
				extern int linkLastRxFrame(u8*, int);
				u8 raw[180]; int n = linkLastRxFrame(raw, sizeof raw);
				CString s; CString h; s.Format("rxFrame len=%d hex=", n);
				for (int i = 0; i < n; i++) { h.Format("%02x", raw[i]); s.Append(h); }
				pDebug->SendTo((u8 *)(const char *)s, s.GetLength(), MSG_DONTWAIT, sender, port);
			}
			else if (buf[0]=='T' && buf[1]=='I' && buf[2]=='M' && buf[3]=='E')
			{
				extern volatile u32 g_cbWriteBlock;
				extern volatile u32 g_cbLastBackdateSamples;
				extern volatile u32 g_cbLastPressLatencyUs;
				extern volatile u32 g_cbLastBackdateClamped;
				extern volatile u32 g_cbExtraLagSamples;
				extern volatile u32 g_lastGridStep;
				extern volatile u32 g_lastLatchPhase;
				int monitor = 0, loopGate100 = 100;
				loopMonitorTelemetry(&monitor, &loopGate100);
				extern volatile u32 g_microRepeatDiv;
				extern volatile u32 g_samplerRec, g_samplerDrumMode, g_samplerLen,
				                    g_samplerDrumCount, g_samplerVoices;
				CString s;
				s.Format("backdate=%u latUs=%u clamped=%u extraLag=%u gridStep=%u latchPhase=%u cbwr=%u started=%d ended=%d qbeats100=%d bpm=%d monitor=%d loopGate=%d microRep=%u sampRec=%u drumMode=%u sampLen=%u drumLoaded=%u voices=%u",
					(unsigned)g_cbLastBackdateSamples,
					(unsigned)g_cbLastPressLatencyUs,
					(unsigned)g_cbLastBackdateClamped,
					(unsigned)g_cbExtraLagSamples,
					(unsigned)g_lastGridStep,
					(unsigned)g_lastLatchPhase,
					(unsigned)g_cbWriteBlock,
					linkHasStarted() ? 1 : 0,
					linkHasEnded() ? 1 : 0,
					(int)(linkQuantBeats() * 100),
					(int)linkGetBPM(),
					monitor,
					loopGate100,
					(unsigned)g_microRepeatDiv,
					(unsigned)g_samplerRec,
					(unsigned)g_samplerDrumMode,
					(unsigned)g_samplerLen,
					(unsigned)g_samplerDrumCount,
					(unsigned)g_samplerVoices);
				pDebug->SendTo((u8 *)(const char *)s, s.GetLength(), MSG_DONTWAIT, sender, port);
			}
			else if (buf[0]=='C' && buf[1]=='L' && buf[2]=='I' && buf[3]=='P')
			{
				// Clip-0 FSM telemetry for track 0 (or "CLIP<n>" for track n):
				// state/play/rec/num/max/running — witnesses the first-loop seam
				// and wrap live without audio capture.
				int t = (n >= 5 && buf[4] >= '0' && buf[4] <= '9') ? (buf[4]-'0') : 0;
				int st = -1; unsigned pl=0, rc=0, nb=0, mx=0, run=0;
				loopClipTelemetry(t, &st, &pl, &rc, &nb, &mx, &run);
				CString s;
				s.Format("track=%d state=%d play=%u rec=%u num=%u max=%u running=%u",
					t, st, pl, rc, nb, mx, run);
				pDebug->SendTo((u8 *)(const char *)s, s.GetLength(), MSG_DONTWAIT, sender, port);
			}
			else if (buf[0]=='U' && buf[1]=='D' && buf[2]=='S' && buf[3]=='C')
			{
				// Raw config descriptor (hex) of the last UAC2 device enumerated
				// (Tascam US-2x2). Grounds UAC2 host bring-up: decode AS interfaces,
				// Clock Source entity, Type-I format/subslot, iso + feedback EPs.
				extern u8 g_uac2Desc[]; extern volatile unsigned g_uac2DescLen;
				unsigned dn = g_uac2DescLen; if (dn > 500) dn = 500;  // 1000 hex chars fits one UDP datagram
				CString s; CString h; s.Format("uac2desc len=%u hex=", g_uac2DescLen);
				for (unsigned i = 0; i < dn; i++) { h.Format("%02x", g_uac2Desc[i]); s.Append(h); }
				pDebug->SendTo((u8 *)(const char *)s, s.GetLength(), MSG_DONTWAIT, sender, port);
			}
			else if (buf[0]=='U' && buf[1]=='A' && buf[2]=='U' && buf[3]=='D')
			{
				// Live USB-audio status: IN/OUT device bound, the bound IN device's
				// format, and the IN delivery counter + peak. inDeliv climbing +
				// inPeak>0 == the (US-2x2 UAC2) input is delivering audio. Globals
				// live in libusb (always linked), so no USE_USB_AUDIO guard.
				extern volatile unsigned g_audioInBound, g_audioOutBound, g_audioUAC2,
				                         g_audioChannels, g_audioSubslot, g_audioRate,
				                         g_audioInDeliv, g_audioInPeak,
				                         g_audioOutDeliv, g_audioOutPeak, g_audioOutSubmitFail;
				// Glitch diagnosis: per-side underrun/resync counters (the 1Hz Tascam
				// glitch should show as one side incrementing ~once/sec). These globals
				// already live in libusb (input_usb/output_usb), so no lib rebuild.
				extern volatile unsigned g_inUnderruns, g_inResyncs, g_outUnderruns, g_otgResyncs;
				extern volatile unsigned g_audioFbRate, g_audioFbCount, g_audioOutMaxGapUs;
				extern volatile unsigned g_outRingResync;
				CString s;
				s.Format("audioIn=%u audioOut=%u uac2=%u ch=%u bits=%u rate=%u inDeliv=%u inPeak=%u outDeliv=%u outPeak=%u outFail=%u inUR=%u inRS=%u outUR=%u otgRS=%u outRingRS=%u fbRate=%u fbCnt=%u outMaxGapUs=%u",
					g_audioInBound, g_audioOutBound, g_audioUAC2, g_audioChannels,
					g_audioSubslot*8, g_audioRate, g_audioInDeliv, g_audioInPeak,
					g_audioOutDeliv, g_audioOutPeak, g_audioOutSubmitFail,
					g_inUnderruns, g_inResyncs, g_outUnderruns, g_otgResyncs, g_outRingResync,
					g_audioFbRate, g_audioFbCount, g_audioOutMaxGapUs);
				g_audioOutMaxGapUs = 0;   // reset so each probe shows the max since last read
				pDebug->SendTo((u8 *)(const char *)s, s.GetLength(), MSG_DONTWAIT, sender, port);
			}
			else if (buf[0]=='U' && buf[1]=='W' && buf[2]=='A' && buf[3]=='V')
			{
				// Continuous ring-WAV dump to a USB drive. mounted=1 => a drive is
				// present and looper-rec.wav is being written; bytes climbing => audio
				// flowing to it; wraps>0 => the ring has looped over the full file.
				extern volatile unsigned g_uwavMounted, g_uwavWraps;
				extern volatile unsigned long long g_uwavBytes, g_uwavMaxData;
				CString s;
				s.Format("mounted=%u bytes=%llu maxData=%llu wraps=%u",
					g_uwavMounted, g_uwavBytes, g_uwavMaxData, g_uwavWraps);
				pDebug->SendTo((u8 *)(const char *)s, s.GetLength(), MSG_DONTWAIT, sender, port);
			}
			else if (buf[0]=='M' && buf[1]=='I' && buf[2]=='D' && buf[3]=='I')
			{
				// USB MIDI roster + flow: which umidi1..8 slots enumerated
				// (slots bitmask + count), MIDI-in packet count, and MIDI-OUT
				// drop/error counters. Diagnoses "APC dark": slots=0 => the APC
				// never enumerated as a MIDI device; slots>0 + inPkts climbing on
				// a pad press => input works; outDrop climbing => LED sends drop.
				extern void usbMidiTelemetry(unsigned*, int*, unsigned*, unsigned*, unsigned*);
				unsigned mask=0, inPkts=0, outDrop=0, outErr=0; int cnt=0;
				usbMidiTelemetry(&mask, &cnt, &inPkts, &outDrop, &outErr);
				CString s;
				s.Format("midiDevices=%d slots=0x%02x inPkts=%u outDrop=%u outErr=%u",
					cnt, mask, inPkts, outDrop, outErr);
				pDebug->SendTo((u8 *)(const char *)s, s.GetLength(), MSG_DONTWAIT, sender, port);
			}
			else {
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
