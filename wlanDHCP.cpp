#include "wlanDHCP.h"
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/util.h>

#define ETH_HDR  14
#define IP_HDR   20
#define UDP_HDR  8
#define DHCP_HDR 236
#define OPT_OFF  (ETH_HDR + IP_HDR + UDP_HDR + DHCP_HDR)
#define FRAME_SZ 600

static u8 s_ip[4];
static bool s_got;

static inline u16 sw16(u16 v) { return __builtin_bswap16(v); }
static inline u32 sw32(u32 v) { return __builtin_bswap32(v); }

static u16 ipCsum(const u8 *h, int n) {
	u32 s = 0;
	for (int i = 0; i < n; i += 2) s += ((u16)h[i] << 8) | h[i+1];
	while (s >> 16) s = (s & 0xffff) + (s >> 16);
	return (u16)~s;
}

static void buildDiscover(u8 *f, const u8 *mac, u32 xid, int *outLen) {
	memset(f, 0, FRAME_SZ);
	const u8 bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
	const u8 bcastIP[4] = {255,255,255,255};
	const u8 zeroIP[4] = {0,0,0,0};
	memcpy(f, bcast, 6);
	memcpy(f+6, mac, 6);
	f[12]=0x08; f[13]=0x00;
	u8 *ip = f+ETH_HDR;
	ip[0]=0x45; ip[8]=64; ip[9]=17;
	memcpy(ip+12, zeroIP, 4);
	memcpy(ip+16, bcastIP, 4);
	u8 *udp = ip+IP_HDR;
	u16 sp=sw16(68), dp=sw16(67);
	memcpy(udp, &sp, 2); memcpy(udp+2, &dp, 2);
	u8 *d = udp+UDP_HDR;
	d[0]=1; d[1]=1; d[2]=6;
	memcpy(d+4, &xid, 4);
	memcpy(d+28, mac, 6);
	const u8 magic[4]={99,130,83,99};
	memcpy(d+DHCP_HDR, magic, 4);
	u8 *o = d+DHCP_HDR+4;
	*o++=53; *o++=1; *o++=1;
	*o++=255;
	int plen = (int)(o - d);
	int udpLen = UDP_HDR + plen;
	int ipLen = IP_HDR + udpLen;
	u16 uL=sw16(udpLen), iL=sw16(ipLen);
	memcpy(udp+4, &uL, 2);
	memcpy(ip+2, &iL, 2);
	u16 cs = sw16(ipCsum(ip, IP_HDR));
	memcpy(ip+10, &cs, 2);
	*outLen = ETH_HDR + ipLen;
}

static void buildRequest(u8 *f, const u8 *mac, u32 xid, const u8 *offer, int *outLen) {
	memset(f, 0, FRAME_SZ);
	const u8 bcast[6]={0xff,0xff,0xff,0xff,0xff,0xff};
	const u8 bcastIP[4]={255,255,255,255};
	const u8 zeroIP[4]={0,0,0,0};
	memcpy(f, bcast, 6); memcpy(f+6, mac, 6);
	f[12]=0x08; f[13]=0x00;
	u8 *ip=f+ETH_HDR;
	ip[0]=0x45; ip[8]=64; ip[9]=17;
	memcpy(ip+12, zeroIP, 4); memcpy(ip+16, bcastIP, 4);
	u8 *udp=ip+IP_HDR;
	u16 sp=sw16(68), dp=sw16(67);
	memcpy(udp, &sp, 2); memcpy(udp+2, &dp, 2);
	u8 *d=udp+UDP_HDR;
	d[0]=1; d[1]=1; d[2]=6;
	memcpy(d+4, &xid, 4);
	memcpy(d+28, mac, 6);
	const u8 magic[4]={99,130,83,99};
	memcpy(d+DHCP_HDR, magic, 4);
	u8 *o=d+DHCP_HDR+4;
	*o++=53; *o++=1; *o++=3;
	*o++=50; *o++=4; memcpy(o, offer, 4); o+=4;
	*o++=255;
	int plen=(int)(o-d);
	int udpLen=UDP_HDR+plen, ipLen=IP_HDR+udpLen;
	u16 uL=sw16(udpLen), iL=sw16(ipLen);
	memcpy(udp+4, &uL, 2); memcpy(ip+2, &iL, 2);
	u16 cs=sw16(ipCsum(ip, IP_HDR));
	memcpy(ip+10, &cs, 2);
	*outLen=ETH_HDR+ipLen;
}

static bool parseOffer(const u8 *f, int len, u32 xid, u8 *outIP) {
	if (len < OPT_OFF+4) return false;
	if (f[12]!=0x08 || f[13]!=0x00) return false;
	const u8 *ip=f+ETH_HDR;
	if (ip[9]!=17) return false;
	const u8 *udp=ip+IP_HDR;
	u16 dp; memcpy(&dp, udp+2, 2);
	if (sw16(dp)!=68) return false;
	const u8 *d=udp+UDP_HDR;
	if (d[0]!=2) return false;
	u32 rxid; memcpy(&rxid, d+4, 4);
	if (rxid!=xid) return false;
	memcpy(outIP, d+16, 4);
	const u8 *o=d+DHCP_HDR+4;
	const u8 *end=f+len;
	while (o+2<=end && *o!=255) {
		if (*o==53 && o[1]>=1) { if (o[2]==2||o[2]==5) return true; }
		o+=2+o[1];
	}
	return false;
}

void wlanDhcpGetIP(CBcm4343Device *pWLAN, const u8 *mac) {
	s_got=false;
	memset(s_ip, 0, 4);
	u32 xid = (u32)(CTimer::GetClockTicks() & 0xFFFFFFFF);
	u8 frame[FRAME_SZ];
	int flen;
	buildDiscover(frame, mac, xid, &flen);
	pWLAN->SendFrame(frame, flen);
	CLogger::Get()->Write("wdhcp", LogNotice, "DISCOVER sent");
	u8 offer[4]={0};
	u64 deadline = CTimer::GetClockTicks() + (u64)10 * CLOCKHZ;
	while (CTimer::GetClockTicks() < deadline) {
		u8 buf[FRAME_SZ]; unsigned rlen;
		while (pWLAN->ReceiveFrame(buf, &rlen)) {
			if (parseOffer(buf, rlen, xid, offer)) {
				CLogger::Get()->Write("wdhcp", LogNotice, "OFFER %d.%d.%d.%d", offer[0],offer[1],offer[2],offer[3]);
				buildRequest(frame, mac, xid, offer, &flen);
				pWLAN->SendFrame(frame, flen);
			}
		}
		if (offer[0]) { memcpy(s_ip, offer, 4); s_got=true; break; }
	}
	if (!s_got)
		CLogger::Get()->Write("wdhcp", LogWarning, "DHCP timeout — no offer in 10s");
}

bool wlanDhcpOK(void) { return s_got; }
const u8 *wlanDhcpIP(void) { return s_ip; }
