// Single-RX-demux routing for the WLAN path (abletonLink.cpp::linkProcess +
// wlanDHCPServer.cpp + wlanDHCP.cpp). The radio has ONE receive queue; Link,
// the AP DHCP server, and the station DHCP client all pull from it. They used to
// each run an independent ReceiveFrame drain and DROP frames they didn't own, so
// in AP mode the DHCP-server drain (called first) swallowed inbound Link
// multicast and peers on the hosted "ticker" never tempo-synced. The fix drains
// once and routes each frame by port: Link :20808 -> parsePkt, DHCP :67 -> AP
// server, DHCP :68 -> station client.
//
// This test mirrors the production classifiers and asserts: (1) every frame is
// claimed by EXACTLY ONE consumer (or none, for non-ours), so no consumer eats
// another's packets; (2) a real client DHCP DISCOVER (src=68,dst=67) is claimed
// by the server -- the regression guard for the parseClient src/dst port bug
// (it read the SOURCE port and required ==67, rejecting every real request).
//
// Build: g++ -O2 -std=c++17 scripts/test-wlan-rxdemux.cpp -o scripts/test-wlan-rxdemux.exe
#include <cstdio>
#include <cstdint>
#include <cstring>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
static int g_fails = 0;
static void check(const char* n, bool c){ if(c)printf("ok: %s\n",n); else {printf("FAIL: %s\n",n);g_fails++;} }

static const int ETH_HDR = 14, IP_HDR = 20, UDP_HDR = 8, DHCP_HDR = 236;
static const int LINK_PORT = 20808;
static const u8  MCAST[4]  = {224,76,78,75};
static const u8  OUR_IP[4] = {192,168,4,1};       // our AP IP / measurement endpoint
static const int OUR_MEP4_PORT = 20808;
static const u32 TEST_XID = 0xAABBCCDDu;
static const u8  HDR_ASDP[8] = {'_','a','s','d','p','_','v',1};
static const u8  HDR_LINK[8] = {'_','l','i','n','k','_','v',1};

static inline u16 be16(u16 v){ return (u16)((v>>8)|(v<<8)); }

// ---- production classifiers, mirrored exactly (current linkTryParse) ----

