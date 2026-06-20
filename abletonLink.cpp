// abletonLink.cpp — full Ableton Link peer over the raw-WiFi "ticker" path.
//
// Discovery (multicast _asdp_v 224.76.78.75:20808) + the unicast measurement
// protocol (_link_v PING/PONG) so the looper joins a real Link session and
// phase-syncs with Ableton Live and other Link apps. Wire codec = linkWire.h
// (bit-exact, host-tested), ghost-clock math = linkGhost.h, peer table +
// ownership = linkSession.h (all host-tested in scripts/test-link-*.cpp).
//
// Transport: raw SendFrame/ReceiveFrame (no IP stack on WLAN). Multicast uses the
// fixed LNK group MAC. Unicast (ping/pong) targets a peer's MAC learned from the
// Ethernet SOURCE of its frames — so AP mode needs NO ARP. The single RX demux
// (linkProcess) routes each frame once: multicast _asdp_v discovery, unicast
// _link_v measurement, DHCP :67/:68. TX is gated on IsLinkUp (un-associated
// SendFrame wedges the radio); reactive PONGs are post-RX so the radio is live.
//
// Ghost timeline: we measure each peer (ping burst -> median offset = GhostXForm),
// elect the session owner (highest NodeId), adopt the owner's timeline, and
// publish our local beat phase for loopMachine to align masterPhase to. When we
// own, our ghost == our host and we broadcast our own timeline.

#include "abletonLink.h"
#include "wlanDHCP.h"
#include "linkWire.h"
#include "linkGhost.h"
#include "linkSession.h"
#include <circle/timer.h>
#include <circle/util.h>

#define LINK_PORT		20808
#define OUR_MEP4_PORT	20808       // we accept unicast pings on our IP:this port
#define LCLK_PORT		20810       // esp "ticker" clock-broadcast (multicast, same group)
#define SEND_INTERVAL_US	1000000u
#define PING_INTERVAL_US	50000u  // 50ms between measurement pings (Link default)
#define MEASURE_RETRY_US	2000000u// re-measure a peer at least this often
#define FRAME_BUF		1600
#define IP_HDR_OFF		14
#define UDP_HDR_OFF		34
#define PAYLOAD_OFF		42

static const u8 MCAST[4]={224,76,78,75};
static const u8 MCAST_MAC[6]={0x01,0x00,0x5e,0x4c,0x4e,0x4b};
static u8 s_ownIP[4]={192,168,4,1};       // AP IP by default; tracks wlanDhcpIP()
static u8 s_ownMac[6]={0};
static CBcm4343Device *s_pWLAN=nullptr;
static double s_bpm=120.0;
static u64 s_nodeId=0, s_lastSend=0;
static bool     s_synced=false;
static unsigned s_lastIgmp=0;

static LinkSession   s_session;
static LinkGhostXForm s_ownXform = {0};   // host->session-ghost (identity when we own)

// Published (Core-2-local) shared-timeline view, read by apcKey25::update (also
// Core 2) into the paramSnapshot for loopMachine. Same-core, no tearing.
static volatile bool    s_phaseValid = false;
static s64  s_beatPhaseMicroBeats = 0;
static s64  s_quantumMicroBeats   = 4000000;  // 4 beats
// telemetry counters
static volatile unsigned s_pingsTx=0, s_pongsRx=0, s_pongsTx=0, s_peersTotal=0;
static volatile unsigned s_pingsRx=0;           // measurement PINGs received from peers
// Raw bytes of the last measurement (_link_v) frame a peer sent us — ground
// truth for the on-the-wire ping/pong format vs our codec (read via :4445 LMSG).
static u8  s_lastMeas[64];
static volatile int s_lastMeasLen = 0;
static volatile int s_lastMeasType = -1;
// Raw bytes of our last transmitted ALIVE and the last ALIVE we received from a
// peer — diff them (via :4445 TALV / RALV) to find why a real Link app (Live)
// accepts the peer's ALIVE but rejects ours (one-way discovery).
static u8  s_lastTxAlive[128];
static volatile int s_lastTxAliveLen = 0;
static u8  s_lastRxAlive[128];
static volatile int s_lastRxAliveLen = 0;
// Full received discovery frame (Ethernet+IP+UDP+payload) for header comparison.
static u8  s_lastRxFrame[180];
static volatile int s_lastRxFrameLen = 0;
static volatile s64      s_ownerOffsetUs=0;

