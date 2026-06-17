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
static volatile s64      s_ownerOffsetUs=0;

// Timeline (train-on-first-loop). s_timeOrigin = CTimer microseconds at the
// (backdated) rec-press = beat 0; s_beatOrigin = 0 there. Broadcast in the tmln
// TLV so a peer's transport aligns to our first loop as song start.
static bool   s_started   = false;
static bool   s_ended     = false;
static u64    s_timeOrigin = 0;      // us
static s64    s_beatOrigin = 0;      // microbeats
static double s_quantBeats = 0.0;

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
	ip[0]=0x45; ip[6]=0x40; ip[8]=1; ip[9]=17;
	u16 ipLen=swap16(20+8+plen); memcpy(ip+2,&ipLen,2);
	if (wlanDhcpOK()) memcpy(s_ownIP, wlanDhcpIP(), 4);
	memcpy(ip+12, s_ownIP, 4); memcpy(ip+16, dip, 4);
	u16 cs=swap16(csum16(ip,20)); memcpy(ip+10,&cs,2);

	u8 *udp=frame+UDP_HDR_OFF;
	u16 sp=swap16(LINK_PORT), dp=swap16(dstPort), uLen=swap16(8+plen);
	memcpy(udp,&sp,2); memcpy(udp+2,&dp,2); memcpy(udp+4,&uLen,2);
	// UDP checksum optional for IPv4; leave 0 (Link tolerates it).

	memcpy(frame + PAYLOAD_OFF, payload, plen);
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
	if (owner && owner->measured)
	{
		// Follow the owner: our ghost xform = the offset we measured to it; adopt
		// its timeline; tempo = owner tempo.
		s_ownXform = owner->xform;
		tl = owner->timeline;
		s_bpm = lgMicrosPerBeatToBpm(tl.tempoMicrosPerBeat);
		s_synced = true;
		s_ownerOffsetUs = owner->xform.offsetMicros;
	}
	else
	{
		// We own (or no measured owner yet): ghost == host, our own timeline.
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
	s_phaseValid          = (owner != 0) || s_started;
	s_peersTotal          = (unsigned)lsPeerCount(&s_session);
}

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
	LinkPeer *p = lsUpsert(&s_session, m.nodeId, now);
	if (!p) return;
	memcpy(p->mac, ethSrcMac, 6);    // learn/refresh MAC for unicast replies

	if (m.msgType == LW_MSG_PING && m.hasHost)
	{
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
	if (plen < (int)LW_HEADER_LEN) return false;

	bool toMcast   = (memcmp(ip + 16, MCAST, 4) == 0) && dport == LINK_PORT;
	bool toUsUnicast = (memcmp(ip + 16, s_ownIP, 4) == 0) && dport == OUR_MEP4_PORT;
	if (!toMcast && !toUsUnicast) return false;

	// Disambiguate by protocol header: _asdp_v (discovery) vs _link_v (measurement).
	bool isAsdp = (memcmp(pl, LW_HDR_ASDP, 8) == 0);
	bool isLink = (memcmp(pl, LW_HDR_LINK, 8) == 0);
	if (!isAsdp && !isLink) return toMcast;   // junk on the mcast port: consume, ignore

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
		bool needBurst = !p->measured ||
		                 (now - p->meas.lastPingMicros) > (s64)MEASURE_RETRY_US;
		if (!needBurst && lgMeasHasEnough(&p->meas)) continue;
		if (lgMeasExhausted(&p->meas) && p->measured) { continue; }  // have a result
		if ((now - p->meas.lastPingMicros) >= (s64)PING_INTERVAL_US &&
		    p->meas.pings < LG_NUM_MEASUREMENTS && !lgMeasHasEnough(&p->meas))
		{
			sendPing(p);
			p->meas.lastPingMicros = now;
			p->meas.pings++;
		}
	}
}

void linkProcess(void)
{
	if (!s_pWLAN) return;

	// SINGLE RX DEMUX: one drain, routed by port. Link multicast discovery +
	// Link unicast measurement (to our IP:mep4 port) -> linkTryParse; DHCP :67 ->
	// AP server; DHCP :68 -> station client. Draining is always safe; the per-tick
	// cap keeps Core 2's control loop from being starved by a busy network.
	u8 buf[FRAME_BUF];
	unsigned len;
	int budget = 64;
	while (budget-- > 0 && s_pWLAN->ReceiveFrame(buf, &len))
	{
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
	return beats;
}

void linkShutdown(void)
{
	if (s_pWLAN && s_pWLAN->IsLinkUp()) sendByeBye();
}
