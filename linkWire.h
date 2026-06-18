// linkWire.h — bit-exact Ableton Link wire codec (pure, host-compilable).
//
// LAYER 1 of full Ableton Link support. Depends only on <stdint.h>/<string.h>
// (no Circle) so the SAME code compiles into the firmware AND the host test
// (scripts/test-link-wire.cpp) — a host-validated codec is only meaningful if
// the firmware uses the identical bytes.
//
// Protocol (from the Ableton/link reference impl, master):
//  - Discovery: UDP multicast 224.76.78.75:20808. 8-byte protocol header
//    "_asdp_v\x01", then MessageHeader { u8 messageType; u8 ttl; u16 groupId;
//    u8 nodeId[8] } (all multi-byte fields BIG-ENDIAN), then a Payload of
//    entries. messageType: ALIVE=1, RESPONSE=2, BYEBYE=3.
//  - Measurement: UNICAST to a peer's mep4 endpoint. SAME MessageHeader shape
//    but protocol header "_link_v\x01"; messageType PING=1, PONG=2.
//  - Payload entry = key(u32 BE) + size(u32 BE) + value. Keys (4 ASCII chars,
//    big-endian u32): 'tmln' timeline, 'sess' session membership, 'mep4' IPv4
//    measurement endpoint, '__ht'/'__gt'/'_pgt' host/ghost/prev-ghost time.
//  - Integers big-endian. chrono::microseconds -> int64 BE.
//
// WIRE FORMAT CONFIRMED against (a) the Ableton/link reference source, (b) a
// Wireshark dissector of REAL Ableton Live captures (westhom/AbletonLinkProtocol),
// and (c) the from-scratch Rust reimpl (anweiss/ableton-link-rs). The discovery
// ALIVE byte offsets match the live capture exactly: off28 microsPerBeat(i64),
// off36 beatOrigin as MICROBEATS (beats*1e6, i64), off44 timeOrigin micros(i64),
// off52 'sess' key, off60 sessionId — all big-endian (htonll). IPv4 = 4 octets
// as-is; u16 port = 2 bytes BE. The ONE residual micro-uncertainty is the
// addr-vs-port ORDER inside the 'mep4' value (we emit addr(4) then port(2),
// the asio-endpoint-standard order); trivially confirmed at the first capture.
// NOTE on semantics (integration layer, not codec): beatOrigin must be passed as
// MICROBEATS, and tempo as microsPerBeat (60e6/bpm).

#ifndef _linkWire_h
#define _linkWire_h

#include <stdint.h>
#include <string.h>

// ---- protocol headers ----
static const uint8_t LW_HDR_ASDP[8] = {'_','a','s','d','p','_','v',1}; // discovery
static const uint8_t LW_HDR_LINK[8] = {'_','l','i','n','k','_','v',1}; // measurement

// ---- message types ----
enum {
    LW_MSG_INVALID  = 0,
    LW_MSG_ALIVE    = 1,   // discovery
    LW_MSG_RESPONSE = 2,   // discovery (reply to ALIVE)
    LW_MSG_BYEBYE   = 3,   // discovery (leaving)
};
enum {
    LW_MSG_PING = 1,       // measurement (over _link_v)
    LW_MSG_PONG = 2,       // measurement (over _link_v)
};

// ---- payload entry keys (4 ASCII chars as a big-endian u32) ----
#define LW_FOURCC(a,b,c,d) ((uint32_t)((uint32_t)(a)<<24|(uint32_t)(b)<<16|(uint32_t)(c)<<8|(uint32_t)(d)))
static const uint32_t LW_KEY_TMLN = LW_FOURCC('t','m','l','n'); // 0x746d6c6e
static const uint32_t LW_KEY_SESS = LW_FOURCC('s','e','s','s'); // 0x73657373
static const uint32_t LW_KEY_MEP4 = LW_FOURCC('m','e','p','4'); // 0x6d657034
static const uint32_t LW_KEY_STST = LW_FOURCC('s','t','s','t'); // 0x73747374 StartStopState
static const uint32_t LW_KEY_HSTT = LW_FOURCC('_','_','h','t'); // 0x5f5f6874
static const uint32_t LW_KEY_GHST = LW_FOURCC('_','_','g','t'); // 0x5f5f6774
static const uint32_t LW_KEY_PRGH = LW_FOURCC('_','p','g','t'); // 0x5f706774