// Timeline (train-on-first-loop). s_timeOrigin = CTimer microseconds at the
// (backdated) rec-press = beat 0; s_beatOrigin = 0 there. Broadcast in the tmln
// TLV so a peer's transport aligns to our first loop as song start.
static bool   s_started   = false;
static bool   s_ended     = false;
static u64    s_timeOrigin = 0;      // us
static s64    s_beatOrigin = 0;      // microbeats
static double s_quantBeats = 0.0;
// Tempo PROPOSAL hold (Link lets ANY peer set the group tempo). When the looper
// records its first loop it derives a tempo and PROPOSES it to the session: for a
// short window we broadcast OUR timeline and stop adopting a peer's, so the ticker
// (esp) / Live adopt our tempo before we revert to following. After the window the
// session tempo equals our proposed value and a later peer tempo change is followed
// normally -> symmetric, every device can set tempo. Written on the audio core
// (linkEnd), read on the control core (republishTimeline); same CTimer clock.
static volatile s64 s_proposeUntil = 0;   // us deadline; 0 = not proposing
#define TEMPO_PROPOSE_HOLD_US 4000000
static unsigned s_rxFrames = 0;   // total frames pulled off the radio RX queue (diag)
unsigned linkRxFrameCount(void) { return s_rxFrames; }
volatile unsigned g_uniRxToUs = 0;   // UDP unicast frames addressed to our IP (diag)
unsigned linkUniRxToUs(void) { return g_uniRxToUs; }

static inline u16 swap16(u16 v) { return __builtin_bswap16(v); }
static inline u32 swap32(u32 v) { return __builtin_bswap32(v); }
static inline u64 swap64(u64 v) { return __builtin_bswap64(v); }

static u16 csum16(const u8 *d, int n)
{
	u32 s = 0;
	for (int i = 0; i < n; i += 2) s += ((u16)d[i] << 8) | d[i+1];
	while (s >> 16) s = (s & 0xffff) + (s >> 16);
	return (u16)~s;
}

static inline s64 nowMicros(void) { return (s64)CTimer::GetClockTicks(); }

// Build NodeId bytes (big-endian) from the u64 we seeded at init.
static inline void ownNodeIdBytes(u8 out[8]) { for (int i=0;i<8;i++) out[i]=(u8)(s_nodeId>>(56-8*i)); }

// Our current timeline as a LinkTimeline (microbeats). Tempo from s_bpm; origin
// from the trained first-loop press (or 0 before training).
static LinkTimeline ownTimeline(void)
{
	LinkTimeline tl;
	tl.tempoMicrosPerBeat   = lgBpmToMicrosPerBeat(s_bpm > 0 ? s_bpm : 120.0);
	tl.beatOriginMicroBeats = s_beatOrigin;
	tl.timeOriginMicros     = s_started ? (s64)s_timeOrigin : 0;
	return tl;
}

// ---- frame TX: wrap a UDP payload in an ETH+IP+UDP frame ----
// dstMac/dstIp NULL => multicast (LNK group). Otherwise unicast to the peer.
static void sendUDP(const u8 *dstMac, const u8 *dstIp, u16 dstPort, const u8 *payload, int plen)
{
	if (!s_pWLAN || plen < 0 || plen > (FRAME_BUF - PAYLOAD_OFF)) return;
	u8 frame[FRAME_BUF];
	memset(frame, 0, PAYLOAD_OFF + plen);

	const u8 *dmac = dstMac ? dstMac : MCAST_MAC;
	const u8 *dip  = dstIp  ? dstIp  : MCAST;
	memcpy(frame+0, dmac, 6); memcpy(frame+6, s_ownMac, 6);
	frame[12]=0x08; frame[13]=0x00;

	u8 *ip = frame + IP_HDR_OFF;
	// TTL=64 + flags=0 to match real Ableton Live frames (captured via :4445 RFRM:
	// Live uses TTL 64, no DF). Our previous TTL=1 multicast was not being relayed
	// by the WiFi AP to the other station where Live runs — so Live never saw our
	// ALIVE (one-way discovery). ip[6]=0 (no DF), ip[8]=64 (TTL).
	ip[0]=0x45; ip[6]=0x00; ip[8]=64; ip[9]=17;
	u16 ipLen=swap16(20+8+plen); memcpy(ip+2,&ipLen,2);
	if (wlanDhcpOK()) memcpy(s_ownIP, wlanDhcpIP(), 4);
	memcpy(ip+12, s_ownIP, 4); memcpy(ip+16, dip, 4);
	u16 cs=swap16(csum16(ip,20)); memcpy(ip+10,&cs,2);

	u8 *udp=frame+UDP_HDR_OFF;
	u16 sp=swap16(LINK_PORT), dp=swap16(dstPort), uLen=swap16(8+plen);
	memcpy(udp,&sp,2); memcpy(udp+2,&dp,2); memcpy(udp+4,&uLen,2);
	memcpy(frame + PAYLOAD_OFF, payload, plen);

	// UDP checksum (pseudo-header + UDP segment). Was left 0 — legal for IPv4 but
	// real Link senders compute it, and a checksum-0 datagram can be dropped by a
	// strict receiver / WiFi multicast path, which made Live never see our ALIVE.
	// The checksum field (udp+6..7) is 0 during the sum, then filled; a 0 result
	// is transmitted as 0xffff per RFC768.
	{
		int seglen = 8 + plen;
		u32 s = 0;
		s += (ip[12]<<8)|ip[13]; s += (ip[14]<<8)|ip[15];   // src IP
		s += (ip[16]<<8)|ip[17]; s += (ip[18]<<8)|ip[19];   // dst IP
		s += 17; s += (u32)seglen;                          // proto + UDP length
		for (int i = 0; i + 1 < seglen; i += 2) s += (udp[i]<<8)|udp[i+1];
		if (seglen & 1) s += (udp[seglen-1]<<8);
		while (s >> 16) s = (s & 0xffff) + (s >> 16);
		u16 uc = (u16)~s; if (uc == 0) uc = 0xffff;
		udp[6] = (u8)(uc >> 8); udp[7] = (u8)(uc & 0xff);
	}

	s_pWLAN->SendFrame(frame, PAYLOAD_OFF + plen);
}

