// Host test for the USB gamepad -> looper control mapping (pure logic in
// patches/gamepadInput.h). No Pi, no Circle: g++ -O2 -std=c++17.
//
//   g++ -O2 -std=c++17 -I ../patches scripts/test-gamepad.cpp -o /tmp/tg && /tmp/tg
//
// Drives gpAxis*/gpHat*/gpButton* helpers and the gamepadInput diff/emit engine
// (bound to a recording emit sink) and asserts the synthesized MIDI matches the
// user's mapping spec.

#include "gamepadInput.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <cstring>

static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); g_fail++; } } while(0)
#define CHECKEQ(a, b, msg) do { if ((a) != (b)) { printf("FAIL: %s (got %d want %d)\n", msg, (int)(a), (int)(b)); g_fail++; } } while(0)

// ---- emit recorder ----
struct Ev { unsigned char st, d1, d2; };
static std::vector<Ev> g_ev;
static void recEmit(unsigned char st, unsigned char d1, unsigned char d2) { g_ev.push_back({st,d1,d2}); }

// find last CC=cc value in g_ev, or -1
static int lastCC(int cc) {
    for (int i = (int)g_ev.size()-1; i >= 0; i--)
        if ((g_ev[i].st & 0xF0) == GP_APC_CC && g_ev[i].d1 == cc) return g_ev[i].d2;
    return -1;
}
static bool sawNote(unsigned char st, int note) {
    for (auto &e : g_ev) if (e.st == st && e.d1 == note) return true;
    return false;
}

static GpState mkState() {
    GpState s; memset(&s, 0, sizeof(s));
    // 6 centered/unipolar axes, default centered, range 0..255 (typical HID)
    s.naxes = 6;
    for (int i = 0; i < 6; i++) { s.axes[i].minimum = 0; s.axes[i].maximum = 255; s.axes[i].value = 128; }
    s.axes[GP_AXIS_LT].value = 0;   // triggers rest at 0
    s.axes[GP_AXIS_RT].value = 0;
    s.nhats = 1; s.hats[0] = -1;    // centered hat
    s.nbuttons = 22; s.buttons = 0;
    return s;
}