#define LW_HEADER_LEN 20   // 8 (proto) + 1 type + 1 ttl + 2 groupId + 8 nodeId
#define LW_ENTRY_HDR  8    // key(4) + size(4)
#define LW_PORT       20808

// ---- big-endian put/get ----
static inline int lwPut16(uint8_t *b, int o, uint16_t v){ b[o]=(uint8_t)(v>>8); b[o+1]=(uint8_t)v; return o+2; }
static inline int lwPut32(uint8_t *b, int o, uint32_t v){ b[o]=(uint8_t)(v>>24); b[o+1]=(uint8_t)(v>>16); b[o+2]=(uint8_t)(v>>8); b[o+3]=(uint8_t)v; return o+4; }
static inline int lwPut64(uint8_t *b, int o, uint64_t v){ for(int i=0;i<8;i++) b[o+i]=(uint8_t)(v>>(56-8*i)); return o+8; }
static inline uint16_t lwGet16(const uint8_t *b, int o){ return (uint16_t)((b[o]<<8)|b[o+1]); }
static inline uint32_t lwGet32(const uint8_t *b, int o){ return ((uint32_t)b[o]<<24)|((uint32_t)b[o+1]<<16)|((uint32_t)b[o+2]<<8)|b[o+3]; }
static inline uint64_t lwGet64(const uint8_t *b, int o){ uint64_t v=0; for(int i=0;i<8;i++) v=(v<<8)|b[o+i]; return v; }

// ---- message header ----
// Writes the 20-byte prefix (proto header + MessageHeader). Returns next offset.
static inline int lwWriteHeader(uint8_t *b, const uint8_t proto[8],
                                uint8_t msgType, uint8_t ttl, uint16_t groupId,
                                const uint8_t nodeId[8])
{
    memcpy(b, proto, 8);
    b[8] = msgType;
    b[9] = ttl;
    int o = lwPut16(b, 10, groupId);
    memcpy(b + o, nodeId, 8);
    return o + 8;            // == LW_HEADER_LEN
}

// ---- payload entries ----
static inline int lwAppendEntry(uint8_t *b, int o, uint32_t key, const uint8_t *val, uint32_t vlen)
{
    o = lwPut32(b, o, key);
    o = lwPut32(b, o, vlen);
    if (vlen) memcpy(b + o, val, vlen);
    return o + (int)vlen;
}
static inline int lwAppendU64Entry(uint8_t *b, int o, uint32_t key, uint64_t v)
{
    uint8_t tmp[8]; lwPut64(tmp, 0, v);
    return lwAppendEntry(b, o, key, tmp, 8);
}
// Timeline value = microsPerBeat(i64) + beatOrigin(i64) + timeOrigin(i64), BE.
static inline int lwAppendTimeline(uint8_t *b, int o, int64_t mpb, int64_t beatOrigin, int64_t timeOrigin)
{
    uint8_t v[24];
    lwPut64(v, 0,  (uint64_t)mpb);
    lwPut64(v, 8,  (uint64_t)beatOrigin);
    lwPut64(v, 16, (uint64_t)timeOrigin);
    return lwAppendEntry(b, o, LW_KEY_TMLN, v, 24);
}
// SessionMembership value = sessionId (NodeId, 8 bytes).
static inline int lwAppendSession(uint8_t *b, int o, const uint8_t sessionId[8])
{
    return lwAppendEntry(b, o, LW_KEY_SESS, sessionId, 8);
}
// mep4 value = IPv4 addr (4 bytes, network order) + UDP port (2 bytes BE).
static inline int lwAppendMep4(uint8_t *b, int o, const uint8_t addr[4], uint16_t port)
{
    uint8_t v[6]; memcpy(v, addr, 4); lwPut16(v, 4, port);
    return lwAppendEntry(b, o, LW_KEY_MEP4, v, 6);
}
// StartStopState value = isPlaying(u8) + beats(i64 microbeats BE) + timestamp(i64
// micros BE) = 17 bytes. CONFIRMED present in real Ableton Live ALIVE frames
// (key 'stst', between 'sess' and 'mep4'); Live's NodeState parser requires it,
// so omitting it made Live discard our ALIVE entirely (one-way discovery). The
// looper has no transport start/stop yet, so we send all-zero (matches Live's
// observed all-zero stst byte-for-byte).
static inline int lwAppendStartStop(uint8_t *b, int o, uint8_t isPlaying,
                                    int64_t beatsMicro, int64_t tsMicros)
{
    uint8_t v[17];
    v[0] = isPlaying;
    lwPut64(v, 1, (uint64_t)beatsMicro);
    lwPut64(v, 9, (uint64_t)tsMicros);
    return lwAppendEntry(b, o, LW_KEY_STST, v, 17);
}

