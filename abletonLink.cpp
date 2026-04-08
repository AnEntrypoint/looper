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

static const u8 MAGIC[8]={'_','a','s','d','p','_','v',0x01};
static const u8 MCAST[4]={224,76,78,75};
static const u8 MCAST_MAC[6]={0x01,0x00,0x5e,0x4c,0x4e,0x4b};
static u8 s_ownIP[4]={192,168,4,3};
static CBcm4343Device *s_pWLAN=nullptr;
static double s_bpm=120.0;
static u64 s_nodeId=0, s_lastSend=0;
static bool     s_synced=false;
static unsigned s_lastIgmp=0, s_rxCount=0;

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
	CLogger::Get()->Write("link", LogNotice, "IGMP join 224.76.78.75");
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
		if ((int)len < 42) continue;
		if (buf[12] != 0x08 || buf[13] != 0x00) continue;
		u8 *ip = buf+IP_HDR_OFF; int ihl=(ip[0]&0x0f)*4;
		if(s_rxCount++<20)CLogger::Get()->Write("link",LogNotice,"rx p=%d d=%d.%d.%d.%d",ip[9],ip[16],ip[17],ip[18],ip[19]);
		if (ip[9] != 17) continue;
		u8 *udp = ip + ihl;
		u16 dp; memcpy(&dp, udp+2, 2);
		if(s_rxCount<=20)CLogger::Get()->Write("link",LogNotice,"udp dp=%d",(int)swap16(dp));
		if (memcmp(ip + 16, MCAST, 4) != 0) continue;
		if (swap16(dp) != LINK_PORT) continue;
		u8 *pl = udp+8; int plen=(int)(len-(pl-buf));
		if (plen < 18) continue;
		parsePkt(pl, plen);
	}

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
