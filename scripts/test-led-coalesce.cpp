// test-led-coalesce.cpp — proves the LED-send retry-cache invariant the user is
// worried about ("cc light updates sometimes dont fire, leaving lights unset"):
// when the MIDI-OUT frame DROPS, the coalesce cache must stay UNCHANGED so the
// next tick retries until the hardware matches. No permanently-lost state.
// Mirrors apcKey25Transpose.cpp sendLedCoalesced + the live-LED drop-retry.
#include <cstdio>
#include <cstdint>
typedef uint8_t u8;

// --- mirror of the firmware coalesce cache + drop-aware send ---
static u8   s_lastLedState[128];
static bool s_ledCacheValid = false;
static int  s_hwLed[128];        // simulated hardware LED state
static bool s_dropNext = false;  // injected MIDI-OUT-full

// returns true iff frame queued (mirrors usbMidiSendNoteOn bool contract)
static bool sim_usbMidiSendNoteOn(u8 note, u8 vel) {
    if (s_dropNext) return false;       // DROP: nothing reaches hardware
    s_hwLed[note] = vel;                // frame landed -> hardware updates
    return true;
}
static void sendLedCoalesced(u8 note, u8 vel) {
    if (!s_ledCacheValid) { for (int i=0;i<128;i++) s_lastLedState[i]=0xFF; s_ledCacheValid=true; }
    if (s_lastLedState[note] == vel) return;          // coalesce: unchanged
    if (sim_usbMidiSendNoteOn(note, vel))             // commit cache ONLY on success
        s_lastLedState[note] = vel;
}
static void invalidateLedCache() { s_ledCacheValid = false; }

#define CHECK(c,msg) do{ if(!(c)){ printf("FAIL: %s\n",msg); fails++; } else { printf("ok: %s\n",msg); } }while(0)

int main() {
    int fails = 0;
    for (int i=0;i<128;i++){ s_hwLed[i] = -1; }

    // 1) normal send updates hw + cache
    s_dropNext = false;
    sendLedCoalesced(60, 3);
    CHECK(s_hwLed[60]==3 && s_lastLedState[60]==3, "normal send reaches hw and caches");

    // 2) coalesce: same value does NOT re-send (no spurious traffic)
    s_hwLed[60] = -99;                 // pretend hw drifted; coalesce should SKIP
    sendLedCoalesced(60, 3);
    CHECK(s_hwLed[60]==-99, "coalesce skips re-send when cache==value");

    // 3) DROP leaves cache UNCHANGED so the new state is retried (the core fix)
    s_dropNext = true;
    sendLedCoalesced(60, 5);           // want to change 3->5 but MIDI OUT full
    CHECK(s_lastLedState[60]==3, "on drop, cache stays at OLD value (not 5) = will retry");
    CHECK(s_hwLed[60]==-99, "on drop, hw did not change");

    // 4) next tick (drop cleared) retries and succeeds -> hw matches new state
    s_dropNext = false;
    sendLedCoalesced(60, 5);
    CHECK(s_hwLed[60]==5 && s_lastLedState[60]==5, "retry after drop lands the new state");

    // 5) sustained drop then recovery: state is NEVER permanently lost
    s_dropNext = true;
    for (int t=0;t<50;t++) sendLedCoalesced(60, 1);   // 50 ticks all dropping
    CHECK(s_lastLedState[60]==5, "sustained drop keeps cache at last-good (5), keeps retrying 1");
    s_dropNext = false;
    sendLedCoalesced(60, 1);
    CHECK(s_hwLed[60]==1, "after 50 dropped ticks, first good tick lands the pending state");

    // 6) reconnect: invalidate cache -> every LED re-sends even if value same
    s_hwLed[60] = -1;                  // hardware reset its LEDs (reconnect)
    invalidateLedCache();
    sendLedCoalesced(60, 1);           // same value 1, but cache invalid -> must re-send
    CHECK(s_hwLed[60]==1, "after invalidateLedCache, same-value LED re-sends (post-reconnect resync)");

    printf(fails? "\n%d FAIL\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
