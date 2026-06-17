#include "abletonLink.h"
#include "wlanDHCP.h"
#include <circle/timer.h>
#include <circle/util.h>

#define LINK_PORT		20808
#define KEY_TMLN		0x746d6c6eu
#define KEY_MMBE		0x6d6d6265u
#define SEND_INTERVAL_US	1000000u
#define FRAME_BUF		1600
#define IP_HDR_OFF		14
#define UDP_HDR_OFF		34
#define PAYLOAD_OFF		42

static const u8 MAGIC[8]={'_','a','s','d','p','_','v',0x01};
static const u8 MCAST[4]={224,76,78,75};
static const u8 MCAST_MAC[6]={0x01,0x00,0x5e,0x4c,0x4e,0x4b};
static u8 s_ownIP[4]={192,168,4,3};
static CBcm4343Device *s_pWLAN=nullptr;
static double s_bpm=120.0;
static u64 s_nodeId=0, s_lastSend=0;
static bool     s_synced=false;
static unsigned s_lastIgmp=0;

// Timeline (train-on-first-loop). s_timeOrigin = CTimer microseconds at the
// (backdated) rec-press = beat 0; s_beatOrigin = 0 there. Broadcast in the
// tmln TLV so a peer's transport aligns to our first loop as song start.
static bool   s_started   = false;   // rec-on anchored the origin
static bool   s_ended     = false;   // rec-off finalized tempo+quant
static u64    s_timeOrigin = 0;      // us
static s64    s_beatOrigin = 0;      // beats*? Link uses beat units; 0 = start
static double s_quantBeats = 0.0;    // chosen subdivision (beats)

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

static void parsePkt(const u8 *buf, int len)
{
	if (len < 20) return;
	if (memcmp(buf, MAGIC, 8) != 0) return;

	u64 senderId;
	memcpy(&senderId, buf + 12, 8);
	if (senderId == s_nodeId) return;
	const u8 *p=buf+20, *end=buf+len;
	while (p + 8 <= end)
	{
		u32 key, sz;
		memcpy(&key, p,   4); key = swap32(key);
		memcpy(&sz,  p+4, 4); sz  = swap32(sz);
		p += 8;
		if (p + sz > end) break;
		if (key == KEY_TMLN && sz >= 24)
		{
			s64 mpb;
			memcpy(&mpb, p, 8);
			mpb = (s64)swap64((u64)mpb);
			if (mpb > 0)
			{
				double newBpm = 60000000.0 / (double)mpb;
				s_bpm    = newBpm;
				s_synced = true;
			}
		}
		p += sz;
	}
}

static void sendAlive(void)
{
	u8 payload[72];
	int plen = 0;
	memset(payload, 0, sizeof payload);

	memcpy(payload, MAGIC, 8);
	payload[8] = 1;
	payload[9] = 5;
	memcpy(payload + 12, &s_nodeId, 8);

	u32 key = swap32(KEY_MMBE);
	u32 sz  = swap32(8);
	memcpy(payload + 20, &key, 4);
	memcpy(payload + 24, &sz,  4);
	memcpy(payload + 28, &s_nodeId, 8);
	plen = 36;

	if (s_bpm > 0.0)
	{
		s64 mpb   = (s64)(60000000.0 / s_bpm);
		s64 mpbBE = (s64)swap64((u64)mpb);
		// beatOrigin + timeOrigin: once the first loop has been recorded
		// (s_started) these anchor the timeline to the backdated press instant
		// = beat 0, so peers treat our loop as song start. Before that they
		// stay 0 (no anchor yet). Big-endian on the wire like mpb.
		s64 beatBE = (s64)swap64((u64)s_beatOrigin);
		s64 timeBE = (s64)swap64((u64)(s_started ? s_timeOrigin : 0));
		u32 tkey  = swap32(KEY_TMLN);
		u32 tsz   = swap32(24);
		memcpy(payload + plen, &tkey,  4); plen += 4;
		memcpy(payload + plen, &tsz,   4); plen += 4;
		memcpy(payload + plen, &mpbBE,  8); plen += 8;
		memcpy(payload + plen, &beatBE, 8); plen += 8;
		memcpy(payload + plen, &timeBE, 8); plen += 8;
	}

	u8 frame[FRAME_BUF];
	memset(frame, 0, PAYLOAD_OFF + plen);

	memcpy(frame+0,MCAST_MAC,6); memcpy(frame+6,&s_nodeId,6);
	frame[12]=0x08; frame[13]=0x00;

	u8 *ip = frame + IP_HDR_OFF;
	ip[0]=0x45; ip[6]=0x40; ip[8]=1; ip[9]=17;
	u16 ipLen=swap16(20+8+plen); memcpy(ip+2,&ipLen,2);
	if (wlanDhcpOK()) memcpy(s_ownIP, wlanDhcpIP(), 4);
	memcpy(ip+12,s_ownIP,4); memcpy(ip+16,MCAST,4);
	u16 csum=swap16(csum16(ip,20)); memcpy(ip+10,&csum,2);
	u8 *udp=frame+UDP_HDR_OFF;
	u16 p16=swap16(LINK_PORT), uLen=swap16(8+plen);
	memcpy(udp,&p16,2); memcpy(udp+2,&p16,2); memcpy(udp+4,&uLen,2);

	memcpy(frame + PAYLOAD_OFF, payload, plen);

	s_pWLAN->SendFrame(frame, PAYLOAD_OFF + plen);
}

