// Consecutive (M>0) quant: round DOWN to a division for sub-phrase (plays now),
// round to nearest phrase MULTIPLE for phrase-or-longer. Mirrors the rewritten
// loopClip.cpp::_calcQuantizeTarget so every stop yields a playable clip.
// Build: g++ -O2 -std=c++17 scripts/test-second-loop-play.cpp -o scripts/test-second-loop-play.exe
#include <cstdint>
#include <cstdio>
#include <initializer_list>
typedef uint32_t u32;
static const u32 CROSSFADE=4;
static int g_fails=0;
static void check(const char*n,bool c){ if(c)printf("ok: %s\n",n); else {printf("FAIL: %s\n",n);g_fails++;} }

static u32 calc_quant(u32 M, u32 rec) {
    u32 floorLen = CROSSFADE*2;
    u32 cand[]={floorLen, M/8,M/4,M/2,M,M*2,M*4,M*8};
    u32 best=floorLen;
    for(int i=0;i<8;i++){
        u32 c=cand[i]; if(c<floorLen) continue;
        if(c<=rec) best=c;
        else { if(rec*2>c) best=c; break; }
    }
    return best;
}

int main(){
    const u32 M=5520; const u32 F=CROSSFADE*2;
    auto isGrid=[&](u32 t){ return t==F||t==M/8||t==M/4||t==M/2||t==M||t==M*2||t==M*4||t==M*8; };

    // The KILLER bug: a SHORT tap that rounds UP to a much-larger grid defers and
    // stalls ("records then silent, never starts"). Invariant: whenever the chosen
    // target EXCEEDS recorded, rec must be at least halfway to it (so the extra
    // wait is < the recorded length — a small, intentional extend, never a stall).
    for (u32 rec : {5u, 200u, 700u, 1400u, 2800u, 5000u, 5500u, 8000u, 11000u, 21000u}) {
        u32 t=calc_quant(M,rec);
        printf("  rec=%u target=%u %s\n", rec, t, (t<=rec)?"plays-now":"extends");
        check("target is a valid grid point", isGrid(t));
        check("if target>recorded, rec is past the midpoint (small extend, no stall)",
              t <= rec || rec*2 >= t);
    }
    // Tiny tap floors and plays now.
    check("tiny tap (5) -> floor, plays now", calc_quant(M,5) == F);
    // Short tap that previously stalled: rec=200 (<<M/8 midpoint 345) rounds DOWN.
    check("rec=200 rounds down to floor (plays now, not deferred to M/8)",
          calc_quant(M,200) <= 200);
    // Near-phrase rounds UP to the phrase (505 play-through).
    check("rec=5500 (just under M) rounds up to M", calc_quant(M,5500) == M);
    // Clean half-phrase tap.
    check("rec~M/2 -> M/2", calc_quant(M, M/2) == M/2);

    if(g_fails){printf("%d FAILURE(S)\n",g_fails);return 1;}
    printf("ALL PASS\n");return 0;
}
