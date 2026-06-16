// p9 error-stack self-heal invariants (patches/p9error.cpp). The plan9 WLAN
// driver's error() uses a setjmp/longjmp stack; stackptr is a DOWN-counter seeded
// to ERROR_STACK_SIZE (free-slot count). pusherror() does --stackptr, and
// error()/nexterror()/poperror() do ++stackptr. A leaked poperror() (pop with
// nothing pushed) drove stackptr past the seed; the upstream assert(stackptr <
// SIZE) then unsigned-wrapped and HALTED the box ~90s in -- the documented reason
// WLAN/Ableton-Link was disabled. The self-heal clamps/reseeds instead of
// asserting. This test proves: under any push/pop/error sequence (including
// adversarial leaks and overflows) the slot index used for longjmp ALWAYS stays
// in [0, ERROR_STACK_SIZE), stackptr never wraps, and a balanced sequence nets to
// the empty baseline -- i.e. the leak degrades to a bounded reseed, never a halt.
//
// Mirrors patches/p9error.cpp exactly. Build:
//   g++ -O2 -std=c++17 scripts/test-p9-errorstack.cpp -o scripts/test-p9-errorstack.exe
#include <cstdio>
#include <cstdint>
typedef uint32_t u32;
static int g_fails = 0;
static void check(const char* n, bool c){ if(c)printf("ok: %s\n",n); else {printf("FAIL: %s\n",n);g_fails++;} }

static const unsigned ERROR_STACK_SIZE = 30;

// Model of the error stack. stackptr is the free-slot down-counter.
struct ErrStack {
    unsigned stackptr = ERROR_STACK_SIZE;   // p9proc_init seeds to SIZE (empty)
    bool bad = false;                       // set if any slot index ever OOB
};

static inline unsigned clamp_top(unsigned p) {
    if (p == 0) return 1;
    if (p > ERROR_STACK_SIZE) return ERROR_STACK_SIZE;
    return p;
}

// Records every slot index actually dereferenced; flags OOB.
static void useSlot(ErrStack& s, unsigned idx) {
    if (idx >= ERROR_STACK_SIZE) s.bad = true;   // would index stack[OOB]
}

// pusherror(): return &stack[--stackptr], with 0->1 wrap guard.
static void pusherror(ErrStack& s) {
    if (s.stackptr == 0) s.stackptr = 1;
    --s.stackptr;
    useSlot(s, s.stackptr);                       // &stack[stackptr] after decrement
}

// error()/nexterror(): reseed+swallow if empty, else ++ and longjmp stack[ptr-1].
// returns true if it "jumped" (had a handler), false if swallowed.
static bool errorJump(ErrStack& s) {
    if (s.stackptr >= ERROR_STACK_SIZE) { s.stackptr = ERROR_STACK_SIZE; return false; }
    s.stackptr = clamp_top(s.stackptr + 1);
    useSlot(s, s.stackptr - 1);
    return true;
}

// poperror(): clamp at empty mark, else ++.
static void poperror(ErrStack& s) {
    if (s.stackptr >= ERROR_STACK_SIZE) { s.stackptr = ERROR_STACK_SIZE; return; }
    s.stackptr++;
}

static bool inRange(const ErrStack& s) { return s.stackptr <= ERROR_STACK_SIZE && !s.bad; }

int main() {
    // 1. Balanced waserror()/poperror() nets back to the empty baseline.
    {
        ErrStack s;
        bool ranged = true;
        for (int i = 0; i < 1000; i++) {
            pusherror(s); if (!inRange(s)) ranged = false;
            poperror(s);  if (!inRange(s)) ranged = false;
        }
        check("balanced push/pop stays in range", ranged);
        check("balanced push/pop nets to empty baseline", s.stackptr == ERROR_STACK_SIZE && !s.bad);
    }

    // 2. Leak storm: poperror() with nothing pushed (the original halt trigger).
    //    Must clamp at the seed, never wrap, never OOB.
    {
        ErrStack s;
        for (int i = 0; i < 100000; i++) poperror(s);
        check("leaked poperror() clamps at seed (no unsigned wrap)", s.stackptr == ERROR_STACK_SIZE);
        check("leaked poperror() never indexes OOB", !s.bad);
    }

    // 3. error() storm on empty stack: each swallows (no jump) and stays clamped.
    {
        ErrStack s;
        bool anyJump = false;
        for (int i = 0; i < 100000; i++) if (errorJump(s)) anyJump = true;
        check("error() on empty stack swallows (no longjmp into garbage)", !anyJump);
        check("error() storm stays at empty baseline, no OOB", s.stackptr == ERROR_STACK_SIZE && !s.bad);
    }

    // 4. Overflow: push far beyond capacity. stackptr must never wrap below 0,
    //    slot index never OOB; reuses slot 0 at the floor.
    {
        ErrStack s;
        for (int i = 0; i < 100000; i++) pusherror(s);
        check("overflow push never wraps below 0", s.stackptr == 0);
        check("overflow push never indexes OOB", !s.bad);
    }

    // 5. Adversarial mix: deep nest, then a longjmp (error) that should unwind,
    //    then more pops than pushes. Index always in [0, SIZE), stackptr bounded.
    {
        ErrStack s;
        bool ranged = true;
        // deep nest to capacity+margin
        for (int i = 0; i < (int)ERROR_STACK_SIZE + 5; i++) { pusherror(s); if (!inRange(s)) ranged = false; }
        // an error fires (jumps to top handler), then handler pops a few
        errorJump(s); if (!inRange(s)) ranged = false;
        for (int i = 0; i < 50; i++) { poperror(s); if (!inRange(s)) ranged = false; }
        // resume normal balanced traffic
        for (int i = 0; i < 500; i++) { pusherror(s); errorJump(s); poperror(s); if (!inRange(s)) ranged = false; }
        check("adversarial nest/jump/over-pop stays bounded, never OOB", ranged && !s.bad);
        check("adversarial sequence converges to a valid stackptr", s.stackptr <= ERROR_STACK_SIZE);
    }

    printf(g_fails ? "\n%d FAIL\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}
