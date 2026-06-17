// linkSession.h — Ableton Link peer table + session/timeline ownership.
// Pure (stdint only, no Circle); shared by firmware + host tests.
//
// A peer is learned from received frames: its NodeId (from the message header),
// its MAC (the Ethernet SOURCE of the frame — so AP-mode unicast needs NO ARP),
// its IPv4 + measurement port (from the 'mep4' TLV), and its timeline (from
// 'tmln'). Session ownership = the highest NodeId across self + all peers (Link's
// deterministic rule); the owner's timeline is the one everyone adopts. Each peer
// also carries its own measurement accumulator + resulting GhostXForm (the
// host<->that-peer's-ghost offset), so a follower can map the owner's ghost
// timeline into its own host clock.

#ifndef _linkSession_h
#define _linkSession_h

#include <stdint.h>
#include <string.h>
#include "linkWire.h"
#include "linkGhost.h"

#define LS_MAX_PEERS        8
#define LS_PEER_TIMEOUT_US  5000000   // drop a peer after 5s without an ALIVE (~5 missed)

typedef struct {
    bool         used;
    uint8_t      nodeId[8];
    uint8_t      mac[6];          // ethernet source of the last frame from this peer
    uint8_t      ipv4[4];
    uint16_t     mep4Port;
    bool         hasEndpoint;     // mep4 learned
    LinkTimeline timeline;
    bool         hasTimeline;
    int64_t      lastSeenMicros;
    // measurement state for THIS peer
    LinkMeasurement meas;
    LinkGhostXForm  xform;        // host->ghost offset to this peer (valid iff measured)
    bool         measured;
    int64_t      prevGhostSent;   // responder: ghost time of the last PONG we sent this peer
} LinkPeer;

typedef struct {
    LinkPeer peers[LS_MAX_PEERS];
    uint8_t  selfNodeId[8];
} LinkSession;

// NodeId is compared as a big-endian u64 (its wire order).
static inline uint64_t lsNodeU64(const uint8_t id[8])
{
    uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | id[i]; return v;
}
static inline bool lsNodeEq(const uint8_t a[8], const uint8_t b[8]) { return memcmp(a, b, 8) == 0; }

static inline void lsInit(LinkSession *s, const uint8_t self[8])
{
    memset(s, 0, sizeof(*s));
    memcpy(s->selfNodeId, self, 8);
}

static inline LinkPeer *lsFind(LinkSession *s, const uint8_t id[8])
{
    for (int i = 0; i < LS_MAX_PEERS; i++)
        if (s->peers[i].used && lsNodeEq(s->peers[i].nodeId, id)) return &s->peers[i];
    return 0;
}

// Insert-or-find a peer slot. Returns NULL if id == self (never peer with self).
// If the table is full, evicts the oldest (lowest lastSeenMicros) slot.
static inline LinkPeer *lsUpsert(LinkSession *s, const uint8_t id[8], int64_t nowMicros)
{
    if (lsNodeEq(id, s->selfNodeId)) return 0;
    LinkPeer *p = lsFind(s, id);
    if (p) { p->lastSeenMicros = nowMicros; return p; }
    // free slot?
    for (int i = 0; i < LS_MAX_PEERS; i++) {
        if (!s->peers[i].used) {
            p = &s->peers[i];
            memset(p, 0, sizeof(*p));
            p->used = true; memcpy(p->nodeId, id, 8);
            lgMeasReset(&p->meas);
            p->lastSeenMicros = nowMicros;
            return p;
        }
    }
    // full: evict oldest
    int oldest = 0;
    for (int i = 1; i < LS_MAX_PEERS; i++)
        if (s->peers[i].lastSeenMicros < s->peers[oldest].lastSeenMicros) oldest = i;
    p = &s->peers[oldest];
    memset(p, 0, sizeof(*p));
    p->used = true; memcpy(p->nodeId, id, 8);
    lgMeasReset(&p->meas);
    p->lastSeenMicros = nowMicros;
    return p;
}

// Drop peers not seen within LS_PEER_TIMEOUT_US.
static inline void lsExpire(LinkSession *s, int64_t nowMicros)
{
    for (int i = 0; i < LS_MAX_PEERS; i++)
        if (s->peers[i].used && (nowMicros - s->peers[i].lastSeenMicros) > LS_PEER_TIMEOUT_US)
            s->peers[i].used = false;
}

// The peer that owns the session (highest NodeId) IF a peer outranks self; else
// NULL (self owns). Only peers with a known timeline can own (we cannot adopt a
// timeline we have not heard).
static inline LinkPeer *lsOwnerPeer(LinkSession *s)
{
    LinkPeer *owner = 0;
    uint64_t selfU = lsNodeU64(s->selfNodeId);
    uint64_t bestU = selfU;
    for (int i = 0; i < LS_MAX_PEERS; i++) {
        if (!s->peers[i].used || !s->peers[i].hasTimeline) continue;
        uint64_t u = lsNodeU64(s->peers[i].nodeId);
        if (u > bestU) { bestU = u; owner = &s->peers[i]; }
    }
    return owner;   // NULL => self owns (or no peer with a timeline outranks us)
}

static inline bool lsSelfOwns(LinkSession *s) { return lsOwnerPeer(s) == 0; }

static inline int lsPeerCount(const LinkSession *s)
{
    int n = 0; for (int i = 0; i < LS_MAX_PEERS; i++) if (s->peers[i].used) n++; return n;
}

#endif
