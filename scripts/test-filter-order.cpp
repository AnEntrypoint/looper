// Filter-move-to-end — host validation.
//
// The looper used to run HP/LP filters BEFORE the microrepeat glitch (inside
// processFilterAndSends). They now run AFTER it (processFilters at the end of
// the chain). This test asserts:
//   (1) DEFAULT-KNOB PARITY: at default knobs (HP cutoff 0, LP cutoff 1)
//       processFilters is a byte-exact pass-through, so relocating it changes
//       nothing audible until a filter knob is moved.
//   (2) ORDER IS OBSERVABLE: with the LP filter engaged and a microrepeat slice
//       active, filtering AFTER the glitch (new order) differs from filtering
//       BEFORE it (old order) — proving the filter now acts on the stuttered
//       signal. With the filter bypassed the two orders are identical.
//
// Build: g++ -O2 -std=c++17 scripts/test-filter-order.cpp -o scripts/test-filter-order.exe
#include <cstdio>
#include <cstdint>
#include <cmath>
#include "../patches/apcEffectsProcessor.h"
#include "../patches/microRepeat.h"

static int g_fails = 0;
static void check(const char* n, bool c){ if(c)printf("ok: %s\n",n); else {printf("FAIL: %s\n",n);g_fails++;} }

static const int N = 64;

int main()
{
    // (1) default-knob parity: processFilters is a pass-through.
    {
        apcEffectsProcessor* fx = new apcEffectsProcessor(48000);
        // default ctor: m_hpCutoff=0, m_lpCutoff=1, m_lpRes=0 -> both guards skip.
        float l[N], r[N], l0[N], r0[N];
        for (int i=0;i<N;i++){ l[i]=l0[i]=std::sin(i*0.3f); r[i]=r0[i]=std::cos(i*0.2f); }
        fx->processFilters(l, r, N, 48000);
        bool same = true;
        for (int i=0;i<N;i++){ if (l[i]!=l0[i] || r[i]!=r0[i]) same=false; }
        check("default knobs: filter pass-through byte-exact", same);
        delete fx;
    }

    // Helper: run one block through the microrepeat stage (int domain) producing
    // a stuttered block, returning it in inout.
    auto runMR = [](microRepeat& mr, int* inout, uint32_t phase, uint32_t M, uint8_t div){
        mr.process(inout, phase, M, div, N);
    };

    // (2) order observability. Build a changing ramp stream so the stutter (a
    // repeated earlier slice) differs from the live signal, then compare:
    //   NEW: microrepeat THEN LP filter
    //   OLD: LP filter THEN microrepeat
    // with the LP engaged. Then repeat with LP bypassed (must be identical).
    auto streamDiff = [&](bool engageLP)->bool{
        const uint32_t M = 320;           // 16 beats -> beat=20 blocks
        microRepeat* pmrNew = new microRepeat(); microRepeat* pmrOld = new microRepeat();
        microRepeat& mrNew = *pmrNew; microRepeat& mrOld = *pmrOld;
        apcEffectsProcessor* pfxNew = new apcEffectsProcessor(48000);
        apcEffectsProcessor* pfxOld = new apcEffectsProcessor(48000);
        apcEffectsProcessor& fxNew = *pfxNew; apcEffectsProcessor& fxOld = *pfxOld;
        if (engageLP){ fxNew.setLowpassCutoff(0.5f); fxOld.setLowpassCutoff(0.5f); }
        uint8_t div = 4;                  // 1/4 beat stutter
        bool anyDiff = false;
        for (uint32_t blk=0; blk<120; blk++){
            int base = (int)(blk*N);
            int aNew[2*N], aOld[2*N];
            for (int i=0;i<N;i++){
                int s = (int)(3000.0*std::sin((base+i)*0.05));
                aNew[i]=aOld[i]=s; aNew[N+i]=aOld[N+i]=s;
            }
            // NEW order: glitch then filter.
            runMR(mrNew, aNew, blk, M, div);
            { float fl[N],fr[N]; for(int i=0;i<N;i++){fl[i]=aNew[i]/32768.0f;fr[i]=aNew[N+i]/32768.0f;}
              fxNew.processFilters(fl,fr,N,48000);
              for(int i=0;i<N;i++){aNew[i]=(int)(fl[i]*32768.0f);aNew[N+i]=(int)(fr[i]*32768.0f);} }
            // OLD order: filter then glitch.
            { float fl[N],fr[N]; for(int i=0;i<N;i++){fl[i]=aOld[i]/32768.0f;fr[i]=aOld[N+i]/32768.0f;}
              fxOld.processFilters(fl,fr,N,48000);
              for(int i=0;i<N;i++){aOld[i]=(int)(fl[i]*32768.0f);aOld[N+i]=(int)(fr[i]*32768.0f);} }
            runMR(mrOld, aOld, blk, M, div);
            for (int i=0;i<2*N;i++) if (aNew[i]!=aOld[i]) anyDiff=true;
        }
        delete pfxNew; delete pfxOld; delete pmrNew; delete pmrOld;
        return anyDiff;
    };

    check("LP engaged: filter-after-glitch differs from filter-before", streamDiff(true));
    check("LP bypassed: order makes no difference", !streamDiff(false));

    printf(g_fails ? "\n%d FAIL\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}