// ---- discovery TX ----
static void sendDiscovery(u8 msgType)
{
	u8 nid[8]; ownNodeIdBytes(nid);
	u8 sess[8]; for (int i=0;i<8;i++) sess[i]=nid[i];   // session id seed = our nodeId
	LinkTimeline tl = ownTimeline();
	if (wlanDhcpOK()) memcpy(s_ownIP, wlanDhcpIP(), 4);
	u8 pl[256];
	int n = lwEncodeAlive(pl, msgType, /*ttl*/5, /*groupId*/0, nid, sess,
	                      tl.tempoMicrosPerBeat, tl.beatOriginMicroBeats, tl.timeOriginMicros,
	                      s_ownIP, OUR_MEP4_PORT);
	if (msgType == LW_MSG_ALIVE) {
		int c = n < (int)sizeof s_lastTxAlive ? n : (int)sizeof s_lastTxAlive;
		memcpy(s_lastTxAlive, pl, c); s_lastTxAliveLen = c;
	}
	sendUDP(0, 0, LINK_PORT, pl, n);   // multicast
}

static void sendByeBye(void)
{
	u8 nid[8]; ownNodeIdBytes(nid);
	u8 pl[32];
	int n = lwEncodeByeBye(pl, 5, 0, nid);
	sendUDP(0, 0, LINK_PORT, pl, n);
}

// ---- measurement: send a PING to a peer's mep4 endpoint ----
static void sendPing(LinkPeer *p)
{
	if (!p->hasEndpoint) return;
	u8 nid[8]; ownNodeIdBytes(nid);
	u8 pl[64];
	int n = lwEncodePing(pl, 5, 0, nid, nowMicros());
	sendUDP(p->mac, p->ipv4, p->mep4Port, pl, n);
	s_pingsTx++;
}

// ---- measurement: respond to a PING with a PONG (our ghost) ----
static void sendPong(LinkPeer *p, s64 echoedHost)
{
	u8 nid[8]; ownNodeIdBytes(nid);
	u8 sess[8]; for (int i=0;i<8;i++) sess[i]=nid[i];
	s64 ghost = lgHostToGhost(s_ownXform, nowMicros());
	s64 prevGhost = p->prevGhostSent;
	p->prevGhostSent = ghost;
	u8 pl[96];
	int n = lwEncodePong(pl, 5, 0, nid, sess, ghost, prevGhost, echoedHost);
	sendUDP(p->mac, p->ipv4, p->mep4Port, pl, n);
	s_pongsTx++;
}

