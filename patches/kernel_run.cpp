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

// Must match usbdevicefactory.cpp's RAWD_SNAP_SAMPLES exactly (the array
// size the g_audioInSnapL/R extern globals were allocated with).
#define RAWD_SNAP_SAMPLES 128
extern void usbMidiProcess(bool bPlugAndPlayUpdated);
extern void gamepadProcess(bool bPlugAndPlayUpdated);
extern void gamepadProcessTick(void);
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
	gamepadProcess(bPnP);       // bind/unbind USB gamepad on plug/unplug (Core 2)
	gamepadProcessTick();       // drain gamepad snapshot -> control surface (Core 2)
	loop();
	{ extern void usbWavTick(void); usbWavTick(); }   // continuous ring-WAV dump to USB (Core 2)
	{ extern void loopDumpTick(void); loopDumpTick(); }   // per-track dump-on-demand (Core 2)
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
			// "BUID" verb: report the RUNNING kernel's build identity (compile
			// date+time string) so a fix/flash/verify cycle can confirm the
			// flashed binary actually booted, without a syslog listener. See
			// KernelGetBuildId() in kernel.cpp for why this exists.
			if (buf[0] == 'B' && buf[1] == 'U' && buf[2] == 'I' && buf[3] == 'D')
			{
				extern const char *KernelGetBuildId(void);
				const char *bid = KernelGetBuildId();
				pDebug->SendTo((u8 *)bid, (int)strlen(bid), MSG_DONTWAIT, sender, port);
			}
			// "WLAN" verb: report ticker join/AP state + Link sync (verifies the
			// "must join or host ticker" requirement live, no syslog needed).
			else if (buf[0] == 'W' && buf[1] == 'L' && buf[2] == 'A' && buf[3] == 'N')
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
				extern unsigned linkTtmpRx(void);
				unsigned ttmp = linkTtmpRx();
#else
				const char *mode = "disabled";
				int dhcpOK = 0, dhcpAtt = 0;
				unsigned rxF = 0, dRx = 0, dOf = 0, uni = 0, clk = 0, ttmp = 0;
