#include "abletonLink.h"
#include <circle/net/ipaddress.h>
#include <circle/net/in.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/util.h>

#define log_name "link"
#define LINK_PORT		20808
#define KEY_TMLN		0x746d6c6eu
#define KEY_MMBE		0x6d6d6265u
#define SEND_INTERVAL_US	1000000u

static const u8 MAGIC[8] = {'_','a','s','d','p','_','v',0x01};
static const u8 MCAST[4] = {224, 76, 78, 75};

static CNetSubSystem *s_pNet    = nullptr;
static CSocket       *s_pSocket = nullptr;
static double         s_bpm     = 120.0;
static u64            s_nodeId  = 0;
static u64            s_lastSend = 0;
static bool           s_synced  = false;

static inline u32 swap32(u32 v) { return __builtin_bswap32(v); }
static inline u64 swap64(u64 v) { return __builtin_bswap64(v); }

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
				s_bpm = 60000000.0 / (double)mpb;
				s_synced = true;
			}
		}
		p += sz;
	}
}

static void sendAlive(void)
{
	u8 buf[64];
	memset(buf, 0, sizeof buf);

	memcpy(buf, MAGIC, 8);
	buf[8] = 1;
	buf[9] = 1;
	memcpy(buf + 10, &s_nodeId, 8);

	u32 key = swap32(KEY_MMBE);
	u32 sz  = swap32(8);
	memcpy(buf + 18, &key, 4);
	memcpy(buf + 22, &sz,  4);
	memcpy(buf + 26, &s_nodeId, 8);

	int total = 34;

	if (s_bpm > 0.0)
	{
		s64 mpb   = (s64)(60000000.0 / s_bpm);
		s64 mpbBE = (s64)swap64((u64)mpb);
		s64 zero  = 0;
		u32 tkey  = swap32(KEY_TMLN);
		u32 tsz   = swap32(24);
		memcpy(buf + total, &tkey,  4); total += 4;
		memcpy(buf + total, &tsz,   4); total += 4;
		memcpy(buf + total, &mpbBE, 8); total += 8;
		memcpy(buf + total, &zero,  8); total += 8;
		memcpy(buf + total, &zero,  8); total += 8;
	}

	CIPAddress dest(MCAST);
	s_pSocket->SendTo(buf, total, 0, dest, LINK_PORT);
}

void linkInit(CNetSubSystem *pNet, CSocket *pSocket)
{
	s_pNet    = pNet;
	s_pSocket = pSocket;
	s_nodeId  = (u64)CTimer::GetClockTicks();
	s_bpm     = 120.0;
	s_lastSend = 0;
	s_synced  = false;
}

void linkProcess(void)
{
	if (!s_pSocket) return;

	u8 buf[512];
	CIPAddress sender;
	u16 senderPort;
	int n;
	while ((n = s_pSocket->ReceiveFrom(buf, sizeof buf, MSG_DONTWAIT, &sender, &senderPort)) > 0)
		parsePkt(buf, n);

	u64 now = (u64)CTimer::GetClockTicks();
	if (now - s_lastSend >= SEND_INTERVAL_US)
	{
		sendAlive();
		s_lastSend = now;
	}
}

double linkGetBPM(void)       { return s_bpm; }
void   linkSetBPM(double bpm) { s_bpm = bpm; }
bool   linkIsSynced(void)     { return s_synced; }