// ---- recompute the published shared timeline (owner election + adoption) ----
static void republishTimeline(void)
{
	LinkPeer *owner = lsOwnerPeer(&s_session);
	LinkTimeline tl;
	bool phaseTrusted = false;
	bool proposing = (s_proposeUntil != 0 && nowMicros() < s_proposeUntil);
	if (proposing)
	{
		// We just recorded a loop and are PROPOSING its tempo to the group. Broadcast
		// OUR timeline; do NOT adopt a peer's during the hold so the proposal is not
		// reverted before the ticker/Live pick it up. ownTimeline() carries s_bpm (the
		// loop tempo from linkEnd) + our origin, exactly what a peer's tmln adopt reads.
		s_ownXform.offsetMicros = 0;
		tl = ownTimeline();
		s_bpm = lgMicrosPerBeatToBpm(tl.tempoMicrosPerBeat);
		s_ownerOffsetUs = 0;
		s_synced = (lsPeerCount(&s_session) > 0);
		phaseTrusted = s_started;   // our own loop defines phase
	}
	else if (owner && owner->measured)
	{
		// Follow the owner: our ghost xform = the offset we measured to it; adopt
		// its timeline; tempo = owner tempo. Measured => beat phase is trustworthy.
		s_ownXform = owner->xform;
		tl = owner->timeline;
		s_bpm = lgMicrosPerBeatToBpm(tl.tempoMicrosPerBeat);
		s_synced = true;
		s_ownerOffsetUs = owner->xform.offsetMicros;
		phaseTrusted = true;
	}
	else if (owner && owner->hasTimeline)
	{
		// TEMPO-ONLY sync: the owner advertises a timeline but never completed the
		// ping/pong measurement (e.g. an esp32 "ticker" that broadcasts Link but
		// runs no PingResponder). Tempo needs NO clock offset, so adopt the owner's
		// BPM and mark synced -- the looper quantizes to ticker's tempo. But the
		// beat PHASE depends on the (unknown) ghost offset, so do NOT trust it:
		// leave the ghost xform at identity and phaseValid false so loopMachine's
		// phase-align step is skipped (it would otherwise lock the first loop's
		// downbeat to a bogus phase). Better a correct tempo with free phase than a
		// wrong phase.
		s_ownXform.offsetMicros = 0;
		tl = owner->timeline;
		s_bpm = lgMicrosPerBeatToBpm(tl.tempoMicrosPerBeat);
		s_synced = true;
		s_ownerOffsetUs = 0;
		phaseTrusted = false;
	}
	else
	{
		// We own (or no owner yet): ghost == host, our own timeline.
		s_ownXform.offsetMicros = 0;
		tl = ownTimeline();
		s_ownerOffsetUs = 0;
		// s_synced stays as discovery set it (a peer present but unmeasured still
		// counts as "seen"); cleared when no peers.
		if (lsPeerCount(&s_session) == 0) s_synced = false;
	}
	// Publish our beat phase at 'now' for loopMachine (Core-2-local statics).
	s_quantumMicroBeats   = (s_quantBeats > 0 ? (s64)(s_quantBeats*1e6) : 4000000);
	s_beatPhaseMicroBeats = lgBeatPhaseAtHost(s_ownXform, tl, nowMicros(), s_quantumMicroBeats);
	// Phase is valid only when we measured the owner (trusted offset) or we are the
	// session origin ourselves; a tempo-only adoption does NOT validate phase.
	s_phaseValid          = phaseTrusted || s_started;
	s_peersTotal          = (unsigned)lsPeerCount(&s_session);
}

// ---- esp "ticker" clock broadcast (multicast, bypasses the unicast wall) ----
// The bcm4343 in this setup delivers multicast/broadcast but NOT unicast-to-self
// (witnessed: :4445 WLAN uniRx stays 0 while rxFrames climbs), so the standard
// Link unicast ping/pong measurement can never complete and phase never locks.
// The esp ticker therefore ALSO multicasts its current clock micros ("LCLK" +
// i64 LE host-micros) to the Link group on LCLK_PORT. We treat that clock as the
// owner peer's measured ghost offset (offset = espNow - ourNow; one-way WiFi
// latency ~1-3ms is <1% of a beat, fine for phrase sync), which lets the existing
// owner-election / timeline-adoption / lgBeatPhaseAtHost machinery validate phase
// exactly as a real measurement would. Payload: "LCLK"(4) + i64 espNowMicros LE.
static volatile unsigned g_clkRx = 0;          // diag: clock broadcasts consumed
static void handleClockBroadcast(const u8 *pl, int plen)
{
	if (plen < 12 || memcmp(pl, "LCLK", 4) != 0) return;
	s64 espNow; memcpy(&espNow, pl + 4, 8);    // both ends little-endian
	g_clkRx++;
	LinkPeer *owner = lsOwnerPeer(&s_session);
	if (!owner || !owner->hasTimeline) return;  // need the owner's timeline to map
	owner->xform.offsetMicros = espNow - nowMicros();
	owner->measured = true;                     // trusted offset -> phase becomes valid
	// Throttle the republish so a misbehaving/high-rate broadcaster can never
	// starve the Core-2 control plane: the offset above is updated every packet
	// (cheap), but the timeline recompute runs at most ~50 Hz; the periodic
	// republish at the end of linkProcess also refreshes between these.
	static s64 s_lastRepub = 0;
	s64 now = nowMicros();
	if (now - s_lastRepub < 20000) return;
	s_lastRepub = now;
	republishTimeline();
}
unsigned linkClkRx(void) { return g_clkRx; }