// linkTryParse: IPv4/UDP to EITHER multicast:20808 OR our-IP:mep4-port, with a
// _asdp_v (discovery) or _link_v (measurement) payload header. This is the
// extended unicast-aware version.
static bool linkClaim(const u8 *buf, int len){
    if (len < 42) return false;
    if (buf[12]!=0x08 || buf[13]!=0x00) return false;
    const u8 *ip = buf+ETH_HDR; int ihl=(ip[0]&0x0f)*4;
    if (ip[9]!=17) return false;
    const u8 *udp = ip+ihl;
    u16 dp; memcpy(&dp, udp+2, 2); u16 dport = be16(dp);
    bool toMcast      = memcmp(ip+16, MCAST, 4)==0  && dport==LINK_PORT;
    bool toUsUnicast  = memcmp(ip+16, OUR_IP, 4)==0 && dport==OUR_MEP4_PORT;
    if (!toMcast && !toUsUnicast) return false;
    const u8 *pl = udp+8; int plen=(int)(len-(pl-buf));
    if (plen < 20) return false;
    bool isAsdp = memcmp(pl, HDR_ASDP, 8)==0;
    bool isLink = memcmp(pl, HDR_LINK, 8)==0;
    if (!isAsdp && !isLink) return toMcast;   // junk on mcast port consumed+ignored
    return true;
}
// parseClient (AP server): IPv4/UDP, DST port 67, BOOTREQUEST, has opt53.
static bool serverClaim(const u8 *f, int len){
    if (len < 282) return false;
    if (f[12]!=0x08 || f[13]!=0x00) return false;
    const u8 *ip = f+ETH_HDR;
    if (ip[9]!=17) return false;
    const u8 *udp = ip+IP_HDR;
    u16 dp; memcpy(&dp, udp+2, 2);          // DEST port (the fix)
    if (be16(dp)!=67) return false;
    const u8 *d = udp+UDP_HDR;
    if (d[0]!=1) return false;              // BOOTREQUEST
    const u8 *o = d+DHCP_HDR+4; const u8 *end = f+len;
    while (o+2<=end && *o!=255){ if(*o==53 && o[1]>=1) return true; o+=2+o[1]; }
    return false;
}
// The OLD BUGGY server predicate read the SOURCE port — kept to prove the fix.
static bool serverClaimBuggy(const u8 *f, int len){
    if (len < 282) return false;
    if (f[12]!=0x08 || f[13]!=0x00) return false;
    const u8 *ip = f+ETH_HDR;
    if (ip[9]!=17) return false;
    const u8 *udp = ip+IP_HDR;
    u16 dp; memcpy(&dp, udp, 2);            // SOURCE port (the bug)
    if (be16(dp)!=67) return false;
    const u8 *d = udp+UDP_HDR;
    if (d[0]!=1) return false;
    const u8 *o = d+DHCP_HDR+4; const u8 *end = f+len;
    while (o+2<=end && *o!=255){ if(*o==53 && o[1]>=1) return true; o+=2+o[1]; }
    return false;
}
// parseOffer (station client): IPv4/UDP, DST port 68, BOOTREPLY, xid match, opt53 2/5.
static bool clientClaim(const u8 *f, int len){
    const int OPT_OFF = ETH_HDR+IP_HDR+UDP_HDR+DHCP_HDR;
    if (len < OPT_OFF+4) return false;
    if (f[12]!=0x08 || f[13]!=0x00) return false;
    const u8 *ip = f+ETH_HDR;
    if (ip[9]!=17) return false;
    const u8 *udp = ip+IP_HDR;
    u16 dp; memcpy(&dp, udp+2, 2);
    if (be16(dp)!=68) return false;
    const u8 *d = udp+UDP_HDR;
    if (d[0]!=2) return false;              // BOOTREPLY
    u32 rxid; memcpy(&rxid, d+4, 4);
    if (rxid != TEST_XID) return false;
    const u8 *o = d+DHCP_HDR+4; const u8 *end = f+len;
    while (o+2<=end && *o!=255){ if(*o==53 && o[1]>=1 && (o[2]==2||o[2]==5)) return true; o+=2+o[1]; }
    return false;
}

// ---- frame builders ----
static int makeUDP(u8 *f, u16 ethType, u8 proto, const u8 dstIP[4], u16 srcPort, u16 dstPort, int total){
    memset(f, 0, total);
    f[12]=(u8)(ethType>>8); f[13]=(u8)ethType;
    u8 *ip = f+ETH_HDR;
    ip[0]=0x45; ip[9]=proto;
    if (dstIP) memcpy(ip+16, dstIP, 4);
    u8 *udp = ip+IP_HDR;
    u16 sp=be16(srcPort), dpp=be16(dstPort);
    memcpy(udp, &sp, 2); memcpy(udp+2, &dpp, 2);
    return total;
}
// add a BOOTP body with op + xid + msgtype option, return frame length
static int makeDHCP(u8 *f, u16 srcPort, u16 dstPort, u8 op, u32 xid, u8 msgType, int total){
    const u8 anyIP[4]={1,2,3,4};
    makeUDP(f, 0x0800, 17, anyIP, srcPort, dstPort, total);
    u8 *d = f+ETH_HDR+IP_HDR+UDP_HDR;
    d[0]=op; d[1]=1; d[2]=6;
    memcpy(d+4, &xid, 4);
    const u8 magic[4]={99,130,83,99}; memcpy(d+DHCP_HDR, magic, 4);
    u8 *o = d+DHCP_HDR+4;
    *o++=53; *o++=1; *o++=msgType; *o++=255;
    return total;
}

// Stamp a Link protocol header at the UDP payload start (offset 42).
static int makeLink(u8 *f, const u8 dstIP[4], u16 dstPort, const u8 hdr[8], int total){
    makeUDP(f, 0x0800, 17, dstIP, 40000, dstPort, total);
    memcpy(f + ETH_HDR + IP_HDR + UDP_HDR, hdr, 8);
    // a NodeId follows at +12 of the payload; leave zero (fine for routing).
    return total;
}

static void route(const u8 *f, int len, bool &L, bool &S, bool &C){
    L = linkClaim(f, len);
    S = !L && serverClaim(f, len);
    C = !L && !S && clientClaim(f, len);
}

