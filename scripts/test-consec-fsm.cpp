// Full consecutive-loop FSM simulation: latch -> record -> finish -> tail ->
// finishRecording -> playing. Asserts the clip REACHES PLAYING with running==1.
// Mirrors loopClipUpdate.cpp record/state advance + loopClipState.cpp updateState
// + loopClip.cpp _startEndingRecording/_finishRecording/_startPlaying running cnt.
// Build: g++ -O2 -std=c++17 scripts/test-consec-fsm.cpp -o scripts/test-consec-fsm.exe
#include <cstdint>
#include <cstdio>
typedef uint32_t u32;
static const u32 CROSSFADE=4;
static int g_fails=0;
static void check(const char*n,bool c){ if(c)printf("ok: %s\n",n); else {printf("FAIL: %s\n",n);g_fails++;} }

enum St{RECORDING,RECORDING_MAIN,RECORDING_TAIL,FINISHING,RECORDED,PLAYING,LOOPING};

struct Clip {
    St st; u32 play, rec, num, max, qtarget; bool qwill; int running; u32 M;
};

static u32 calc_quant(u32 M, u32 rec){
    u32 F=CROSSFADE*2; u32 cand[]={F,M/8,M/4,M/2,M,M*2,M*4,M*8}; u32 best=F;
    for(int i=0;i<8;i++){u32 c=cand[i]; if(c<F)continue; if(c<=rec)best=c; else{if(rec*2>c)best=c;break;}}
    return best;
}
static void startEnding(Clip&c,u32 trim,bool willPlay){
    c.num=trim?trim:c.rec; c.max=c.num+CROSSFADE;
    c.st=willPlay?RECORDING_TAIL:FINISHING; c.play=0;
    // NOTE: _startEndingRecording does NOT touch running in the real code.
}
static void finishRec(Clip&c){
    bool willPlay=(c.st==RECORDING_TAIL);
    c.st=RECORDED; c.running--;                 // line 231
    if(willPlay){ /* _startPlaying preserve */ c.st=PLAYING; c.running++; } // line 284
}
// One audio-block update of the clip while recording/tail (running gate handled
// by caller). Returns nothing; advances rec + state like loopClipUpdate.
static void updateBlock(Clip&c){
    bool rp = (c.st==RECORDING||c.st==RECORDING_MAIN||c.st==RECORDING_TAIL||c.st==FINISHING);
    if(rp){
        c.rec++;
        if(c.st==RECORDING && c.rec>=CROSSFADE) c.st=RECORDING_MAIN;
        if(c.st==RECORDING_MAIN && c.qtarget>0 && c.rec>=c.qtarget){
            u32 t=c.qtarget; bool p=c.qwill; c.qtarget=0; c.qwill=false; startEnding(c,t,p);
        } else if((c.st==RECORDING_TAIL||c.st==FINISHING) && c.rec>=c.max){
            finishRec(c);
        }
    }
}
// updateState(PLAY) finish tap.
static void tapFinishPlay(Clip&c){
    if(c.st==RECORDING||c.st==RECORDING_MAIN){
        if(c.st==RECORDING && c.rec==0){ /* stopImmediate */ c.st=RECORDED; c.running--; return; }
        u32 target=calc_quant(c.M,c.rec);
        if(target<=c.rec) startEnding(c,target,true);
        else { c.qtarget=target; c.qwill=true; }
    }
}

static bool simulate(u32 M, u32 recordBlocks, const char* label){
    Clip c{}; c.M=M; c.st=RECORDING; c.running=1;   // _startRecording +1
    // The machine only calls update() while running>0 AND m_running (first loop plays).
    // Record `recordBlocks` audio blocks:
    for(u32 i=0;i<recordBlocks;i++){ if(c.running>0) updateBlock(c); }
    // operator taps finish:
    tapFinishPlay(c);
    // continue audio blocks (tail records to max, then finishRecording->playing).
    // The clip only advances if running>0 (track-update gate). Give it plenty.
    for(int i=0;i<(int)M*9 && c.st!=PLAYING && c.st!=RECORDED && c.st!=LOOPING; i++){
        if(c.running>0) updateBlock(c); else break;   // running==0 -> update NEVER called = stuck
    }
    printf("  [%s] M=%u rec=%u -> st=%d running=%d num=%u qtarget=%u\n",
           label, M, recordBlocks, (int)c.st, c.running, c.num, c.qtarget);
    return c.st==PLAYING && c.running==1 && c.num>0;
}

int main(){
    const u32 M=5520;
    check("short consec tap (rec=700~M/8) reaches PLAYING running=1",
          simulate(M, 700, "short"));
    check("half-phrase consec (rec=2760) reaches PLAYING",
          simulate(M, 2760, "half"));
    check("near-phrase consec (rec=5400) reaches PLAYING",
          simulate(M, 5400, "near-phrase"));
    check("phrase consec (rec=5520) reaches PLAYING",
          simulate(M, 5520, "phrase"));
    check("tiny tap (rec=2) reaches PLAYING (floored)",
          simulate(M, 2, "tiny"));
    if(g_fails){printf("%d FAILURE(S)\n",g_fails);return 1;}
    printf("ALL PASS\n");return 0;
}