// ---- whole-message encoders ----
// Discovery ALIVE/RESPONSE: timeline + session + mep4.
static inline int lwEncodeAlive(uint8_t *b, uint8_t msgType, uint8_t ttl, uint16_t groupId,
                                const uint8_t nodeId[8], const uint8_t sessionId[8],
                                int64_t mpb, int64_t beatOrigin, int64_t timeOrigin,
                                const uint8_t mep4Addr[4], uint16_t mep4Port)
{
    int o = lwWriteHeader(b, LW_HDR_ASDP, msgType, ttl, groupId, nodeId);
    o = lwAppendTimeline(b, o, mpb, beatOrigin, timeOrigin);
    o = lwAppendSession(b, o, sessionId);
    o = lwAppendStartStop(b, o, 0, 0, 0);   // required by Live's NodeState parser
    o = lwAppendMep4(b, o, mep4Addr, mep4Port);
    return o;
}
// Discovery BYEBYE: header only (no payload).
static inline int lwEncodeByeBye(uint8_t *b, uint8_t ttl, uint16_t groupId, const uint8_t nodeId[8])
{
    return lwWriteHeader(b, LW_HDR_ASDP, LW_MSG_BYEBYE, ttl, groupId, nodeId);
}
// Measurement PING: host time only.
static inline int lwEncodePing(uint8_t *b, uint8_t ttl, uint16_t groupId,
                               const uint8_t nodeId[8], int64_t hostTimeMicros)
{
    int o = lwWriteHeader(b, LW_HDR_LINK, LW_MSG_PING, ttl, groupId, nodeId);
    return lwAppendU64Entry(b, o, LW_KEY_HSTT, (uint64_t)hostTimeMicros);
}
// Measurement PONG: session + ghost + prevGhost + echoed host time.
static inline int lwEncodePong(uint8_t *b, uint8_t ttl, uint16_t groupId,
                               const uint8_t nodeId[8], const uint8_t sessionId[8],
                               int64_t ghostMicros, int64_t prevGhostMicros, int64_t echoedHostMicros)
{
    int o = lwWriteHeader(b, LW_HDR_LINK, LW_MSG_PONG, ttl, groupId, nodeId);
    o = lwAppendSession(b, o, sessionId);
    o = lwAppendU64Entry(b, o, LW_KEY_GHST, (uint64_t)ghostMicros);
    o = lwAppendU64Entry(b, o, LW_KEY_PRGH, (uint64_t)prevGhostMicros);
    o = lwAppendU64Entry(b, o, LW_KEY_HSTT, (uint64_t)echoedHostMicros);
    return o;
}