// ---- decode an inbound Link message (discovery or measurement) ----
static void handleMessage(const u8 *pl, int plen, const u8 *ethSrcMac)
{
	LwMessage m;
	if (!lwDecode(pl, plen, &m)) return;
	// Ignore our own packets (we host the AP and see our own multicast).
	u8 nid[8]; ownNodeIdBytes(nid);
	if (memcmp(m.nodeId, nid, 8) == 0) return;

	s64 now = nowMicros();

	if (!m.isLink)
	{
		// Capture a received ALIVE (ground-truth valid discovery frame) to diff
		// against our own transmitted ALIVE (s_lastTxAlive).
		if (m.msgType == LW_MSG_ALIVE) {
			int c = plen < (int)sizeof s_lastRxAlive ? plen : (int)sizeof s_lastRxAlive;
			memcpy(s_lastRxAlive, pl, c); s_lastRxAliveLen = c;
		}
		// Discovery: ALIVE / RESPONSE / BYEBYE.
		if (m.msgType == LW_MSG_BYEBYE)
		{
			LinkPeer *p = lsFind(&s_session, m.nodeId);
			if (p) p->used = false;
			republishTimeline();
			return;
		}
		LinkPeer *p = lsUpsert(&s_session, m.nodeId, now);
		if (!p) return;
		memcpy(p->mac, ethSrcMac, 6);
		if (m.hasMep4) { memcpy(p->ipv4, m.mep4Addr, 4); p->mep4Port = m.mep4Port; p->hasEndpoint = true; }
		if (m.hasTimeline) {
			p->timeline.tempoMicrosPerBeat   = m.mpb;
			p->timeline.beatOriginMicroBeats  = m.beatOrigin;
			p->timeline.timeOriginMicros      = m.timeOrigin;
			p->hasTimeline = true;
		}
		// A new ALIVE => reply RESPONSE so the peer learns us promptly.
		if (m.msgType == LW_MSG_ALIVE) sendDiscovery(LW_MSG_RESPONSE);
		republishTimeline();
		return;
	}

	// Measurement: PING (we respond) / PONG (we accumulate a sample).
	// Capture the raw frame (ground truth for the wire format) regardless of how
	// our codec decoded it.
	{
		int n = plen < (int)sizeof s_lastMeas ? plen : (int)sizeof s_lastMeas;
		memcpy(s_lastMeas, pl, n); s_lastMeasLen = n; s_lastMeasType = m.msgType;
	}
	LinkPeer *p = lsUpsert(&s_session, m.nodeId, now);
	if (!p) return;
	memcpy(p->mac, ethSrcMac, 6);    // learn/refresh MAC for unicast replies

	if (m.msgType == LW_MSG_PING && m.hasHost)
	{
		s_pingsRx++;
		sendPong(p, m.hostTime);
		return;
	}
	if (m.msgType == LW_MSG_PONG && m.hasHost && m.hasGhost)
	{
		s_pongsRx++;
		// sample operands: host=echoed send (m.hostTime), prevHost=recv now,
		// ghost=m.ghostTime, prevGhost=m.prevGhostTime (0 on first).
		s64 out[2];
		int k = lwMeasurementSamples(m.hostTime, now, m.ghostTime,
		                             m.hasPrevGhost ? m.prevGhostTime : 0, out);
		lgMeasAddSamples(&p->meas, out, k);
		if (lgMeasHasEnough(&p->meas) || lgMeasExhausted(&p->meas))
		{
			if (lgMeasHasEnough(&p->meas))
			{
				p->xform = lgMeasResult(&p->meas);
				p->measured = true;
				republishTimeline();
			}
			else
			{
				// failed burst: keep last good xform; reset to retry later.
				lgMeasReset(&p->meas);
			}
		}
		return;
	}
}

// ---- RX classify: ONE frame -> Link multicast / Link unicast / (caller: DHCP) ----
// Returns true iff this frame was a Link frame we consumed.
static bool linkTryParse(const u8 *buf, unsigned len)
{
	if ((int)len < 42) return false;
	if (buf[12] != 0x08 || buf[13] != 0x00) return false;        // not IPv4
	const u8 *ip = buf+IP_HDR_OFF; int ihl=(ip[0]&0x0f)*4;
	if (ip[9] != 17) return false;                                // not UDP
	const u8 *udp = ip + ihl;
	u16 dpRaw; memcpy(&dpRaw, udp+2, 2); u16 dport = swap16(dpRaw);
	const u8 *pl = udp+8; int plen=(int)(len-(pl-buf));

	// esp ticker clock broadcast: multicast (or broadcast) to the Link group on
	// LCLK_PORT. Consume it directly (it carries phase, not a Link message). This
	// MUST run BEFORE the LW_HEADER_LEN (20) guard below: the LCLK payload is only
	// 12 bytes ("LCLK"+i64), so the guard would otherwise drop it and clkRx never
	// climbs (the bug that left phase tempo-only despite the esp sending fine).
	bool toClk = (memcmp(ip + 16, MCAST, 4) == 0 || ip[16] == 255) && dport == LCLK_PORT;
	if (toClk) { handleClockBroadcast(pl, plen); return true; }

	if (plen < (int)LW_HEADER_LEN) return false;

	// Diagnostic: count ANY UDP unicast addressed to our IP (any port). If this
	// stays 0 while discovery multicast climbs, the radio is not delivering
	// unicast-to-self frames -> Link MEASUREMENT (unicast ping/pong) can never
	// arrive, which is the phase-sync wall.
	extern volatile unsigned g_uniRxToUs;
	if (memcmp(ip + 16, s_ownIP, 4) == 0) g_uniRxToUs++;

	bool toMcast   = (memcmp(ip + 16, MCAST, 4) == 0) && dport == LINK_PORT;
	bool toUsUnicast = (memcmp(ip + 16, s_ownIP, 4) == 0) && dport == OUR_MEP4_PORT;
	if (!toMcast && !toUsUnicast) return false;

	// Disambiguate by protocol header: _asdp_v (discovery) vs _link_v (measurement).
	bool isAsdp = (memcmp(pl, LW_HDR_ASDP, 8) == 0);
	bool isLink = (memcmp(pl, LW_HDR_LINK, 8) == 0);
	if (!isAsdp && !isLink) return toMcast;   // junk on the mcast port: consume, ignore

	// Capture the FULL received frame (Eth+IP+UDP+payload) so we can compare a
	// real Live frame's IP/UDP headers (TTL, checksum, flags) to ours (:4445 RFRM).
	if (isAsdp) {
		int c = (int)len < (int)sizeof s_lastRxFrame ? (int)len : (int)sizeof s_lastRxFrame;
		memcpy(s_lastRxFrame, buf, c); s_lastRxFrameLen = c;
	}

	handleMessage(pl, plen, buf + 6 /* ethernet source MAC */);
	return true;
}

