// Standalone host test: real Ableton Link Start/Stop ('stst') wire round-trip
// and the local trigger logic added in abletonLink.cpp -- a first loop
// (re)defining the phrase must set isPlaying=true/beat=0/timestamp=downbeat
// and broadcast it; a full erase must re-arm so the NEXT first loop triggers
// it again; a 2nd/3rd loop must NOT re-trigger it.
// Build: g++ -O2 -std=c++17 scripts/test-link-startstop.cpp -o scripts/test-link-startstop.exe
#include "../linkWire.h"
#include <cstdio>

static int g_fails = 0;
static void check(const char* n, bool c){ if(c) printf("ok: %s\n",n); else {printf("FAIL: %s\n",n); g_fails++;} }

// --- wire round-trip: encode a real (non-zero) Start/Stop and decode it back ---
static void test_wire_roundtrip() {
    uint8_t nid[8] = {1,2,3,4,5,6,7,8};
    uint8_t sess[8] = {1,2,3,4,5,6,7,8};
    uint8_t mep[4] = {192,168,4,1};
    uint8_t b[256];
    int64_t downbeatTs = 123456789;
    int n = lwEncodeAlive(b, LW_MSG_ALIVE, 5, 0, nid, sess,
                          500000, 0, 0, mep, LW_PORT,
                          /*isPlaying=*/1, /*beatsMicro=*/0, /*tsMicros=*/downbeatTs);
    LwMessage m;
    bool ok = lwDecode(b, n, &m);
    check("ALIVE with real Start/Stop decodes", ok);
    check("decoded hasStartStop", m.hasStartStop);
    check("decoded isPlaying==1", m.isPlaying == 1);
    check("decoded startStopBeatsMicro==0 (new phrase starts at beat 0)", m.startStopBeatsMicro == 0);
    check("decoded startStopTsMicros matches the downbeat instant", m.startStopTsMicros == downbeatTs);

    // Old hardcoded-zero behavior must still decode fine (backward compat / peer
    // that never sends transport state, or our own not-yet-playing state).
    int n2 = lwEncodeAlive(b, LW_MSG_ALIVE, 5, 0, nid, sess, 500000, 0, 0, mep, LW_PORT);
    LwMessage m2;
    lwDecode(b, n2, &m2);
    check("default (no transport yet) still encodes isPlaying=0", m2.isPlaying == 0);
}

// --- reduced model of the local trigger logic added to abletonLink.cpp/loopClip.cpp ---
struct LinkTransport {
    bool isPlaying = false;
    int64_t startStopBeatsMicro = 0;
    int64_t startStopTsMicros = 0;
    int triggerCount = 0;   // how many times linkEnd's first-loop branch fired

    // Mirrors loopClip::_startEndingRecording's `if (m_masterLoopBlocks == 0)`
    // gate calling linkEnd(), which now also arms Start/Stop.
    void onLoopFinished(bool wasFirstLoop, int64_t downbeatTs) {
        if (!wasFirstLoop) return;   // 2nd/3rd/Nth loop: must NOT re-trigger
        isPlaying = true;
        startStopBeatsMicro = 0;
        startStopTsMicros = downbeatTs;
        triggerCount++;
    }

    // Mirrors linkResetTransport(), called from loopMachine.cpp's erase paths.
    void onFullErase() {
        isPlaying = false;
    }
};

static void test_trigger_logic() {
    LinkTransport t;
    check("initially not playing", !t.isPlaying);

    // First loop recorded (masterLoopBlocks was 0) -> triggers Start/Stop.
    t.onLoopFinished(/*wasFirstLoop=*/true, /*downbeatTs=*/1000);
    check("first loop sets isPlaying=true", t.isPlaying);
    check("first loop sets beat=0", t.startStopBeatsMicro == 0);
    check("first loop records the downbeat timestamp", t.startStopTsMicros == 1000);
    check("first loop triggers exactly once", t.triggerCount == 1);

    // A 2nd loop (masterLoopBlocks already set, aligns to existing grid) must
    // NOT re-trigger -- this is the edge-startstop-during-consecutive-loop-not-first case.
    t.onLoopFinished(/*wasFirstLoop=*/false, /*downbeatTs=*/2000);
    check("2nd loop does not re-trigger Start/Stop", t.triggerCount == 1);
    check("2nd loop does not change the recorded downbeat", t.startStopTsMicros == 1000);

    // Full erase drops back to empty -> transport re-armed to not-playing.
    t.onFullErase();
    check("full erase clears isPlaying", !t.isPlaying);

    // The NEXT loop after erase is a fresh "first loop" again -> must trigger.
    t.onLoopFinished(/*wasFirstLoop=*/true, /*downbeatTs=*/3000);
    check("post-erase first loop re-triggers Start/Stop (re-armed)", t.isPlaying);
    check("post-erase first loop triggers a second time (edge-erase-then-new-first-loop)", t.triggerCount == 2);
    check("post-erase first loop records ITS OWN downbeat, not the stale one", t.startStopTsMicros == 3000);
}

int main() {
    test_wire_roundtrip();
    test_trigger_logic();
    printf(g_fails ? "SOME FAILED\n" : "ALL PASS\n");
    return g_fails ? 1 : 0;
}
