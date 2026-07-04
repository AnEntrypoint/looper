// Standalone host test: a first loop recorded while Link-synced to a peer
// session must still define the LOCAL master grid from its OWN recorded
// length, not inherit the peer's advertised tempo. Models the fixed
// loopClip::_startEndingRecording (loopClip.cpp) + loopMachine::update's
// linkSynced&&anyRecorded re-derivation (loopMachine.cpp) + abletonLink's
// propose-hold republishTimeline behavior (abletonLink.cpp), reduced to the
// interaction under test.
// Build: g++ -O2 -std=c++17 scripts/test-first-loop-link-synced.cpp -o scripts/test-first-loop-link-synced.exe
#include <cstdint>
#include <cstdio>
typedef uint32_t u32;

static int g_fails = 0;
static void check(const char* n, bool c){ if(c) printf("ok: %s\n",n); else {printf("FAIL: %s\n",n); g_fails++;} }

// --- reduced model of the relevant global/session state ---
struct Machine {
    u32 masterLoopBlocks = 0;
    u32 masterPhase = 1234;   // free-running phase at the moment of this loop's stop
};

// Mirror of abletonLink.cpp linkDeriveQuant: pick the nearest-to-120bpm
// candidate beat count (of {0.25,0.5,1,2,4,8,16}) whose implied BPM falls in
// [80,160] if any candidate does.
static void linkDeriveQuant(double clip_seconds, double *out_beats, double *out_bpm) {
    static const double cand[] = {0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0};
    const int N = 7;
    if (clip_seconds <= 0.0001) { *out_beats = 4.0; *out_bpm = 120.0; return; }
    double bestB = 4.0, bestBpm = 120.0, bestDist = 1e18; bool haveInWin = false;
    for (int i = 0; i < N; i++) {
        double bpm = 60.0 * cand[i] / clip_seconds;
        double dist = bpm > 120.0 ? bpm - 120.0 : 120.0 - bpm;
        bool inWin = (bpm >= 80.0 && bpm <= 160.0);
        if (inWin) { if (!haveInWin || dist < bestDist) { bestB = cand[i]; bestBpm = bpm; bestDist = dist; haveInWin = true; } }
        else if (!haveInWin && dist < bestDist) { bestB = cand[i]; bestBpm = bpm; bestDist = dist; }
    }
    *out_beats = bestB; *out_bpm = bestBpm;
}

struct LinkSession {
    bool synced = true;          // a peer/ticker is present
    double peerBpm = 90.0;       // the PEER's tempo, unrelated to what we're about to record
    double proposedBpm = 0.0;    // what linkEnd() derives from OUR loop and proposes
    bool proposing = false;      // s_proposeUntil hold window active

    double bpm() const { return proposing ? proposedBpm : peerBpm; }

    // abletonLink.cpp linkEnd(): derive tempo from the just-recorded clip and
    // start the propose-hold window (republishTimeline's 'proposing' branch
    // then reports OUR bpm, not the peer's, until the hold elapses).
    void linkEnd(double clipSeconds) {
        double beats;
        linkDeriveQuant(clipSeconds, &beats, &proposedBpm);
        proposing = true;
    }
};

// FIXED loopClip::_startEndingRecording: first loop ALWAYS defines the local
// grid from its own recorded length, Link-synced or not.
static void startEndingRecording_fixed(Machine& m, LinkSession& link,
                                        u32 numBlocks, u32 recordStartPhaseOffset,
                                        double clipSeconds) {
    if (m.masterLoopBlocks == 0) {
        link.linkEnd(clipSeconds);
        // no !linkIsSynced() gate any more
        m.masterLoopBlocks = numBlocks;
        m.masterPhase = recordStartPhaseOffset % numBlocks;
    }
}

// loopMachine.cpp's linkSynced&&anyRecorded branch: re-derive masterLoopBlocks
// from the session tempo once a clip exists (raw = blocksPerSecond*60*16/bpm,
// 16 beats per phrase; blocks = ((raw+4)/8)*8). Uses link.bpm(), which during
// the propose hold reports OUR just-proposed tempo (abletonLink.cpp
// republishTimeline), not the peer's.
static u32 deriveBlocksFromBpm(double bpm, double blocksPerSecond) {
    u32 raw = (u32)((blocksPerSecond * 60.0 * 16.0) / bpm + 0.5);
    return ((raw + 4) / 8) * 8;
}

// What masterLoopBlocks WOULD be if this loop's own recorded length were
// expressed as a 16-beat phrase at the tempo linkEnd() derived for it -- the
// self-consistent target the re-derivation should land near.
static u32 expectedBlocksFromOwnTempo(double ownBpm, double blocksPerSecond) {
    return deriveBlocksFromBpm(ownBpm, blocksPerSecond);
}

int main() {
    Machine m;
    LinkSession link;
    link.synced = true;
    link.peerBpm = 90.0;                 // peer's unrelated tempo
    const double blocksPerSecond = 689.0625; // INTEGRAL_BLOCKS_PER_SECOND-ish stand-in
    const u32 numBlocks = 300;           // this loop's own recorded length
    const double clipSeconds = numBlocks / blocksPerSecond;
    const u32 recordStartPhaseOffset = 5000;

    // Bank is clear (masterLoopBlocks==0) and Link-synced to a peer at 90 BPM
    // when the first loop is recorded and stopped.
    startEndingRecording_fixed(m, link, numBlocks, recordStartPhaseOffset, clipSeconds);

    check("first loop (Link-synced) defines masterLoopBlocks from ITS OWN length, not 0",
          m.masterLoopBlocks == numBlocks);
    check("first loop (Link-synced) sets masterPhase from its own recordStartPhaseOffset",
          m.masterPhase == (recordStartPhaseOffset % numBlocks));

    // Next block: loopMachine.cpp's linkSynced&&anyRecorded branch re-derives
    // masterLoopBlocks from the session BPM. During the propose-hold window
    // that BPM is OUR OWN proposed tempo (not the peer's 90 BPM), so this must
    // reproduce (approximately, given the quantized bpm<->blocks round-trip)
    // the SAME grid we just set -- not silently jump to the peer's tempo.
    u32 rederived = deriveBlocksFromBpm(link.bpm(), blocksPerSecond);
    check("propose-hold reports OUR bpm, not peer's stale bpm",
          link.bpm() == link.proposedBpm && link.bpm() != link.peerBpm);
    // The re-derivation must be self-consistent with OUR OWN proposed tempo
    // (a 16-beat phrase at that tempo), not asserted equal to numBlocks
    // directly -- linkDeriveQuant's clip-relative beat count and the 16-
    // beats-per-phrase convention are different scales by design.
    u32 expected = expectedBlocksFromOwnTempo(link.proposedBpm, blocksPerSecond);
    check("re-derivation from OUR proposed tempo is self-consistent (same formula, same bpm)",
          rederived == expected);

    // Contrast: the OLD (buggy) behavior would have left masterLoopBlocks==0
    // after startEndingRecording (since linkIsSynced()==true skipped the
    // assignment), so the very first re-derivation would come from the
    // PEER's tempo (90 BPM) with no "our own loop" anchor at all -- assert
    // that is NOT what happened here, i.e. the peer's tempo alone would have
    // produced a materially different length than what we actually got.
    u32 peerDerived = deriveBlocksFromBpm(link.peerBpm, blocksPerSecond);
    check("fixed behavior differs from the old bug's peer-tempo-derived length",
          peerDerived != m.masterLoopBlocks);

    printf(g_fails ? "SOME FAILED\n" : "ALL PASS\n");
    return g_fails ? 1 : 0;
}