static void sendIgmpJoin(void)
{
	u8 f[60];
	memset(f, 0, sizeof f);
	memcpy(f, MCAST_MAC, 6);
	memcpy(f + 6, s_ownMac, 6);
	f[12] = 0x08; f[13] = 0x00;
	u8 *ip = f + 14;
	ip[0] = 0x46; ip[1] = 0xc0;
	u16 tot = swap16(32); memcpy(ip + 2, &tot, 2);
	ip[6] = 0x40; ip[8] = 1; ip[9] = 2;
	memcpy(ip + 12, wlanDhcpIP(), 4);
	memcpy(ip + 16, MCAST, 4);
	ip[20] = 0x94; ip[21] = 0x04;
	u16 cs = swap16(csum16(ip, 24)); memcpy(ip + 10, &cs, 2);
	u8 *igmp = ip + 24;
	igmp[0] = 0x16;
	memcpy(igmp + 4, MCAST, 4);
	u16 ics = swap16(csum16(igmp, 8)); memcpy(igmp + 2, &ics, 2);
	s_pWLAN->SendFrame(f, 14 + 32);
}

void linkInit(CBcm4343Device *pWLAN)
{
	s_pWLAN    = pWLAN;
	s_nodeId   = (u64)CTimer::GetClockTicks();
	s_bpm      = 120.0;
	s_lastSend = 0;
	s_synced   = false;
	if (s_pWLAN) s_pWLAN->GetMACAddress()->CopyTo(s_ownMac);
	if (wlanDhcpOK()) memcpy(s_ownIP, wlanDhcpIP(), 4);
	u8 self[8]; ownNodeIdBytes(self);
	lsInit(&s_session, self);
	s_ownXform.offsetMicros = 0;
}

// Drive the per-peer measurement scheduler: ping each endpoint-known peer that is
// due (not enough samples yet, or stale measurement to refresh). Bounded work.
static void driveMeasurement(s64 now)
{
	for (int i = 0; i < LS_MAX_PEERS; i++)
	{
		LinkPeer *p = &s_session.peers[i];
		if (!p->used || !p->hasEndpoint) continue;
		// Periodically re-measure an already-measured peer (drift refresh).
		if (p->measured && (now - p->meas.lastPingMicros) > (s64)MEASURE_RETRY_US)
			lgMeasReset(&p->meas);
		if (lgMeasHasEnough(&p->meas)) continue;          // this burst is done
		// A burst that ran its 5 pings without enough pongs (peer slow/lossy) is
		// RESET after the ping interval so we keep retrying — otherwise pings
		// stays pinned at LG_NUM_MEASUREMENTS and the peer is never measured.
		if (lgMeasExhausted(&p->meas))
		{
			if ((now - p->meas.lastPingMicros) >= (s64)PING_INTERVAL_US) lgMeasReset(&p->meas);
			else continue;
		}
		if ((now - p->meas.lastPingMicros) >= (s64)PING_INTERVAL_US)
		{
			sendPing(p);
			p->meas.lastPingMicros = now;
			p->meas.pings++;
		}
	}
}