#endif
				CString s;
				s.Format("wlan=%s dhcpOK=%d dhcpAtt=%d rxFrames=%u uniRx=%u clkRx=%u ttmpRx=%u dhcpRx=%u dhcpOffers=%u link=%s bpm=%d p9err=%u", mode,
					dhcpOK, dhcpAtt, rxF, uni, clk, ttmp, dRx, dOf,
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
			else if (buf[0]=='D' && buf[1]=='I' && buf[2]=='A' && buf[3]=='G')
			{
				// Graph-tick diagnostics: why is the audio graph dead (cbwr=0/outWr=0)
				// while IN URBs fire? nInUpd stuck != 0 + outWr frozen == Core 1 not
				// draining the dispatch ring; dispDrop climbing == ring overflow; ready=0
				// == Core 1 never released; c1busy=0 == Core 1 never ran.
				extern volatile unsigned g_outWrites;
				extern volatile unsigned g_dispatchDropped;
				extern volatile bool g_coreAudioReady;
				extern volatile u64 g_coreBusyTicks[4];
				extern volatile u64 g_coreIdleTicks[4];
				extern unsigned AudioSystem_nInUpdate(void);
				extern unsigned AudioSystem_numOverflows(void);
				extern unsigned AudioSystem_updateScheduled(void);
				extern volatile unsigned g_diagInHandler, g_diagInBlockCross, g_diagInResp;
				extern volatile unsigned g_outUpdEntered, g_outNumConn;
				extern volatile unsigned g_diagWalkN, g_diagTypeMask;
				extern volatile unsigned g_diagOutGateHit;
				CString s;
				s.Format("outWr=%u outUpd=%u outConn=%u walkN=%u typeMask=%x outGate=%u dispDrop=%u nInUpd=%u overflows=%u updSched=%u ready=%d inHnd=%u inXing=%u inResp=%u c1busy=%u c1idle=%u",
					(unsigned)g_outWrites, (unsigned)g_outUpdEntered, (unsigned)g_outNumConn, (unsigned)g_diagWalkN, (unsigned)g_diagTypeMask, (unsigned)g_diagOutGateHit, (unsigned)g_dispatchDropped,
					AudioSystem_nInUpdate(), AudioSystem_numOverflows(), AudioSystem_updateScheduled(),
					g_coreAudioReady?1:0,
					(unsigned)g_diagInHandler, (unsigned)g_diagInBlockCross, (unsigned)g_diagInResp,
					(unsigned)g_coreBusyTicks[1], (unsigned)g_coreIdleTicks[1]);
				pDebug->SendTo((u8 *)(const char *)s, s.GetLength(), MSG_DONTWAIT, sender, port);
			}
			else if (buf[0]=='G' && buf[1]=='P' && buf[2]=='A' && buf[3]=='D')
			{
				// USB gamepad state (capture-free witness of the mapping):
				// connected, axis count, button bitmask, current glitch note,
				// dropped snapshots, and whether SHIFT(L1)/reverb(R1) are held.
				extern void gamepadTelemetry(int*, int*, unsigned*, int*, unsigned*, int*, int*);
				int conn=0, axes=0, hatNote=0, shift=0, reverb=0;
				unsigned buttons=0, dropped=0;
				gamepadTelemetry(&conn, &axes, &buttons, &hatNote, &dropped, &shift, &reverb);
				CString s;
				s.Format("connected=%d axes=%d buttons=%x hat=%d dropped=%u shift=%d reverb=%d",
					conn, axes, buttons, hatNote, dropped, shift, reverb);
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
				                         g_audioInDeliv, g_audioInPeak, g_audioInSubmitFail,
				                         g_audioOutDeliv, g_audioOutPeak, g_audioOutSubmitFail;
				// Glitch diagnosis: per-side underrun/resync counters (the 1Hz Tascam
				// glitch should show as one side incrementing ~once/sec). These globals
				// already live in libusb (input_usb/output_usb), so no lib rebuild.
				extern volatile unsigned g_inUnderruns, g_inResyncs, g_outUnderruns, g_otgResyncs;
				extern volatile unsigned g_audioFbRate, g_audioFbCount, g_audioOutMaxGapUs;
				extern volatile unsigned g_audioInMaxGapUs;
				extern volatile unsigned g_outRingResync;
				extern volatile unsigned g_outWrites;
				extern volatile unsigned g_audioInZcL, g_audioInZcR;
				extern volatile unsigned long long g_audioInEnergyL, g_audioInEnergyR;
				extern volatile unsigned g_audioInEnergyN;
				extern volatile unsigned g_audioInSlot0Count, g_audioInSlot1Count;
				extern volatile unsigned g_loopWetZcL, g_loopWetZcR;
				extern volatile unsigned long long g_loopWetEnergyL, g_loopWetEnergyR;
				extern volatile unsigned g_loopWetEnergyN;
				extern unsigned AudioOutputUSB_outAvail (void);
				extern unsigned AudioInputUSB_inAvail (void);
				// Mean-absolute-amplitude this window (ienL/ienR): ZCR proved blind to a
				// real user-confirmed buzz occurrence (lower ZCR during the buzz than
				// idle) -- this catches elevated noise energy of ANY spectral shape
				// (low-frequency hum included) since it sums |sample| regardless of
				// zero-crossing rate. Computed as a 32-bit average (energy/N fits
				// easily -- N is samples/window, energy is sum of 16-bit magnitudes)
				// so CString::Format needs no 64-bit specifier.
				unsigned enN = g_audioInEnergyN;
				unsigned ienL = enN ? (unsigned)(g_audioInEnergyL / enN) : 0;
				unsigned ienR = enN ? (unsigned)(g_audioInEnergyR / enN) : 0;
				// Same computation for the WET (post-loopMachine) tap point --
				// wzcL/wzcR/wenL/wenR. Both project WAV-writers capture from HERE, not
				// raw USB input, so comparing these against izcL/ienL above during a
				// live buzz occurrence is the direct test of whether the artifact is
				// introduced inside loopMachine's live-pitch/microrepeat/filter chain.
				unsigned wenN = g_loopWetEnergyN;
				unsigned wenL = wenN ? (unsigned)(g_loopWetEnergyL / wenN) : 0;
				unsigned wenR = wenN ? (unsigned)(g_loopWetEnergyR / wenN) : 0;
				// pktSize/pktsSubmitted: negotiated IN microframe-batching numbers
				// (StartInRequest), exposed to correlate a device's specific
				// bandwidth against any timing artifact -- e.g. the AIR192-only
				// "snore" burst-cadence investigation (see memory
				// mono-snore-glitch-uac2-specific). Plain free-function accessor
				// (not the class type) since this app-side TU doesn't include
				// usbaudiodevice.h, matching every other cross-TU telemetry
				// accessor in this codebase.
				extern unsigned short CUSBAudioDevice_GetInPktSize0 (void);
				extern unsigned CUSBAudioDevice_GetInPktsSubmitted0 (void);
				extern volatile unsigned g_audioOutZeroPkts;
				unsigned short inPktSize = CUSBAudioDevice_GetInPktSize0 ();
				unsigned inPktsSubmitted = CUSBAudioDevice_GetInPktsSubmitted0 ();
				CString s;
				s.Format("audioIn=%u audioOut=%u uac2=%u ch=%u bits=%u rate=%u inDeliv=%u inPeak=%u inFail=%u outDeliv=%u outPeak=%u outFail=%u inUR=%u inRS=%u outUR=%u otgRS=%u outRingRS=%u outWr=%u outAvail=%u inAvail=%u fbRate=%u fbCnt=%u outMaxGapUs=%u inMaxGapUs=%u izcL=%u izcR=%u ienL=%u ienR=%u ienN=%u slot0=%u slot1=%u wzcL=%u wzcR=%u wenL=%u wenR=%u wenN=%u inPktSize=%u inPkts=%u outZeroPkts=%u",
					g_audioInBound, g_audioOutBound, g_audioUAC2, g_audioChannels,
					g_audioSubslot*8, g_audioRate, g_audioInDeliv, g_audioInPeak, g_audioInSubmitFail,
					g_audioOutDeliv, g_audioOutPeak, g_audioOutSubmitFail,
					g_inUnderruns, g_inResyncs, g_outUnderruns, g_otgResyncs, g_outRingResync,
					g_outWrites, AudioOutputUSB_outAvail(), AudioInputUSB_inAvail(),
					g_audioFbRate, g_audioFbCount, g_audioOutMaxGapUs, g_audioInMaxGapUs,
					g_audioInZcL, g_audioInZcR, ienL, ienR, enN,
					g_audioInSlot0Count, g_audioInSlot1Count,
					g_loopWetZcL, g_loopWetZcR, wenL, wenR, wenN,
					(unsigned) inPktSize, inPktsSubmitted, g_audioOutZeroPkts);
				g_audioOutMaxGapUs = 0;   // reset so each probe shows the max since last read
				g_audioInMaxGapUs  = 0;   // reset so each probe shows the max since last read
				g_audioInZcL = 0;         // reset so each probe shows the ZCR since last read
				g_audioInZcR = 0;
				g_audioInEnergyL = 0;     // reset so each probe shows the energy since last read
				g_audioInEnergyR = 0;
				g_audioInEnergyN = 0;
				g_audioInSlot0Count = 0;  // reset so each probe shows the slot split since last read
				g_audioInSlot1Count = 0;
				g_loopWetZcL = 0;         // reset so each probe shows the WET-tap window since last read
				g_loopWetZcR = 0;
				g_loopWetEnergyL = 0;
				g_loopWetEnergyR = 0;
				g_loopWetEnergyN = 0;
				pDebug->SendTo((u8 *)(const char *)s, s.GetLength(), MSG_DONTWAIT, sender, port);
			}
			else if (buf[0]=='R' && buf[1]=='A' && buf[2]=='W' && buf[3]=='D')
			{
				// Real raw-input WAVEFORM capture -- not statistics. izcL/izcR
				// (zero-crossing-rate) and ienL/ienR (mean-absolute-amplitude,
				// UAUD verb above) both proved BLIND to a real, always-on,
				// user-confirmed buzz+distortion on this hardware: every scalar
				// sample this session showed flat, unremarkable values, even one
				// taken while the user confirmed the artifact was present at that
				// exact moment. A narrow spectral feature, intermodulation
				// product, or bit-level corruption can leave gross statistics
				// completely untouched -- this verb dumps the last
				// RAWD_SNAP_SAMPLES raw stereo samples (continuously overwritten
				// at the SAME tap point as izcL/ienL, in InCompletion, before
				// loopMachine/effects) as hex so the actual waveform can be
				// inspected directly (min/max/shape, or reconstructed to audio),
				// with no separate arm/trigger step needed since it's always
				// fresh. seq= lets the caller confirm two reads didn't race a
				// snapshot update mid-copy (compare seq before/after decoding).
				extern volatile s16 g_audioInSnapL[], g_audioInSnapR[];
				extern volatile unsigned g_audioInSnapSeq;
				unsigned seq0 = g_audioInSnapSeq;
				CString s; CString h;
				s.Format("rawd seq=%u n=%u L=", seq0, RAWD_SNAP_SAMPLES);
				for (unsigned i = 0; i < RAWD_SNAP_SAMPLES; i++)
				{
					h.Format("%04x", (u16) g_audioInSnapL[i]);
					s.Append(h);
				}
				s.Append(" R=");
				for (unsigned i = 0; i < RAWD_SNAP_SAMPLES; i++)
				{
					h.Format("%04x", (u16) g_audioInSnapR[i]);
					s.Append(h);
				}
				unsigned seq1 = g_audioInSnapSeq;
				CString tail; tail.Format(" seqEnd=%u", seq1);
				s.Append(tail);
				pDebug->SendTo((u8 *)(const char *)s, s.GetLength(), MSG_DONTWAIT, sender, port);
			}
			else if (buf[0]=='M' && buf[1]=='R' && buf[2]=='A' && buf[3]=='W')
			{
				// MONO-conversion diagnostic: dumps the post-sum USB-IN mono
				// waveform (input_usb.cpp::inHandler) and the post-effects
				// USB-OUT mono waveform (output_usb.cpp::update) side by side,
				// so a live capture can localize a reported artifact to the
				// USB-IN sum, the effects chain, or the USB-OUT duplication.
				// IN is a true rolling ring (multiple completions deep, see
				// input_usb.cpp) -- read starting from the oldest still-valid
				// slot so the hex dump comes out in real chronological order,
				// not wrapped array order.
				extern volatile s16 g_audioMonoSnap[];
				extern volatile unsigned g_audioMonoSnapSeq;
				extern unsigned AudioInputUSB_monoSnapWritePos (void);
				extern volatile s16 g_audioMonoOutSnap[];
				extern volatile unsigned g_audioMonoOutSnapSeq;
				unsigned seqIn0 = g_audioMonoSnapSeq;
				unsigned seqOut0 = g_audioMonoOutSnapSeq;
				unsigned inWr = AudioInputUSB_monoSnapWritePos ();
				CString s; CString h;
				s.Format("mraw seqIn=%u seqOut=%u IN=", seqIn0, seqOut0);
				for (unsigned i = 0; i < 128; i++)
				{
					unsigned idx = (inWr + i) % 128;   // oldest-first chronological order
					h.Format("%04x", (u16) g_audioMonoSnap[idx]);
					s.Append(h);
				}
				s.Append(" OUT=");
				for (unsigned i = 0; i < 64; i++)
				{
					h.Format("%04x", (u16) g_audioMonoOutSnap[i]);
					s.Append(h);
				}
				pDebug->SendTo((u8 *)(const char *)s, s.GetLength(), MSG_DONTWAIT, sender, port);
			}
			else if (buf[0]=='M' && buf[1]=='E' && buf[2]=='V' && buf[3]=='T')
			{
				// Read back the glitch-event log: a continuously-running
				// detector (input_usb.cpp inHandler) flags a discontinuity in
				// the post-mono-sum USB-IN stream and logs tick + absolute
				// sample index + value/prevValue -- cheap enough to poll
				// sparsely (unlike a raw capture) while waiting for one of the
				// user-confirmed seconds-scale glitch bursts to actually
				// happen. Dumps up to MEVT_LOG_SIZE=64 most recent events.
				// Flat parallel-array accessors (not a struct) -- avoids a
				// cross-TU struct-type mismatch since this app-side file and
				// input_usb.cpp (lib-side) don't share a header here; matches
				// this codebase's existing convention for cross-TU telemetry.
				extern unsigned AudioInputUSB_glitchLogWritePos (void);
				extern const u32      *AudioInputUSB_glitchLogTicks  (void);
				extern const unsigned *AudioInputUSB_glitchLogSample (void);
				extern const s16      *AudioInputUSB_glitchLogValue  (void);
				extern const s16      *AudioInputUSB_glitchLogPrev   (void);
				unsigned wr = AudioInputUSB_glitchLogWritePos ();
				const u32      *ticks = AudioInputUSB_glitchLogTicks ();
				const unsigned *samps = AudioInputUSB_glitchLogSample ();
				const s16      *vals  = AudioInputUSB_glitchLogValue ();
				const s16      *prevs = AudioInputUSB_glitchLogPrev ();
				const unsigned LOG_SIZE = 64;
				unsigned count = wr < LOG_SIZE ? wr : LOG_SIZE;
				unsigned start = wr < LOG_SIZE ? 0 : (wr - LOG_SIZE);
				CString s; CString h;
				s.Format("mevt wr=%u count=%u E=", wr, count);
				for (unsigned i = start; i < wr; i++)
				{
					unsigned slot = i % LOG_SIZE;
					h.Format("[t%u s%u v%04x p%04x]", ticks[slot], samps[slot], (u16) vals[slot], (u16) prevs[slot]);
					s.Append(h);
				}
				pDebug->SendTo((u8 *)(const char *)s, s.GetLength(), MSG_DONTWAIT, sender, port);
			}
			else if (buf[0]=='M' && buf[1]=='L' && buf[2]=='O' && buf[3]=='N' && buf[4]=='G')
			{
				// Arm the long triggered capture (8192 mono samples, ~170ms @
				// 48kHz -- spans several full periods of the reported ~50ms
				// snore, unlike MRAW's 128-sample/~2.7ms window). One-shot:
				// free-runs from here until full, then auto-disarms so MDUMP
				// reads a stable, non-overwritten buffer. Re-arm with another
				// MLONG before the next capture.
				extern void AudioInputUSB_armLongCapture (void);
				AudioInputUSB_armLongCapture ();
				CString s; s.Format("mlong armed");
				pDebug->SendTo((u8 *)(const char *)s, s.GetLength(), MSG_DONTWAIT, sender, port);
			}
			else if (buf[0]=='M' && buf[1]=='D' && buf[2]=='U' && buf[3]=='M' && buf[4]=='P')
			{
				// Read back a 256-sample chunk of the long capture. Request
				// body: "MDUMP<chunk>" where <chunk> is an ASCII decimal chunk
				// index (0..31 for 8192/256); malformed/missing chunk digits
				// default to chunk 0. Each chunk is ~1KB hex text -- a first
				// attempt at 512 samples/~2KB per chunk silently failed (every
				// MDUMP request timed out with no reply, while DIAG/MRAW on the
				// same UDP receive loop kept working normally), consistent with
				// exceeding this network path's safe UDP payload size; 256
				// samples matches RAWD's proven-working ~1KB response size.
				extern unsigned AudioInputUSB_longCaptureWritePos (void);
				extern bool AudioInputUSB_longCaptureArmed (void);
				extern const s16 *AudioInputUSB_longCaptureBuffer (void);
				const unsigned CHUNK_SAMPLES = 256;
				const unsigned TOTAL_SAMPLES = 8192;
				unsigned chunk = 0;
				if (n > 5)
				{
					chunk = 0;
					for (int k = 5; k < n && buf[k] >= '0' && buf[k] <= '9'; k++)
						chunk = chunk * 10 + (unsigned)(buf[k] - '0');
				}
				unsigned start = chunk * CHUNK_SAMPLES;
				unsigned wr = AudioInputUSB_longCaptureWritePos ();
				bool armed = AudioInputUSB_longCaptureArmed ();
				const s16 *cap = AudioInputUSB_longCaptureBuffer ();
				CString s; CString h;
				s.Format("mdump chunk=%u wr=%u armed=%u D=", chunk, wr, armed ? 1u : 0u);
				if (start < TOTAL_SAMPLES)
				{
					unsigned end = start + CHUNK_SAMPLES;
					if (end > TOTAL_SAMPLES) end = TOTAL_SAMPLES;
					for (unsigned i = start; i < end; i++)
					{
						h.Format("%04x", (u16) cap[i]);
						s.Append(h);
					}
				}
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
