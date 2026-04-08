#include "abletonLink.h"
#include "wlanDHCP.h"
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/util.h>

#define LINK_PORT		20808
#define KEY_TMLN		0x746d6c6eu
#define KEY_MMBE		0x6d6d6265u
#define SEND_INTERVAL_US	1000000u
#define FRAME_BUF		1600
#define IP_HDR_OFF		14
#define UDP_HDR_OFF		34
#define PAYLOAD_OFF		42

static const u8 MAGIC[8] = {'_','a','s','d','p','_','v',0x01};
static const u8 MCAST[4]     = {224, 76, 78, 75};
static const u8 MCAST_MAC[6] = {0x01, 0x00, 0x5e, 0x4c, 0x4e, 0x4b};
static u8 s_ownIP[4] = {192, 168, 4, 3};

static CBcm4343Device *s_pWLAN    = nullptr;
static double          s_bpm      = 120.0;
static u64             s_nodeId   = 0;
static u64             s_lastSend = 0;
static bool            s_synced   = false;
static bool            s_loggedSend = false;
static u32             s_rxCount  = 0;

static inline u16 swap16(u16 v) { return __builtin_bswap16(v); }
static inline u32 swap32(u32 v) { return __builtin_bswap32(v); }
static inline u64 swap64(u64 v) { return __builtin_bswap64(v); }

static u16 ipChecksum(const u8 *hdr, int len)
{
	u32 sum = 0;
	for (int i = 0; i < len; i += 2)
	{
		u16 w = ((u16)hdr[i] << 8) | hdr[i+1];
		sum += w;
	}
	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
	return (u16)(~sum);
}

static void parsePkt(const u8 *buf, int len)
{
	if (len < 18) return;
	if (memcmp(buf, MAGIC, 8) != 0) return;

	u64 senderId;
	memcpy(&senderId, buf + 10, 8);
	if (senderId == s_nodeId) return;

	const u8 *p   = buf + 10;
	const u8 *end = buf + len;

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
				if (!s_synced)
					CLogger::Get()->Write("link", LogNotice, "Link peer synced: bpm=%.2f", newBpm);
				s_bpm    = newBpm;
				s_synced = true;
			}
		}
		p += sz;
	}
}

static void sendAlive(void)
{
	u8 payload[64];
	int plen = 0;
	memset(payload, 0, sizeof payload);

	memcpy(payload, MAGIC, 8);
	payload[8] = 1;
	payload[9] = 1;
	memcpy(payload + 10, &s_nodeId, 8);

	u32 key = swap32(KEY_MMBE);
	u32 sz  = swap32(8);
	memcpy(payload + 18, &key, 4);
	memcpy(payload + 22, &sz,  4);
	memcpy(payload + 26, &s_nodeId, 8);
	plen = 34;

	if (s_bpm > 0.0)
	{
		s64 mpb   = (s64)(60000000.0 / s_bpm);
		s64 mpbBE = (s64)swap64((u64)mpb);
		s64 zero  = 0;
		u32 tkey  = swap32(KEY_TMLN);
		u32 tsz   = swap32(24);
		memcpy(payload + plen, &tkey,  4); plen += 4;
		memcpy(payload + plen, &tsz,   4); plen += 4;
		memcpy(payload + plen, &mpbBE, 8); plen += 8;
		memcpy(payload + plen, &zero,  8); plen += 8;
		memcpy(payload + plen, &zero,  8); plen += 8;
	}

	u8 frame[FRAME_BUF];
	memset(frame, 0, PAYLOAD_OFF + plen);

	memcpy(frame + 0, MCAST_MAC, 6);
	memcpy(frame + 6, &s_nodeId, 6);
	frame[12] = 0x08;
	frame[13] = 0x00;

	u8 *ip = frame + IP_HDR_OFF;
	ip[0]  = 0x45;
	ip[1]  = 0;
	u16 ipLen = swap16(20 + 8 + plen);
	memcpy(ip + 2, &ipLen, 2);
	ip[6]  = 0x40;
	ip[8]  = 1;
	ip[9]  = 17;
	if (wlanDhcpOK()) memcpy(s_ownIP, wlanDhcpIP(), 4);
	memcpy(ip + 12, s_ownIP, 4);
	memcpy(ip + 16, MCAST,  4);
	u16 csum = swap16(ipChecksum(ip, 20));
	memcpy(ip + 10, &csum, 2);

	u8 *udp = frame + UDP_HDR_OFF;
	u16 srcPort = swap16(LINK_PORT);
	u16 dstPort = swap16(LINK_PORT);
	u16 udpLen  = swap16(8 + plen);
	memcpy(udp + 0, &srcPort, 2);
	memcpy(udp + 2, &dstPort, 2);
	memcpy(udp + 4, &udpLen,  2);

	memcpy(frame + PAYLOAD_OFF, payload, plen);

	s_pWLAN->SendFrame(frame, PAYLOAD_OFF + plen);
}

void linkInit(CBcm4343Device *pWLAN)
{
	s_pWLAN    = pWLAN;
	s_nodeId   = (u64)CTimer::GetClockTicks();
	s_bpm      = 120.0;
	s_lastSend = 0;
	s_synced   = false;
}

void linkProcess(void)
{
	if (!s_pWLAN) return;

	u8 buf[FRAME_BUF];
	unsigned len;
	while (s_pWLAN->ReceiveFrame(buf, &len))
	{
		s_rxCount++;
		if (s_rxCount <= 5)
			CLogger::Get()->Write("link", LogNotice, "rx len=%u eth=%02x%02x proto=%d dst=%d.%d.%d.%d port=%d",
				len, buf[12], buf[13],
				len > IP_HDR_OFF ? buf[IP_HDR_OFF+9] : 0,
				len > IP_HDR_OFF ? buf[IP_HDR_OFF+16] : 0,
				len > IP_HDR_OFF ? buf[IP_HDR_OFF+17] : 0,
				len > IP_HDR_OFF ? buf[IP_HDR_OFF+18] : 0,
				len > IP_HDR_OFF ? buf[IP_HDR_OFF+19] : 0,
				len > UDP_HDR_OFF+2 ? (int)swap16(*(u16*)(buf+UDP_HDR_OFF+2)) : 0);
		if (len < PAYLOAD_OFF + 18) continue;
		if (buf[12] != 0x08 || buf[13] != 0x00) continue;
		u8 *ip = buf + IP_HDR_OFF;
		if (ip[9] != 17) continue;
		if (memcmp(ip + 16, MCAST, 4) != 0) continue;
		u8 *udp = buf + UDP_HDR_OFF;
		u16 dstPort;
		memcpy(&dstPort, udp + 2, 2);
		if (swap16(dstPort) != LINK_PORT) continue;
		parsePkt(buf + PAYLOAD_OFF, (int)(len - PAYLOAD_OFF));
	}

	u64 now = (u64)CTimer::GetClockTicks();
	if (now - s_lastSend >= SEND_INTERVAL_US)
	{
		if (!s_loggedSend)
		{
			CLogger::Get()->Write("link", LogNotice, "sending alive bpm=%d", (int)s_bpm);
			s_loggedSend = true;
		}
		sendAlive();
		s_lastSend = now;
	}
}

double linkGetBPM(void)       { return s_bpm; }
void   linkSetBPM(double bpm) { s_bpm = bpm; }
bool   linkIsSynced(void)     { return s_synced; }
