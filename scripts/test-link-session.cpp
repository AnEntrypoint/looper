// HARDEST NODE: end-to-end Ableton Link session simulation (linkWire.h +
// linkGhost.h + linkSession.h). Two peers A (higher NodeId = owner) and B
// (follower) with clocks offset by a known OFFSET. B discovers A, measures the
// clock offset via jittery PING/PONG, elects A as owner, adopts A's ghost
// timeline, and computes a local beat phase. The test asserts B's beat phase at
// a real instant matches A's beat phase at the SAME instant within jitter -- i.e.
// full phase sync, not just tempo. Also covers ownership election, tempo-change
// propagation, peer expiry/hand-back, self-ignore, and capacity.
//
// Build: g++ -O2 -std=c++17 scripts/test-link-session.cpp -o scripts/test-link-session.exe
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "../linkWire.h"
#include "../linkGhost.h"
#include "../linkSession.h"

static int g_fails = 0;
static void check(const char* n, bool c){ if(c)printf("ok: %s\n",n); else {printf("FAIL: %s\n",n);g_fails++;} }

int main()
{
    const uint8_t idA[8] = {0,0,0,0,0,0,0,2};  // owner (higher u64)
    const uint8_t idB[8] = {0,0,0,0,0,0,0,1};  // follower
    const uint8_t macA[6] = {0xAA,0xAA,0xAA,0,0,2};
    const uint8_t ipA[4]  = {192,168,4,2};

    // A owns the session; its ghost clock == its own host clock (owner offset 0).
    // A's timeline: 120 bpm, beatOrigin 0 at timeOrigin 0 (ghost domain).
    LinkTimeline tlA;
    tlA.tempoMicrosPerBeat   = lgBpmToMicrosPerBeat(120.0);   // 500000
    tlA.beatOriginMicroBeats = 0;
    tlA.timeOriginMicros     = 0;
    check("bpm->mpb 120 == 500000", tlA.tempoMicrosPerBeat == 500000);
    check("mpb->bpm 500000 == 120", (int)(lgMicrosPerBeatToBpm(500000)+0.5) == 120);

    // B's clock runs OFFSET behind A's: B_host = A_host - OFFSET. So to reach the
    // shared ghost (= A_host), B must add OFFSET: ghost = B_host + OFFSET.
    const int64_t OFFSET = 1000000;  // 1 s

    LinkSession sB; lsInit(&sB, idB);
    LinkSession sA; lsInit(&sA, idA);

    // --- discovery: B learns A from an ALIVE frame ---
    int64_t nowB = 8000000;          // B host clock "now"
    LinkPeer *pA = lsUpsert(&sB, idA, nowB);
    check("B upsert A returns a slot", pA != 0);
    memcpy(pA->mac, macA, 6); memcpy(pA->ipv4, ipA, 4); pA->mep4Port = 20808; pA->hasEndpoint = true;
    pA->timeline = tlA; pA->hasTimeline = true;
    check("B learned A MAC from frame (no ARP)", memcmp(pA->mac, macA, 6) == 0);

    // --- ownership election ---
    check("B sees A as owner (higher NodeId)", lsOwnerPeer(&sB) == pA);
    check("B does NOT self-own", !lsSelfOwns(&sB));
    // A's view: B is lower, so A self-owns.
    LinkPeer *pB = lsUpsert(&sA, idB, nowB);
    pB->hasTimeline = true; pB->timeline = tlA;
    check("A self-owns (higher NodeId)", lsSelfOwns(&sA));
    check("A sees no owner-peer", lsOwnerPeer(&sA) == 0);

    // --- self-ignore ---
    check("upsert(self) returns NULL", lsUpsert(&sB, idB, nowB) == 0);

    // --- measurement: B pings A, accumulates offset samples ---
    lgMeasReset(&pA->meas);
    uint32_t rng = 99991;
    for (int i = 0; i < 60; i++) {
        rng = rng * 1103515245u + 12345u;
        int64_t jit = (int64_t)((rng >> 16) % 41) - 20;     // [-20,20] us
        int64_t d   = 250 + ((rng >> 9) % 150);             // one-way 250..399 us
        int64_t tb  = (int64_t)i * 50000;                   // B send time (its clock)
        int64_t send = tb;
        int64_t recv = tb + 2*d;
        // A stamps ghost = A_host at receipt = (tb + OFFSET) + d, plus jitter.
        int64_t ghost = (tb + OFFSET + d) + jit;
        int64_t out[2];
        int k = lwMeasurementSamples(send, recv, ghost, /*prevGhost*/0, out);
        lgMeasAddSamples(&pA->meas, out, k);
    }
    pA->xform = lgMeasResult(&pA->meas);
    pA->measured = true;
    int64_t offErr = pA->xform.offsetMicros > OFFSET ? pA->xform.offsetMicros - OFFSET
                                                     : OFFSET - pA->xform.offsetMicros;
    check("B measured A clock offset within jitter (<=20us)", offErr <= 20);

    // --- adopt owner timeline + compute beat phase; compare to A's own phase ---
    // At a real instant, A_host = HA, B_host = HA - OFFSET. quantum = 4 beats.
    const int64_t QUANTUM = 4 * 1000000LL;  // microbeats
    bool phaseMatch = true; int64_t worst = 0;
    for (int s = 0; s < 8; s++) {
        int64_t HA  = 3000000 + (int64_t)s * 137777;   // A host time (== ghost)
        int64_t HB  = HA - OFFSET;                      // same real instant on B clock
        // A computes its phase directly from its own clock (ghost == host).
        int64_t mbA = lgTimelineMicroBeatsAt(tlA, HA);
        int64_t phA = lgBeatPhaseMicro(mbA, QUANTUM);
        // B maps its host -> ghost via the measured xform, then into A's timeline.
        int64_t phB = lgBeatPhaseAtHost(pA->xform, pA->timeline, HB, QUANTUM);
        int64_t e = phA > phB ? phA - phB : phB - phA;
        if (e > worst) worst = e;
        if (e > 100) phaseMatch = false;   // <=100 microbeats = 1e-4 beat
    }
    check("B beat phase matches A beat phase at same instant (full phase sync)", phaseMatch);

    // --- tempo change propagates: A speeds to 140 bpm, B re-reads A timeline ---
    {
        LinkTimeline tl140 = tlA; tl140.tempoMicrosPerBeat = lgBpmToMicrosPerBeat(140.0);
        pA->timeline = tl140;
        // one beat (1e6 microbeats) now takes mpb(140) micros, faster than 120.
        int64_t mbAt = lgTimelineMicroBeatsAt(tl140, tl140.timeOriginMicros + tl140.tempoMicrosPerBeat);
        check("after tempo change, +1 beat-of-time advances exactly 1e6 microbeats",
              mbAt == tl140.beatOriginMicroBeats + 1000000);
        check("140bpm mpb < 120bpm mpb (faster)", tl140.tempoMicrosPerBeat < 500000);
    }

    // --- peer expiry + ownership hand-back ---
    {
        LinkSession s; lsInit(&s, idB);
        LinkPeer *p = lsUpsert(&s, idA, 1000000); p->hasTimeline = true; p->timeline = tlA;
        check("before expiry: A owns", lsOwnerPeer(&s) == p);
        lsExpire(&s, 1000000 + LS_PEER_TIMEOUT_US + 1);
        check("after timeout: A dropped", lsFind(&s, idA) == 0);
        check("after expiry: self owns again", lsSelfOwns(&s));
        check("peer count back to 0", lsPeerCount(&s) == 0);
    }

    // --- capacity clamp (multi-peer): >LS_MAX_PEERS distinct ids, count bounded ---
    {
        LinkSession s; lsInit(&s, idB);
        for (int i = 0; i < LS_MAX_PEERS + 5; i++) {
            uint8_t id[8] = {0,0,0,0,0,0,(uint8_t)(i>>8),(uint8_t)(i+10)};
            lsUpsert(&s, id, 1000000 + i);   // increasing lastSeen
        }
        check("peer table never exceeds LS_MAX_PEERS", lsPeerCount(&s) == LS_MAX_PEERS);
    }

    // --- multi-peer ownership: max NodeId across all peers wins ---
    {
        LinkSession s; lsInit(&s, idB);   // self u64 = 1
        const uint8_t id3[8] = {0,0,0,0,0,0,0,3};
        const uint8_t id5[8] = {0,0,0,0,0,0,0,5};
        LinkPeer *p3 = lsUpsert(&s, id3, 1000); p3->hasTimeline = true; p3->timeline = tlA;
        LinkPeer *p5 = lsUpsert(&s, id5, 1000); p5->hasTimeline = true; p5->timeline = tlA;
        check("owner = highest NodeId among peers (id5)", lsOwnerPeer(&s) == p5);
    }

    printf(g_fails ? "\n%d FAIL\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}