// ---- decoder ----
typedef struct {
    bool     valid;
    bool     isLink;       // true: _link_v (measurement); false: _asdp_v (discovery)
    uint8_t  msgType;
    uint8_t  ttl;
    uint16_t groupId;
    uint8_t  nodeId[8];
    // present-flags + values for the entries we care about
    bool     hasTimeline; int64_t mpb, beatOrigin, timeOrigin;
    bool     hasSession;  uint8_t sessionId[8];
    bool     hasMep4;     uint8_t mep4Addr[4]; uint16_t mep4Port;
    bool     hasHost;     int64_t hostTime;
    bool     hasGhost;    int64_t ghostTime;
    bool     hasPrevGhost;int64_t prevGhostTime;
} LwMessage;

// Parse a full UDP payload (the bytes after the UDP header). Returns false if it
// is not a valid Link message. Tolerates unknown entries (skips by size).
static inline bool lwDecode(const uint8_t *b, int len, LwMessage *m)
{
    memset(m, 0, sizeof(*m));
    if (len < LW_HEADER_LEN) return false;
    if (memcmp(b, LW_HDR_ASDP, 8) == 0)      m->isLink = false;
    else if (memcmp(b, LW_HDR_LINK, 8) == 0) m->isLink = true;
    else return false;
    m->msgType = b[8];
    m->ttl     = b[9];
    m->groupId = lwGet16(b, 10);
    memcpy(m->nodeId, b + 12, 8);

    int o = LW_HEADER_LEN;
    while (o + LW_ENTRY_HDR <= len)
    {
        uint32_t key = lwGet32(b, o);
        uint32_t sz  = lwGet32(b, o + 4);
        int vo = o + LW_ENTRY_HDR;
        if (vo + (int)sz > len) break;            // truncated entry
        if (key == LW_KEY_TMLN && sz >= 24) {
            m->hasTimeline = true;
            m->mpb        = (int64_t)lwGet64(b, vo);
            m->beatOrigin = (int64_t)lwGet64(b, vo + 8);
            m->timeOrigin = (int64_t)lwGet64(b, vo + 16);
        } else if (key == LW_KEY_SESS && sz >= 8) {
            m->hasSession = true; memcpy(m->sessionId, b + vo, 8);
        } else if (key == LW_KEY_MEP4 && sz >= 6) {
            m->hasMep4 = true; memcpy(m->mep4Addr, b + vo, 4); m->mep4Port = lwGet16(b, vo + 4);
        } else if (key == LW_KEY_HSTT && sz >= 8) {
            m->hasHost = true; m->hostTime = (int64_t)lwGet64(b, vo);
        } else if (key == LW_KEY_GHST && sz >= 8) {
            m->hasGhost = true; m->ghostTime = (int64_t)lwGet64(b, vo);
        } else if (key == LW_KEY_PRGH && sz >= 8) {
            m->hasPrevGhost = true; m->prevGhostTime = (int64_t)lwGet64(b, vo);
        }
        o = vo + (int)sz;
    }
    m->valid = true;
    return true;
}

// ---- measurement sample math (Measurement.hpp) ----
// Per successful ping/pong exchange the initiator derives up to two offset
// samples; the session offset = median of all samples (GhostXForm slope is 1).
//   s1 = ghost - (host + prevHost)/2                 (when ghost,prevHost != 0)
//   s2 = (ghost + prevGhost)/2 - prevHost            (when prevGhost != 0)
// Returns the number of samples written (0..2).
static inline int lwMeasurementSamples(int64_t host, int64_t prevHost,
                                       int64_t ghost, int64_t prevGhost,
                                       int64_t out[2])
{
    int n = 0;
    if (ghost != 0 && prevHost != 0)
        out[n++] = ghost - (host + prevHost) / 2;
    if (prevGhost != 0 && prevHost != 0)
        out[n++] = (ghost + prevGhost) / 2 - prevHost;
    return n;
}

#endif
