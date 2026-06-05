// Arrangement (preset) memory upkeep on looper erase.
//
// An "arrangement" is a preset slot: m_presetMask[p] is a bit-mask of which
// loopers play when the preset is recalled, m_presetUsed[p] marks it captured
// (its LED is lit only while used). When a looper is erased it must be dropped
// from every arrangement; an arrangement made up solely of now-erased loopers
// becomes empty and is deleted (used=false), so its LED goes dark exactly when
// its last member is gone.
//
// This test replicates the exact logic of apcKey25::_forgetLooperFromPresets
// and _forgetAllPresets (apcKey25Notes.cpp) so the invariant is witnessed on
// the host without a Pi:
//   (1) erasing a looper clears its bit from every mask;
//   (2) a non-empty arrangement stays used after a partial erase;
//   (3) an arrangement deletes (used=false, LED dark) only when its mask hits 0;
//   (4) erasing a looper not in an arrangement is a no-op for it;
//   (5) erasing an already-empty/never-set looper never spuriously deletes;
//   (6) a deleted slot is reusable by a fresh capture;
//   (7) whole-bank clear deletes every arrangement.
//
// Build: g++ -O2 -std=c++17 scripts/test-arrangement-forget.cpp -o scripts/test-arrangement-forget.exe
#include <cstdio>
#include <cstdint>
typedef uint32_t u32;

static int g_fails = 0;
static void check(const char* n, bool c){ if(c)printf("ok: %s\n",n); else {printf("FAIL: %s\n",n);g_fails++;} }

static const int LOOPER_NUM_PRESETS = 10;

// Mirror of apcKey25 preset state.
static u32  m_presetMask[LOOPER_NUM_PRESETS];
static bool m_presetUsed[LOOPER_NUM_PRESETS];

// Mirror of apcKey25::_capturePreset for a given explicit set of playing loopers.
static void capturePreset(int p, u32 playingMask)
{
    if (p < 0 || p >= LOOPER_NUM_PRESETS) return;
    m_presetMask[p] = playingMask;
    m_presetUsed[p] = true;
}

// Verbatim mirror of apcKey25::_forgetLooperFromPresets.
static void forgetLooperFromPresets(int n)
{
    if (n < 0 || n >= 32) return;
    u32 bit = (1u << n);
    for (int p = 0; p < LOOPER_NUM_PRESETS; p++)
    {
        if (!m_presetUsed[p]) continue;
        if (!(m_presetMask[p] & bit)) continue;
        m_presetMask[p] &= ~bit;
        if (m_presetMask[p] == 0)
            m_presetUsed[p] = false;
    }
}

// Verbatim mirror of apcKey25::_forgetAllPresets.
static void forgetAllPresets()
{
    for (int p = 0; p < LOOPER_NUM_PRESETS; p++)
    {
        m_presetMask[p] = 0;
        m_presetUsed[p] = false;
    }
}

static void reset()
{
    for (int p = 0; p < LOOPER_NUM_PRESETS; p++) { m_presetMask[p] = 0; m_presetUsed[p] = false; }
}

int main()
{
    // ---- (1)(2)(3) partial erase keeps used, full erase deletes ----
    reset();
    capturePreset(0, (1u<<2)|(1u<<5)|(1u<<7));   // arrangement of loopers {2,5,7}
    check("captured arrangement is used",        m_presetUsed[0]);
    check("captured mask = {2,5,7}",             m_presetMask[0] == ((1u<<2)|(1u<<5)|(1u<<7)));

    forgetLooperFromPresets(2);
    check("erase 2: bit cleared",                (m_presetMask[0] & (1u<<2)) == 0);
    check("erase 2: 5,7 remain",                 m_presetMask[0] == ((1u<<5)|(1u<<7)));
    check("erase 2: still used (not empty)",      m_presetUsed[0]);

    forgetLooperFromPresets(5);
    check("erase 5: only 7 remains",             m_presetMask[0] == (1u<<7));
    check("erase 5: still used",                  m_presetUsed[0]);

    forgetLooperFromPresets(7);
    check("erase 7: mask empty",                 m_presetMask[0] == 0);
    check("erase 7: arrangement DELETED (LED dark)", !m_presetUsed[0]);

    // ---- (4) erasing a looper not in the arrangement is a no-op ----
    reset();
    capturePreset(3, (1u<<1)|(1u<<4));
    forgetLooperFromPresets(9);                  // 9 not a member
    check("non-member erase: mask unchanged",    m_presetMask[3] == ((1u<<1)|(1u<<4)));
    check("non-member erase: still used",         m_presetUsed[3]);

    // ---- (5) erasing while already empty/never-set never deletes spuriously ----
    reset();
    capturePreset(1, (1u<<8));
    forgetLooperFromPresets(0);                  // looper 0 in no mask
    check("empty-looper erase: unrelated arrangement intact", m_presetUsed[1] && m_presetMask[1]==(1u<<8));

    // ---- (6) a deleted slot is reusable by fresh capture ----
    reset();
    capturePreset(2, (1u<<3));
    forgetLooperFromPresets(3);
    check("slot 2 deleted",                      !m_presetUsed[2] && m_presetMask[2]==0);
    capturePreset(2, (1u<<6)|(1u<<9));           // re-capture into the same slot
    check("slot 2 re-captured fresh",            m_presetUsed[2] && m_presetMask[2]==((1u<<6)|(1u<<9)));

    // ---- (7) whole-bank clear deletes every arrangement ----
    reset();
    capturePreset(0, (1u<<0));
    capturePreset(4, (1u<<1)|(1u<<2));
    capturePreset(9, (1u<<19));
    forgetAllPresets();
    bool allDark = true;
    for (int p = 0; p < LOOPER_NUM_PRESETS; p++)
        if (m_presetUsed[p] || m_presetMask[p] != 0) allDark = false;
    check("CLEAR_ALL: every arrangement dark", allDark);

    // ---- one looper shared across two arrangements ----
    reset();
    capturePreset(0, (1u<<5)|(1u<<6));
    capturePreset(1, (1u<<5));                    // arrangement made solely of looper 5
    forgetLooperFromPresets(5);
    check("shared erase: arr0 keeps 6, used",    m_presetUsed[0] && m_presetMask[0]==(1u<<6));
    check("shared erase: arr1 (only 5) deleted", !m_presetUsed[1] && m_presetMask[1]==0);

    if (g_fails) { printf("\n%d FAIL\n", g_fails); return 1; }
    printf("\nALL PASS\n");
    return 0;
}
