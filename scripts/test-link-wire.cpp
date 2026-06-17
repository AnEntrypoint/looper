// Bit-exact Ableton Link wire codec tests (linkWire.h). Layer 1 of full Link.
// The codec compiles into BOTH firmware and this test, so a green run here means
// the firmware emits/parses the same bytes. Validates framing certainty (header,
// keys, entry sizing, int64 BE, encode<->decode round-trip) and the measurement
// sample math. The two capture-dependent value layouts (mep4 endpoint bytes,
// timeline beat unit) are exercised for round-trip but flagged in linkWire.h for
// confirmation against a real Ableton Live packet capture.
//
// Build: g++ -O2 -std=c++17 scripts/test-link-wire.cpp -o scripts/test-link-wire.exe
#include <cstdio>
#include <cstring>
#include <cstdint>
#include "../linkWire.h"

static int g_fails = 0;
static void check(const char* n, bool c){ if(c)printf("ok: %s\n",n); else {printf("FAIL: %s\n",n);g_fails++;} }

int main()
{
    const uint8_t node[8]   = {1,2,3,4,5,6,7,8};
    const uint8_t sess[8]   = {9,10,11,12,13,14,15,16};
    const uint8_t mep[4]    = {192,168,4,1};
    uint8_t b[512];

    // ---- key constants ----
    check("tmln key == 0x746d6c6e", LW_KEY_TMLN == 0x746d6c6eu);
    check("sess key == 0x73657373", LW_KEY_SESS == 0x73657373u);
    check("mep4 key == 0x6d657034 (NOT old mmbe 0x6d6d6265)",
          LW_KEY_MEP4 == 0x6d657034u && LW_KEY_MEP4 != 0x6d6d6265u);
    check("__ht key == 0x5f5f6874", LW_KEY_HSTT == 0x5f5f6874u);
    check("__gt key == 0x5f5f6774", LW_KEY_GHST == 0x5f5f6774u);
    check("_pgt key == 0x5f706774", LW_KEY_PRGH == 0x5f706774u);

    // ---- ALIVE: header bytes + structure ----
    {
        int n = lwEncodeAlive(b, LW_MSG_ALIVE, 5, 0, node, sess,
                              500000 /*mpb=120bpm*/, 7 /*beatOrigin*/, 123456 /*timeOrigin*/,
                              mep, LW_PORT);
        check("ALIVE proto header == _asdp_v\\x01", memcmp(b, LW_HDR_ASDP, 8) == 0);
        check("ALIVE msgType byte == 1 (ALIVE)", b[8] == LW_MSG_ALIVE);
        check("ALIVE ttl byte == 5", b[9] == 5);
        check("ALIVE groupId BE == 0", lwGet16(b,10) == 0);
        check("ALIVE nodeId at offset 12", memcmp(b+12, node, 8) == 0);
        // first entry must be tmln at offset 20
        check("ALIVE first entry key == tmln at off 20", lwGet32(b,20) == LW_KEY_TMLN);
        check("ALIVE tmln size == 24",                   lwGet32(b,24) == 24);
        check("ALIVE tmln mpb BE == 500000",   (int64_t)lwGet64(b,28) == 500000);
        check("ALIVE total length == 20 + (8+24)+(8+8)+(8+6) = 82", n == 82);

        LwMessage m;
        check("ALIVE decodes", lwDecode(b, n, &m) && m.valid && !m.isLink);
        check("ALIVE decode msgType", m.msgType == LW_MSG_ALIVE);
        check("ALIVE decode nodeId", memcmp(m.nodeId, node, 8) == 0);
        check("ALIVE decode timeline", m.hasTimeline && m.mpb==500000 && m.beatOrigin==7 && m.timeOrigin==123456);
        check("ALIVE decode session", m.hasSession && memcmp(m.sessionId, sess, 8)==0);
        check("ALIVE decode mep4 addr+port", m.hasMep4 && memcmp(m.mep4Addr, mep, 4)==0 && m.mep4Port==LW_PORT);
    }

    // ---- RESPONSE: same shape, type 2 ----
    {
        int n = lwEncodeAlive(b, LW_MSG_RESPONSE, 5, 0, node, sess, 500000, 0, 0, mep, LW_PORT);
        LwMessage m; lwDecode(b, n, &m);
        check("RESPONSE msgType == 2", m.msgType == LW_MSG_RESPONSE && !m.isLink);
    }

    // ---- BYEBYE: header only ----
    {
        int n = lwEncodeByeBye(b, 5, 0, node);
        check("BYEBYE length == header only (20)", n == LW_HEADER_LEN);
        LwMessage m; check("BYEBYE decodes type 3", lwDecode(b,n,&m) && m.msgType==LW_MSG_BYEBYE && !m.hasTimeline);
    }

    // ---- PING: _link_v header + __ht ----
    {
        int n = lwEncodePing(b, 5, 0, node, 111222333);
        check("PING proto header == _link_v\\x01", memcmp(b, LW_HDR_LINK, 8) == 0);
        check("PING msgType == 1 (PING)", b[8] == LW_MSG_PING);
        check("PING first entry key == __ht", lwGet32(b,20) == LW_KEY_HSTT);
        LwMessage m;
        check("PING decodes as link msg", lwDecode(b,n,&m) && m.isLink && m.msgType==LW_MSG_PING);
        check("PING decode hostTime", m.hasHost && m.hostTime==111222333 && !m.hasGhost);
    }

    // ---- PONG: session + ghost + prevGhost + echoed host ----
    {
        int n = lwEncodePong(b, 5, 0, node, sess, 555000, 554000, 111222333);
        check("PONG proto header == _link_v", memcmp(b, LW_HDR_LINK, 8) == 0);
        check("PONG msgType == 2 (PONG)", b[8] == LW_MSG_PONG);
        LwMessage m; lwDecode(b, n, &m);
        check("PONG decode session", m.isLink && m.hasSession && memcmp(m.sessionId, sess, 8)==0);
        check("PONG decode ghost/prevGhost/echoedHost",
              m.hasGhost && m.ghostTime==555000 &&
              m.hasPrevGhost && m.prevGhostTime==554000 &&
              m.hasHost && m.hostTime==111222333);
    }

    // ---- reject garbage / foreign frames ----
    {
        LwMessage m;
        uint8_t junk[20]; memset(junk, 0xAB, sizeof junk);
        check("garbage header rejected", !lwDecode(junk, 20, &m));
        check("too-short rejected", !lwDecode(b, 5, &m));
    }

    // ---- unknown entries tolerated (forward-compat): inject aep4 then tmln ----
    {
        int o = lwWriteHeader(b, LW_HDR_ASDP, LW_MSG_ALIVE, 5, 0, node);
        uint8_t junkv[6] = {1,1,1,1,1,1};
        o = lwAppendEntry(b, o, LW_FOURCC('a','e','p','4'), junkv, 6); // unknown audio endpoint
        o = lwAppendTimeline(b, o, 600000, 0, 0);
        LwMessage m;
        check("decoder skips unknown entry and still finds tmln after it",
              lwDecode(b, o, &m) && m.hasTimeline && m.mpb==600000);
    }

    // ---- measurement sample math (Measurement.hpp) ----
    {
        int64_t out[2];
        // host=1000 prevHost=900 ghost=5000 prevGhost=4900
        int k = lwMeasurementSamples(1000, 900, 5000, 4900, out);
        check("two samples when all four times present", k == 2);
        check("sample1 = ghost-(host+prevHost)/2 = 5000-950 = 4050", out[0] == 4050);
        check("sample2 = (ghost+prevGhost)/2-prevHost = 4950-900 = 4050", out[1] == 4050);
        // first ping (no prevGhost): only sample1
        k = lwMeasurementSamples(1000, 900, 5000, 0, out);
        check("one sample when prevGhost==0", k == 1 && out[0]==4050);
    }

    printf(g_fails ? "\n%d FAIL\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}