static void sendIgmpJoin(void)
{
	u8 f[60];
	memset(f, 0, sizeof f);
	memcpy(f, MCAST_MAC, 6);
	memcpy(f + 6, &s_nodeId, 6);
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
}

// Classify ONE received frame as Link multicast (our group + UDP :20808) and, if
// so, feed its payload to parsePkt. Returns true iff the frame was ours (consumed)
// so the single demux doesn't also offer it to the DHCP handlers.
static bool linkTryParse(const u8 *buf, unsigned len)
{
	if ((int)len < 42) return false;
	if (buf[12] != 0x08 || buf[13] != 0x00) return false;        // not IPv4
	const u8 *ip = buf+IP_HDR_OFF; int ihl=(ip[0]&0x0f)*4;
	if (ip[9] != 17) return false;                                // not UDP
	if (memcmp(ip + 16, MCAST, 4) != 0) return false;             // not our mcast
	const u8 *udp = ip + ihl;
	u16 dp; memcpy(&dp, udp+2, 2);
	if (swap16(dp) != LINK_PORT) return false;                    // not :20808
	const u8 *pl = udp+8; int plen=(int)(len-(pl-buf));
	if (plen >= 18) parsePkt(pl, plen);
	return true;                                                  // ours either way
}

void linkProcess(void)
{
	if (!s_pWLAN) return;

	// SINGLE RX DEMUX (load-bearing). The radio has ONE receive queue; Link, the
	// AP DHCP server, and the station DHCP client all need frames off it. They
	// USED to each run their own ReceiveFrame drain and DROP frames they didn't
	// recognise, so whichever ran first ate the others' packets — in AP mode the
	// DHCP-server drain (called before linkProcess) swallowed inbound Link
	// multicast, so peers on the hosted "ticker" never tempo-synced. Now we drain
	// HERE, once, and route each frame by port: Link :20808 -> parsePkt, DHCP :67
	// -> AP server, DHCP :68 -> station client. Draining is always safe (only
	// SendFrame wedges an un-associated radio); the per-tick cap keeps Core 2's
	// control loop from being starved by a busy network.
	u8 buf[FRAME_BUF];
	unsigned len;
	int budget = 64;
	while (budget-- > 0 && s_pWLAN->ReceiveFrame(buf, &len))
	{
		if (linkTryParse(buf, len)) continue;            // Link multicast :20808
		if (wlanDhcpServeFrame(buf, (int)len)) continue; // AP DHCP server :67
		if (wlanDhcpClientFrame(buf, (int)len)) continue;// station DHCP client :68
		// otherwise: not ours — dropped.
	}

	// TX is gated on association: SendFrame() over an un-associated radio wedges
	// the plan9/DWC-SDIO transmit path and freezes Core 2's control plane. In AP
	// mode IsLinkUp() is true once CreateOpenNet succeeds (we host the link); when
	// joined it tracks the station assoc state. The DHCP reply/request SendFrames
	// above are reactive (only after a frame was received => radio is live), so
	// they need no extra gate; only this proactive 1 Hz beacon does.
	if (!s_pWLAN->IsLinkUp()) return;

	u64 now = (u64)CTimer::GetClockTicks();
	if (now - s_lastSend >= SEND_INTERVAL_US)
	{
		sendAlive();
		s_lastSend = now;
	}
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

void linkStart(unsigned originTicks)
{
	// Anchor the timeline at the (backdated) press instant = beat 0.
	s_timeOrigin = originTicks ? (u64)originTicks : (u64)CTimer::GetClockTicks();
	s_beatOrigin = 0;
	s_started    = true;
	s_ended      = false;
	s_quantBeats = 0.0;
	// Until rec-off we hold the boot/default tempo; the loop length sets it.
}

void linkDeriveQuant(double clip_seconds, double *out_beats, double *out_bpm)
{
	// Musical beat-count candidates; choose the one whose BPM is nearest 120,
	// preferring candidates inside [80,160]. (Mirrors scripts/test-link-quant.cpp.)
	static const double cand[] = {0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0};
	const int N = (int)(sizeof cand / sizeof cand[0]);
	if (clip_seconds <= 0.0001) { *out_beats = 4.0; *out_bpm = 120.0; return; }

	double bestB = 4.0, bestBpm = 120.0, bestDist = 1e18; bool haveInWin = false;
	for (int i = 0; i < N; i++)
	{
		double bpm = 60.0 * cand[i] / clip_seconds;
		double dist = bpm > 120.0 ? bpm - 120.0 : 120.0 - bpm;
		bool inWin = (bpm >= 80.0 && bpm <= 160.0);
		// Prefer in-window; among in-window, nearest 120. If none in window yet,
		// track nearest overall as fallback.
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
	// Origin stays at the rec-on press (beat 0); sendAlive now broadcasts the
	// finalized mpb + origin so peers sync to this loop as the song-start pattern.
	return beats;
}