// ARP responder (load-bearing for Link MEASUREMENT over our hosted AP). We run
// no IP stack on WLAN, so we never answered ARP for our AP IP (192.168.4.1). A
// peer that joins the hosted "ticker" AP discovers us fine (discovery is
// MULTICAST) and we adopt its tempo, but the ping/pong MEASUREMENT is UNICAST:
// the peer must resolve 192.168.4.1's MAC via ARP to send us pings (and to send
// pongs back to our pings). With no ARP reply the peer can't unicast to us at all
// -> pingsRx=0, pongsRx=0, phase never measured. Answering ARP for our IP closes
// that loop. Reactive (post-RX, radio demonstrably live), so SendFrame is safe.
static bool arpMaybeReply(const u8 *f, int len)
{
	if (len < 42) return false;
	if (f[12] != 0x08 || f[13] != 0x06) return false;          // not ARP
	const u8 *a = f + 14;
	if (a[0]!=0x00 || a[1]!=0x01 || a[2]!=0x08 || a[3]!=0x00) return false; // not Eth/IPv4
	if (a[6]!=6 || a[7]!=4) return false;
	if (a[8]!=0x00 || a[9]!=0x01) return false;                // not a request
	const u8 *tpa = a + 24;                                     // target protocol addr
	if (memcmp(tpa, s_ownIP, 4) != 0) return false;            // not for our IP
	if (s_ownIP[0]==0 && s_ownIP[1]==0 && s_ownIP[2]==0 && s_ownIP[3]==0) return false;

	const u8 *sha = a + 8;   // sender hw addr
	const u8 *spa = a + 18;  // sender protocol addr
	u8 r[42];
	memcpy(r+0, sha, 6);            // dst MAC = requester
	memcpy(r+6, s_ownMac, 6);       // src MAC = us
	r[12]=0x08; r[13]=0x06;
	u8 *ra = r + 14;
	ra[0]=0x00; ra[1]=0x01; ra[2]=0x08; ra[3]=0x00; ra[4]=6; ra[5]=4;
	ra[6]=0x00; ra[7]=0x02;         // reply
	memcpy(ra+8,  s_ownMac, 6);     // sender hw = us
	memcpy(ra+14, s_ownIP, 4);      // sender proto = us
	memcpy(ra+18, sha, 6);          // target hw = requester
	memcpy(ra+24, spa, 4);          // target proto = requester
	s_pWLAN->SendFrame(r, sizeof r);
	return true;
}

void linkProcess(void)
{
	if (!s_pWLAN) return;

	// SINGLE RX DEMUX: one drain, routed by port. Link multicast discovery +
	// Link unicast measurement (to our IP:mep4 port) -> linkTryParse; DHCP :67 ->
	// AP server; DHCP :68 -> station client; ARP -> arpMaybeReply (so a peer can
	// resolve our AP IP and unicast measurement frames). Draining is always safe;
	// the per-tick cap keeps Core 2's control loop from being starved.
	u8 buf[FRAME_BUF];
	unsigned len;
	int budget = 64;
	while (budget-- > 0 && s_pWLAN->ReceiveFrame(buf, &len))
	{
		s_rxFrames++;                                    // total frames off the radio (diag)
		if (arpMaybeReply(buf, (int)len)) continue;      // ARP for our IP -> reply our MAC
		if (linkTryParse(buf, len)) continue;            // Link mcast :20808 + unicast measurement
		if (wlanDhcpServeFrame(buf, (int)len)) continue; // AP DHCP server :67
		if (wlanDhcpClientFrame(buf, (int)len)) continue;// station DHCP client :68
	}

	// TX gated on association (un-associated SendFrame wedges the radio). Reactive
	// PONGs above already fired post-RX (radio live). Below = proactive beacons +
	// pings + IGMP + periodic timeline republish.
	if (!s_pWLAN->IsLinkUp()) return;

	s64 now = nowMicros();
	lsExpire(&s_session, now);

	if ((u64)now - s_lastSend >= SEND_INTERVAL_US)
	{
		sendDiscovery(LW_MSG_ALIVE);
		s_lastSend = (u64)now;
		republishTimeline();   // refresh phase even with no traffic
	}
	driveMeasurement(now);
	if (wlanDhcpOK() && (unsigned)now - s_lastIgmp >= 30 * CLOCKHZ)
	{
		sendIgmpJoin();
		s_lastIgmp = (unsigned)now;
	}
}

double linkGetBPM(void)       { return s_bpm; }
void   linkSetBPM(double bpm) { s_bpm = bpm; }
bool   linkIsSynced(void)     { return s_synced; }

bool   linkHasStarted(void)   { return s_started; }
bool   linkHasEnded(void)     { return s_ended; }
double linkQuantBeats(void)   { return s_quantBeats; }

// Shared ghost beat phase for loopMachine (read on Core 2 by apcKey25::update,
// then published into the paramSnapshot). Returns false when no valid session
// phase yet. phaseMicroBeats in [0, quantumMicroBeats).
bool linkGhostPhase(s64 *phaseMicroBeats, s64 *quantumMicroBeats)
{
	if (!s_phaseValid) return false;
	if (phaseMicroBeats)   *phaseMicroBeats   = s_beatPhaseMicroBeats;
	if (quantumMicroBeats) *quantumMicroBeats = s_quantumMicroBeats;
	return true;
}

// Telemetry snapshot for the :4445 LINK verb.
void linkTelemetry(unsigned *peers, s64 *offsetUs, unsigned *pingsTx,
                   unsigned *pongsRx, int *selfOwns)
{
	if (peers)    *peers    = s_peersTotal;
	if (offsetUs) *offsetUs = s_ownerOffsetUs;
	if (pingsTx)  *pingsTx  = s_pingsTx;
	if (pongsRx)  *pongsRx  = s_pongsRx;
	if (selfOwns) *selfOwns = lsSelfOwns(&s_session) ? 1 : 0;
}

