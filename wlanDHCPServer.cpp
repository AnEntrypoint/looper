#include "wlanDHCP.h"
#include <circle/logger.h>
#include <circle/util.h>

#define ETH_HDR  14
#define IP_HDR   20
#define UDP_HDR  8
#define DHCP_HDR 236
#define FRAME_SZ 600
#define POOL_START 2
#define POOL_SIZE  8

static CBcm4343Device *s_pAP;
static u8 s_serverIP[4] = {192, 168, 4, 1};
static u8 s_mask[4]     = {255, 255, 255, 0};
static u8 s_pool[POOL_SIZE][4];
static u8 s_poolMAC[POOL_SIZE][6];
static u8 s_poolUsed[POOL_SIZE];

static inline u16 sw16(u16 v) { return __builtin_bswap16(v); }

static u16 ipCs(const u8 *h, int n) {
	u32 s = 0;
	for (int i = 0; i < n; i += 2) s += ((u16)h[i] << 8) | h[i+1];
	while (s >> 16) s = (s & 0xffff) + (s >> 16);
	return (u16)~s;
}

static int poolFind(const u8 *mac) {
	for (int i = 0; i < POOL_SIZE; i++)
		if (s_poolUsed[i] && memcmp(s_poolMAC[i], mac, 6) == 0) return i;
	return -1;
}

static int poolAlloc(const u8 *mac) {
	int slot = poolFind(mac);
	if (slot >= 0) return slot;
	for (int i = 0; i < POOL_SIZE; i++) {
		if (!s_poolUsed[i]) {
			s_poolUsed[i] = 1;
			memcpy(s_poolMAC[i], mac, 6);
			return i;
		}
	}
	return -1;
}

static void buildReply(u8 *f, const u8 *clientMAC, u32 xid, const u8 *yip, u8 msgType, int *outLen) {
	memset(f, 0, FRAME_SZ);
	const u8 bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
	const u8 bcastIP[4] = {255,255,255,255};
	memcpy(f, bcast, 6);
	memcpy(f + 6, s_serverIP, 4);
	f[10] = 0; f[11] = 0; f[12] = 0;
	f[12] = 0x08; f[13] = 0x00;
	u8 *ip = f + ETH_HDR;
	ip[0] = 0x45; ip[8] = 64; ip[9] = 17;
	memcpy(ip + 12, s_serverIP, 4);
	memcpy(ip + 16, bcastIP, 4);
	u8 *udp = ip + IP_HDR;
	u16 sp = sw16(67), dp = sw16(68);
	memcpy(udp, &sp, 2); memcpy(udp + 2, &dp, 2);
	u8 *d = udp + UDP_HDR;
	d[0] = 2; d[1] = 1; d[2] = 6;
	memcpy(d + 4, &xid, 4);
	memcpy(d + 16, yip, 4);
	memcpy(d + 20, s_serverIP, 4);
	memcpy(d + 28, clientMAC, 6);
	const u8 magic[4] = {99,130,83,99};
	memcpy(d + DHCP_HDR, magic, 4);
	u8 *o = d + DHCP_HDR + 4;
	*o++ = 53; *o++ = 1; *o++ = msgType;
	*o++ = 54; *o++ = 4; memcpy(o, s_serverIP, 4); o += 4;
	*o++ = 1;  *o++ = 4; memcpy(o, s_mask, 4); o += 4;
	*o++ = 3;  *o++ = 4; memcpy(o, s_serverIP, 4); o += 4;
	*o++ = 51; *o++ = 4; *o++ = 0; *o++ = 1; *o++ = 0x51; *o++ = 0x80;
	*o++ = 255;
	int plen = (int)(o - d);
	int udpLen = UDP_HDR + plen, ipLen = IP_HDR + udpLen;
	u16 uL = sw16(udpLen), iL = sw16(ipLen);
	memcpy(udp + 4, &uL, 2); memcpy(ip + 2, &iL, 2);
	u16 cs = sw16(ipCs(ip, IP_HDR));
	memcpy(ip + 10, &cs, 2);
	*outLen = ETH_HDR + ipLen;
}

static bool parseClient(const u8 *f, int len, u8 *outMAC, u32 *outXid, u8 *outType) {
	if (len < 282) return false;
	if (f[12] != 0x08 || f[13] != 0x00) return false;
	const u8 *ip = f + ETH_HDR;
	if (ip[9] != 17) return false;
	const u8 *udp = ip + IP_HDR;
	u16 dp; memcpy(&dp, udp, 2);
	if (sw16(dp) != 67) return false;
	const u8 *d = udp + UDP_HDR;
	if (d[0] != 1) return false;
	memcpy(outXid, d + 4, 4);
	memcpy(outMAC, d + 28, 6);
	const u8 *o = d + DHCP_HDR + 4;
	const u8 *end = f + len;
	while (o + 2 <= end && *o != 255) {
		if (*o == 53 && o[1] >= 1) { *outType = o[2]; return true; }
		o += 2 + o[1];
	}
	return false;
}

void wlanApInit(CBcm4343Device *pWLAN) {
	s_pAP = pWLAN;
	memset(s_poolUsed, 0, sizeof s_poolUsed);
	for (int i = 0; i < POOL_SIZE; i++) {
		s_pool[i][0] = 192; s_pool[i][1] = 168;
		s_pool[i][2] = 4;   s_pool[i][3] = POOL_START + i;
	}
	wlanApSetIP(s_serverIP);
}

void wlanDhcpServe(void) {
	if (!s_pAP) return;
	u8 buf[FRAME_SZ]; unsigned rlen;
	while (s_pAP->ReceiveFrame(buf, &rlen)) {
		u8 mac[6]; u32 xid; u8 type = 0;
		if (!parseClient(buf, (int)rlen, mac, &xid, &type)) continue;
		if (type == 1) {
			int slot = poolAlloc(mac);
			if (slot < 0) continue;
			u8 f[FRAME_SZ]; int flen;
			buildReply(f, mac, xid, s_pool[slot], 2, &flen);
			s_pAP->SendFrame(f, flen);
			CLogger::Get()->Write("wdhcps", LogNotice, "OFFER %d.%d.%d.%d", s_pool[slot][0], s_pool[slot][1], s_pool[slot][2], s_pool[slot][3]);
		} else if (type == 3) {
			int slot = poolFind(mac);
			if (slot < 0) continue;
			u8 f[FRAME_SZ]; int flen;
			buildReply(f, mac, xid, s_pool[slot], 5, &flen);
			s_pAP->SendFrame(f, flen);
			CLogger::Get()->Write("wdhcps", LogNotice, "ACK %d.%d.%d.%d", s_pool[slot][0], s_pool[slot][1], s_pool[slot][2], s_pool[slot][3]);
		}
	}
}
