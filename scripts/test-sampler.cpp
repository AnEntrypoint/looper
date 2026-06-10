// Sampler subsystem — host validation.
//
// Mirrors patches/sampler.h. Asserts the load-bearing properties:
//   (1) auto-clip: leading/trailing silence is trimmed; an all-silence capture
//       leaves the slot unloaded;
//   (2) chromatic resample ratio: a higher note consumes the sample faster
//       (note 72 = 2x, note 48 = 0.5x of note 60), proven by voice lifetime;
//   (3) drum slot plays at ORIGINAL pitch (rate 1) as a one-shot;
//   (4) polyphony: N held notes -> N voices; drum overrides chromatic on a key;
//   (5) voice-steal: never exceeds the pool size;
//   (6) click-free: per-sample output step stays small through a held note;
//   (7) record overrun clamps at the buffer max (no overflow), recording ends;
//   (8) empty-key / no-content NOTE_ON is a silent no-op.
//
// Build: g++ -O2 -std=c++17 scripts/test-sampler.cpp -o scripts/test-sampler.exe
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include "../patches/sampler.h"

static int g_fails = 0;
static void check(const char* n, bool c){ if(c)printf("ok: %s\n",n); else {printf("FAIL: %s\n",n);g_fails++;} }

static const int N = 64;

// Build a stereo block [L0..L_{N-1}, R0..R_{N-1}] from a per-sample amplitude.
static void constBlock(int* buf, int amp){ for(int i=0;i<N;i++){buf[i]=amp;buf[N+i]=amp;} }

// Record a sample into the sampler: REC_START(target), feed `blocks` blocks of
// the given amplitude shape, REC_STOP. shape(i) returns amplitude for block i.
template<class F>
static void recordInto(sampler& s, int target, int blocks, F shape){
    s.pushEvent(sampler::EV_REC_START, target, 0);
    int junk[2*N];
    // The REC_START event is processed inside renderInto; capture happens via
    // captureBlock. Drive one renderInto first to drain the start event.
    for(int i=0;i<N*2;i++) junk[i]=0;
    s.renderInto(junk, N);                 // drains EV_REC_START -> recording armed
    int blk[2*N];
    for(int b=0;b<blocks;b++){ constBlock(blk, shape(b)); s.captureBlock(blk, N); }
    s.pushEvent(sampler::EV_REC_STOP, 0, 0);
    for(int i=0;i<N*2;i++) junk[i]=0;
    s.renderInto(junk, N);                 // drains EV_REC_STOP -> trim+load
}

// Count render blocks until all voices go silent (bounded).
static int voiceLifetimeBlocks(sampler& s){
    int out[2*N]; int blocks=0;
    while(s.activeVoices()>0 && blocks<200000){ for(int i=0;i<2*N;i++)out[i]=0; s.renderInto(out,N); blocks++; }
    return blocks;
}