int main() {
    // ---- gpAxisNorm01 ----
    CHECK(std::fabs(gpAxisNorm01(0,0,255) - 0.0f) < 1e-6, "norm min=0");
    CHECK(std::fabs(gpAxisNorm01(255,0,255) - 1.0f) < 1e-6, "norm max=1");
    CHECK(std::fabs(gpAxisNorm01(128,0,255) - 0.5019f) < 0.01f, "norm mid~0.5");
    CHECK(std::fabs(gpAxisNorm01(50,10,10) - 0.5f) < 1e-6, "norm degenerate min==max -> 0.5");
    CHECK(std::fabs(gpAxisNorm01(-5,0,255) - 0.0f) < 1e-6, "norm clamp low");
    CHECK(std::fabs(gpAxisNorm01(999,0,255) - 1.0f) < 1e-6, "norm clamp high");

    // ---- deadzone: centered stick reads 0 ----
    CHECK(gpAxisBipolarDeadzoned(128,0,255) == 0.0f, "center -> deadzone 0");
    CHECK(gpAxisBipolarDeadzoned(130,0,255) == 0.0f, "small offset inside deadzone -> 0");
    CHECK(gpAxisBipolarDeadzoned(255,0,255) > 0.9f, "full right -> ~+1");
    CHECK(gpAxisBipolarDeadzoned(0,0,255) < -0.9f, "full left -> ~-1");
    // monotonic leaving deadzone: just outside dz edge -> small positive, no jump
    {
        float edge = gpAxisBipolarDeadzoned(128 + (int)(GP_DEADZONE_FRAC*127) + 3, 0, 255);
        CHECK(edge > 0.0f && edge < 0.3f, "monotonic exit from deadzone");
    }

    // ---- MIDI conversion ----
    CHECKEQ(gpToMidi01(0.0f), 0, "midi 0");
    CHECKEQ(gpToMidi01(1.0f), 127, "midi 127");
    CHECKEQ(gpBipolarToMidi(0.0f), 64, "bipolar center -> 64");
    CHECKEQ(gpBipolarToMidi(1.0f), 127, "bipolar +1 -> 127");
    CHECKEQ(gpBipolarToMidi(-1.0f), 1, "bipolar -1 -> 1");

    // ---- hat -> microrepeat note ----
    CHECKEQ(gpHatToMicroNote(0), GP_HAT_NOTE_UP,    "hat N -> up note");
    CHECKEQ(gpHatToMicroNote(2), GP_HAT_NOTE_RIGHT, "hat E -> right note");
    CHECKEQ(gpHatToMicroNote(4), GP_HAT_NOTE_DOWN,  "hat S -> down note");
    CHECKEQ(gpHatToMicroNote(6), GP_HAT_NOTE_LEFT,  "hat W -> left note");
    CHECKEQ(gpHatToMicroNote(-1), 0, "hat center -> 0");
    CHECKEQ(gpHatToMicroNote(8), 0, "hat null -> 0");

    // ---- button -> looper table coverage (reserved excluded) ----
    // L1/R1 reserved
    {
        int l1bit = 5, r1bit = 6;
        CHECKEQ(gpButtonBitToLooper(l1bit, 20), -1, "L1 not a looper");
        CHECKEQ(gpButtonBitToLooper(r1bit, 20), -1, "R1 not a looper");
        // A button (bit 9) is a looper
        CHECK(gpButtonBitToLooper(9, 20) >= 0, "A -> a looper");
        // dpad bits reserved
        CHECKEQ(gpButtonBitToLooper(15, 20), -1, "UP not a looper");
    }

    // ---- looper -> pad note ----
    CHECKEQ(gpLooperToPadNote(0, 20), 2, "looper0 -> pad note 2 (row0 col2)");
    CHECKEQ(gpLooperToPadNote(4, 20), 10, "looper4 -> pad note 10 (row1 col2)");
    CHECKEQ(gpLooperToPadNote(-1, 20), -1, "looper -1 invalid");

    // ============ integration: gamepadInput diff/emit ============
    gamepadInput gp;
    gp.setEmit(recEmit);
    gp.setNumTracks(20);
    gp.setConnected(true);

    // Z-axis (transpose) full right -> CC52 high
    g_ev.clear();
    { GpState s = mkState(); s.axes[GP_AXIS_Z].value = 255; gp.pushState(&s); gp.processTick(); }
    CHECK(lastCC(GP_CC_TRANSPOSE) > 100, "Z full right -> transpose CC52 high");

    // Z-axis center -> CC52 == 64 (neutral)
    g_ev.clear();
    { GpState s = mkState(); s.axes[GP_AXIS_Z].value = 128; gp.pushState(&s); gp.processTick(); }
    CHECKEQ(lastCC(GP_CC_TRANSPOSE), 64, "Z center -> transpose 64");

    // Z-rotation (formant) full -> CC53 high
    g_ev.clear();
    { GpState s = mkState(); s.axes[GP_AXIS_RZ].value = 255; gp.pushState(&s); gp.processTick(); }
    CHECK(lastCC(GP_CC_FORMANT) > 100, "Rz full -> formant CC53 high");

    // X-axis vertical (Y) pushed DOWN (max) -> lowpass closes (CC55 low), HP off
    g_ev.clear();
    { GpState s = mkState(); s.axes[GP_AXIS_Y].value = 255; gp.pushState(&s); gp.processTick(); }
    CHECK(lastCC(GP_CC_LP) >= 0 && lastCC(GP_CC_LP) < 40, "Y down -> LP closes");
    // up (min) -> highpass opens (CC51 high)
    g_ev.clear();
    { GpState s = mkState(); s.axes[GP_AXIS_Y].value = 0; gp.pushState(&s); gp.processTick(); }
    CHECK(lastCC(GP_CC_HP) > 80, "Y up -> HP opens");

    // X-axis horizontal (X) deflected -> resonance up
    g_ev.clear();
    { GpState s = mkState(); s.axes[GP_AXIS_X].value = 255; gp.pushState(&s); gp.processTick(); }
    CHECK(lastCC(GP_CC_RES) > 80, "X full -> resonance up");

    // LT/RT triggers -> delay amount/time
    g_ev.clear();
    { GpState s = mkState(); s.axes[GP_AXIS_LT].value = 255; s.axes[GP_AXIS_RT].value = 255;
      gp.pushState(&s); gp.processTick(); }
    CHECK(lastCC(GP_CC_DELAY) > 100, "LT -> delay amount high");
    CHECK(lastCC(GP_CC_TIME)  > 100, "RT -> delay time high");

    // R1 -> reverb max while held; release -> 0
    g_ev.clear();
    { GpState s = mkState(); s.buttons = GP_BTN_R1; gp.pushState(&s); gp.processTick(); }
    CHECKEQ(lastCC(GP_CC_REVERB), 127, "R1 held -> reverb 127");
    g_ev.clear();
    { GpState s = mkState(); s.buttons = 0; gp.pushState(&s); gp.processTick(); }
    CHECKEQ(lastCC(GP_CC_REVERB), 0, "R1 released -> reverb 0");

    // L1 -> SHIFT note on, then off
    g_ev.clear();
    { GpState s = mkState(); s.buttons = GP_BTN_L1; gp.pushState(&s); gp.processTick(); }
    CHECK(sawNote(GP_APC_NOTE_ON, GP_APC_SHIFT_NOTE), "L1 -> shift note-on");
    g_ev.clear();
    { GpState s = mkState(); s.buttons = 0; gp.pushState(&s); gp.processTick(); }
    CHECK(sawNote(GP_APC_NOTE_OFF, GP_APC_SHIFT_NOTE), "L1 release -> shift note-off");

    // A button -> looper pad note-on (some pad note), release -> note-off
    g_ev.clear();
    { GpState s = mkState(); s.buttons = GP_BTN_A; gp.pushState(&s); gp.processTick(); }
    {
        bool anyPadOn = false;
        for (auto &e : g_ev) if (e.st == GP_APC_NOTE_ON && e.d1 != GP_APC_SHIFT_NOTE && e.d1 < 40) anyPadOn = true;
        CHECK(anyPadOn, "A button -> looper pad note-on");
    }

    // HAT up -> microrepeat note-on; center -> note-off
    g_ev.clear();
    { GpState s = mkState(); s.hats[0] = 0; gp.pushState(&s); gp.processTick(); }
    CHECK(sawNote(GP_APC_NOTE_ON, GP_HAT_NOTE_UP), "hat up -> microrepeat note-on");
    g_ev.clear();
    { GpState s = mkState(); s.hats[0] = -1; gp.pushState(&s); gp.processTick(); }
    CHECK(sawNote(GP_APC_NOTE_OFF, GP_HAT_NOTE_UP), "hat center -> microrepeat note-off");

    // ---- edge-detect: held button does not re-fire ----
    {
        // First tick: 0->A press = one pad-on. Second tick with A still held =
        // no change = no new pad-on. Count only the second tick's emissions.
        GpState s = mkState(); s.buttons = GP_BTN_A;
        gp.pushState(&s); gp.processTick();          // the press
        g_ev.clear();                                 // discard the press emit
        gp.pushState(&s); gp.processTick();          // A still held, no change
        int padOns = 0;
        for (auto &e : g_ev) if (e.st == GP_APC_NOTE_ON && e.d1 < 40 && e.d1 != GP_APC_SHIFT_NOTE) padOns++;
        CHECK(padOns == 0, "held button A does not re-fire (edge-detect)");
    }

    // ---- ring coalesce: two pushes before a tick -> dropped++ but latest applied ----
    {
        gamepadInput gp2; gp2.setEmit(recEmit); gp2.setNumTracks(20); gp2.setConnected(true);
        g_ev.clear();
        GpState s1 = mkState(); s1.axes[GP_AXIS_Z].value = 0;
        GpState s2 = mkState(); s2.axes[GP_AXIS_Z].value = 255;
        gp2.pushState(&s1);          // un-consumed
        gp2.pushState(&s2);          // coalesce -> dropped++
        CHECK(gp2.dropped() >= 1, "coalesced un-consumed snapshot counted");
        gp2.processTick();
        CHECK(lastCC(GP_CC_TRANSPOSE) > 100, "latest (s2) applied after coalesce");
    }

    // ---- disconnect clears momentary (shift/reverb/hat) ----
    {
        gamepadInput gp3; gp3.setEmit(recEmit); gp3.setNumTracks(20); gp3.setConnected(true);
        { GpState s = mkState(); s.buttons = GP_BTN_L1 | GP_BTN_R1; s.hats[0]=0;
          gp3.pushState(&s); gp3.processTick(); }
        g_ev.clear();
        gp3.setConnected(false);   // unplug
        CHECK(sawNote(GP_APC_NOTE_OFF, GP_APC_SHIFT_NOTE), "disconnect -> shift off");
        CHECKEQ(lastCC(GP_CC_REVERB), 0, "disconnect -> reverb off");
        CHECK(sawNote(GP_APC_NOTE_OFF, GP_HAT_NOTE_UP), "disconnect -> glitch off");
    }

    if (g_fail == 0) printf("ALL PASS\n");
    else printf("%d FAILURES\n", g_fail);
    return g_fail ? 1 : 0;
}