// Inbound measurement counters: PINGs received from peers (Live measuring us) and
// PONGs we sent back. pingsRx>0 proves peer->us reachability + that our ALIVE was
// accepted; pingsRx==0 means the peer never measures us.
void linkMeasCounters(unsigned *pingsRx, unsigned *pongsTx)
{
	if (pingsRx) *pingsRx = s_pingsRx;
	if (pongsTx) *pongsTx = s_pongsTx;
}

// Hex of the last measurement frame a peer sent us (ground truth wire format).
// Returns the byte count; writes up to max bytes of the raw frame into out and
// the message type into *type.
int linkLastMeas(u8 *out, int max, int *type)
{
	int n = s_lastMeasLen < max ? s_lastMeasLen : max;
	for (int i = 0; i < n; i++) out[i] = s_lastMeas[i];
	if (type) *type = s_lastMeasType;
	return n;
}

int linkLastTxAlive(u8 *out, int max)
{
	int n = s_lastTxAliveLen < max ? s_lastTxAliveLen : max;
	for (int i = 0; i < n; i++) out[i] = s_lastTxAlive[i];
	return n;
}
int linkLastRxAlive(u8 *out, int max)
{
	int n = s_lastRxAliveLen < max ? s_lastRxAliveLen : max;
	for (int i = 0; i < n; i++) out[i] = s_lastRxAlive[i];
	return n;
}
int linkLastRxFrame(u8 *out, int max)
{
	int n = s_lastRxFrameLen < max ? s_lastRxFrameLen : max;
	for (int i = 0; i < n; i++) out[i] = s_lastRxFrame[i];
	return n;
}

// Network diagnostics for the :4445 LINK verb: our source IP (what our pings are
// sourced from + what we advertise in mep4 — if this is the default 192.168.4.1
// in station mode we never got a DHCP lease on "ticker", which makes the peer's
// PONG unroutable back to us), our DHCP-lease state, and the first peer's parsed
// mep4 endpoint (confirms we decoded where to ping it).
void linkNetTelemetry(u8 ownIp[4], int *dhcp, u8 peerIp[4], int *peerPort, int *peerHasEp)
{
	if (ownIp) memcpy(ownIp, s_ownIP, 4);
	if (dhcp)  *dhcp = wlanDhcpOK() ? 1 : 0;
	for (int i = 0; i < LS_MAX_PEERS; i++)
	{
		if (!s_session.peers[i].used) continue;
		if (peerIp)    memcpy(peerIp, s_session.peers[i].ipv4, 4);
		if (peerPort)  *peerPort  = s_session.peers[i].mep4Port;
		if (peerHasEp) *peerHasEp = s_session.peers[i].hasEndpoint ? 1 : 0;
		return;
	}
	if (peerIp) { peerIp[0]=peerIp[1]=peerIp[2]=peerIp[3]=0; }
	if (peerPort)  *peerPort = 0;
	if (peerHasEp) *peerHasEp = 0;
}

void linkStart(unsigned originTicks)
{
	s_timeOrigin = originTicks ? (u64)originTicks : (u64)CTimer::GetClockTicks();
	s_beatOrigin = 0;
	s_started    = true;
	s_ended      = false;
	s_quantBeats = 0.0;
}

void linkDeriveQuant(double clip_seconds, double *out_beats, double *out_bpm)
{
	static const double cand[] = {0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0};
	const int N = (int)(sizeof cand / sizeof cand[0]);
	if (clip_seconds <= 0.0001) { *out_beats = 4.0; *out_bpm = 120.0; return; }

	double bestB = 4.0, bestBpm = 120.0, bestDist = 1e18; bool haveInWin = false;
	for (int i = 0; i < N; i++)
	{
		double bpm = 60.0 * cand[i] / clip_seconds;
		double dist = bpm > 120.0 ? bpm - 120.0 : 120.0 - bpm;
		bool inWin = (bpm >= 80.0 && bpm <= 160.0);
		if (inWin)
		{
			if (!haveInWin || dist < bestDist) { bestB = cand[i]; bestBpm = bpm; bestDist = dist; haveInWin = true; }
		}
		else if (!haveInWin && dist < bestDist)
		{
			bestB = cand[i]; bestBpm = bpm; bestDist = dist;
		}
	}
	*out_beats = bestB;
	*out_bpm   = bestBpm;
}

double linkEnd(double clip_seconds)
{
	double beats, bpm;
	linkDeriveQuant(clip_seconds, &beats, &bpm);
	s_bpm        = bpm;
	s_quantBeats = beats;
	s_ended      = true;
	// Propose this loop's tempo to the Link session (any peer may set the group
	// tempo). republishTimeline broadcasts OUR timeline for the hold window so the
	// ticker/Live adopt it; then we resume following the (now-matching) session.
	s_proposeUntil = nowMicros() + TEMPO_PROPOSE_HOLD_US;
	return beats;
}

void linkShutdown(void)
{
	if (s_pWLAN && s_pWLAN->IsLinkUp()) sendByeBye();
}