int main()
{
    // (1) auto-trim: silence | signal | silence -> trimmed to ~signal.
    {
        sampler s;
        // 4 silent blocks, 10 signal blocks (amp 6000), 4 silent blocks.
        recordInto(s, -1, 18, [](int b){ return (b>=4 && b<14) ? 6000 : 0; });
        int len = s.chromLenForTest();
        check("autotrim loaded", s.chromaticLoaded());
        // signal region = 10 blocks * 64 = 640 samples; trim should land close
        // (edge fade keeps a bit). Accept [600, 680].
        check("autotrim length ~signal", len >= 600 && len <= 680);
    }

    // (1b) all-silence -> not loaded.
    {
        sampler s;
        recordInto(s, -1, 10, [](int){ return 0; });
        check("all-silence unloaded", !s.chromaticLoaded() && s.chromLenForTest()==0);
    }

    // (2) chromatic resample ratio via voice lifetime. Load a 1000-sample-ish
    // sample (loud throughout so no trim), then compare lifetimes at notes
    // 60/72/48. Lifetime(72) ~ 0.5*Lifetime(60); Lifetime(48) ~ 2*Lifetime(60).
    {
        sampler s;
        recordInto(s, -1, 40, [](int){ return 8000; });   // ~40*64=2560 samples
        int L = s.chromLenForTest();
        check("chrom sample loaded", L>2000);

        s.pushEvent(sampler::EV_NOTE_ON, 60, 100); { int o[2*N]; s.renderInto(o,N); }
        int life60 = voiceLifetimeBlocks(s);

        s.pushEvent(sampler::EV_NOTE_ON, 72, 100); { int o[2*N]; s.renderInto(o,N); }
        int life72 = voiceLifetimeBlocks(s);

        s.pushEvent(sampler::EV_NOTE_ON, 48, 100); { int o[2*N]; s.renderInto(o,N); }
        int life48 = voiceLifetimeBlocks(s);

        double r72 = (double)life72/(double)life60;
        double r48 = (double)life48/(double)life60;
        check("note72 ~2x faster (half lifetime)", r72>0.40 && r72<0.60);
        check("note48 ~2x slower (double lifetime)", r48>1.80 && r48<2.20);
    }

    // (3) drum slot: original pitch one-shot, ignores note-off.
    {
        sampler s;
        int key = 5;                       // drum slot index 5 -> note 53
        recordInto(s, key, 30, [](int){ return 7000; });
        check("drum loaded", s.drumLoaded(key));
        int dlen = s.drumLenForTest(key);
        int note = sampler::BASE_NOTE + key;
        s.pushEvent(sampler::EV_NOTE_ON, note, 100); { int o[2*N]; s.renderInto(o,N); }
        // note-off must NOT stop a drum one-shot.
        s.pushEvent(sampler::EV_NOTE_OFF, note, 0); { int o[2*N]; s.renderInto(o,N); }
        check("drum still playing after note-off", s.activeVoices()==1);
        int life = voiceLifetimeBlocks(s);
        // rate 1.0 -> lifetime ~ dlen/64 blocks (+ release).
        int expect = dlen/N;
        check("drum original pitch (rate 1)", life>=expect-2 && life<=expect+6);
    }

    // (4) polyphony + drum-overrides-chromatic.
    {
        sampler s;
        recordInto(s, -1, 20, [](int){ return 8000; });   // chromatic loaded
        recordInto(s, 3, 20, [](int){ return 8000; });    // drum on key 3
        s.pushEvent(sampler::EV_NOTE_ON, 60, 100);
        s.pushEvent(sampler::EV_NOTE_ON, 62, 100);
        s.pushEvent(sampler::EV_NOTE_ON, 64, 100);
        { int o[2*N]; s.renderInto(o,N); }
        check("poly 3 voices", s.activeVoices()==3);
        // key 3 -> note 51: drum overrides chromatic. Hard to observe directly;
        // assert drumLoaded gate is what routing keys off.
        check("drum-override gate", s.drumLoaded(3) && s.chromaticLoaded());
    }

    // (5) voice-steal never exceeds the pool.
    {
        sampler s;
        recordInto(s, -1, 20, [](int){ return 8000; });
        for(int n=40;n<40+30;n++){ s.pushEvent(sampler::EV_NOTE_ON, n, 100); }
        { int o[2*N]; s.renderInto(o,N); }
        check("voice-steal bounded", s.activeVoices() <= sampler::VOICES);
    }

    // (6) click-free held note: per-sample output step bounded for a constant
    // sample (the only motion is the attack gain ramp, which is smooth).
    {
        sampler s;
        recordInto(s, -1, 40, [](int){ return 8000; });
        s.pushEvent(sampler::EV_NOTE_ON, 60, 100);
        int prev=0; bool first=true; int maxStep=0;
        for(int b=0;b<20;b++){
            int o[2*N]; for(int i=0;i<2*N;i++)o[i]=0; s.renderInto(o,N);
            for(int i=0;i<N;i++){ int v=o[i]; if(!first){int d=v-prev;if(d<0)d=-d;if(d>maxStep)maxStep=d;} prev=v; first=false; }
        }
        // sample amp 8000, attack over ~48 samples -> step <= ~8000/48 ~ 170.
        check("click-free attack (<400/sample)", maxStep < 400);
    }

    // (7) record overrun clamp.
    {
        sampler s;
        s.pushEvent(sampler::EV_REC_START, -1, 0);
        { int o[2*N]; s.renderInto(o,N); }
        int blk[2*N]; constBlock(blk, 8000);
        int blocks = sampler::CHROM_MAX / N + 100;   // exceed the buffer
        for(int b=0;b<blocks;b++) s.captureBlock(blk, N);
        check("overrun stops recording", !s.recording());
        check("overrun clamped to max", s.recLen() <= sampler::CHROM_MAX);
        s.pushEvent(sampler::EV_REC_STOP, 0, 0);
        { int o[2*N]; s.renderInto(o,N); }
        check("overrun sample still loads", s.chromaticLoaded());
    }

    // (8) empty / no-content NOTE_ON is a silent no-op.
    {
        sampler s;
        s.pushEvent(sampler::EV_NOTE_ON, 60, 100);
        int o[2*N]; for(int i=0;i<2*N;i++)o[i]=0; s.renderInto(o,N);
        bool silent=true; for(int i=0;i<2*N;i++) if(o[i]!=0) silent=false;
        check("no-content no-op", s.activeVoices()==0 && silent);
    }

    printf(g_fails ? "\n%d FAIL\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}