int main(){
    u8 f[600]; bool L,S,C;

    // 1. Link multicast (realistic >=20B payload) -> Link only
    { makeUDP(f, 0x0800, 17, MCAST, 20808, 20808, 80); route(f,80,L,S,C);
      check("Link multicast -> Link consumer only", L && !S && !C); }

    // 2. DHCP DISCOVER (src 68, dst 67) -> server only
    { makeDHCP(f, 68, 67, 1, TEST_XID, 1, 300); route(f,300,L,S,C);
      check("DHCP DISCOVER -> AP server only", !L && S && !C); }

    // 3. DHCP REQUEST (src 68, dst 67, type 3) -> server only
    { makeDHCP(f, 68, 67, 1, TEST_XID, 3, 300); route(f,300,L,S,C);
      check("DHCP REQUEST -> AP server only", !L && S && !C); }

    // 4. DHCP OFFER (src 67, dst 68, BOOTREPLY type 2, xid match) -> client only
    { makeDHCP(f, 67, 68, 2, TEST_XID, 2, 300); route(f,300,L,S,C);
      check("DHCP OFFER -> station client only", !L && !S && C); }

    // 5. ARP (non-IPv4) -> nobody
    { makeUDP(f, 0x0806, 0, nullptr, 0, 0, 60); route(f,60,L,S,C);
      check("ARP frame claimed by nobody", !L && !S && !C); }

    // 6. IPv4 TCP -> nobody
    { const u8 ip4[4]={10,0,0,1}; makeUDP(f, 0x0800, 6, ip4, 1, 2, 300); route(f,300,L,S,C);
      check("IPv4 TCP claimed by nobody", !L && !S && !C); }

    // 7. IPv4 UDP to an unrelated port -> nobody (no mis-claim)
    { const u8 ip4[4]={10,0,0,1}; makeUDP(f, 0x0800, 17, ip4, 5000, 1234, 300); route(f,300,L,S,C);
      check("UDP to unrelated port claimed by nobody", !L && !S && !C); }

    // 8. Link multicast on a busy AP: the server drainer must NOT eat it.
    //    (the whole point — before the demux, serverClaim's drain would have
    //    pulled and dropped this frame.)
    { makeUDP(f, 0x0800, 17, MCAST, 20808, 20808, 60);
      check("Link multicast is NOT claimed by the DHCP server", !serverClaim(f,60)); }

    // 9. Regression guard: a real DISCOVER is REJECTED by the old buggy
    //    source-port predicate but ACCEPTED by the fixed dest-port one.
    { makeDHCP(f, 68, 67, 1, TEST_XID, 1, 300);
      check("old src-port parseClient would REJECT a real DISCOVER (bug)", !serverClaimBuggy(f,300));
      check("fixed dest-port parseClient ACCEPTS a real DISCOVER", serverClaim(f,300)); }

    // 10. NEW: multicast _asdp_v discovery -> Link only.
    { makeLink(f, MCAST, 20808, HDR_ASDP, 80); route(f,80,L,S,C);
      check("multicast _asdp_v discovery -> Link only", L && !S && !C); }

    // 11. NEW: unicast _link_v PING to OUR_IP:mep4 port -> Link (measurement).
    { makeLink(f, OUR_IP, OUR_MEP4_PORT, HDR_LINK, 80); route(f,80,L,S,C);
      check("unicast _link_v to our endpoint -> Link measurement", L && !S && !C); }

    // 12. NEW: unicast _link_v to a FOREIGN IP -> nobody (not ours).
    { const u8 foreign[4]={192,168,4,9}; makeLink(f, foreign, OUR_MEP4_PORT, HDR_LINK, 80); route(f,80,L,S,C);
      check("unicast _link_v to a foreign IP -> claimed by nobody", !L && !S && !C); }

    // 13. NEW: DHCP still routes with Link unicast classification present.
    { makeDHCP(f, 68, 67, 1, TEST_XID, 1, 300); route(f,300,L,S,C);
      check("DHCP DISCOVER still -> server only (no Link regression)", !L && S && !C); }
    { makeDHCP(f, 67, 68, 2, TEST_XID, 2, 300); route(f,300,L,S,C);
      check("DHCP OFFER still -> client only (no Link regression)", !L && !S && C); }

    printf(g_fails ? "\n%d FAIL\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}
