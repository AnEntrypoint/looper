// patches/p9error.cpp — leak-tolerant plan9 error-stack for circle/addon/wlan.
//
// Upstream version asserts (assert(stackptr < ERROR_STACK_SIZE)) in error(),
// nexterror() and poperror(). stackptr is a DOWN-counter seeded to
// ERROR_STACK_SIZE (= number of free slots): pusherror() does --stackptr,
// poperror()/error()/nexterror() do ++stackptr. A balanced waserror()/poperror()
// nets to zero, so a single missing poperror() on some hot path inside the
// vendored ether4330/p9proc code slowly drives stackptr DOWN past 0. Unsigned
// wrap then makes (stackptr < ERROR_STACK_SIZE) false, the assert fires, and the
// whole box halts ~90s after boot under audio load — taking WiFi/Ableton-Link
// with it. (Symptom logged at p9error.cpp:43.)
//
// These error() longjmps are non-fatal control flow: every CBcm4343Device call
// site (SendFrame/Control/SetMulticastFilter/attach) just returns FALSE when the
// jump fires. So instead of asserting on imbalance we SELF-HEAL: clamp stackptr
// to the valid [0, ERROR_STACK_SIZE) slot range and, on detected underflow
// (nothing left to pop), re-seed to the empty baseline. WiFi stays up
// indefinitely; a leak degrades to a harmless reseed instead of a halt.
//
// HARDENING (WLAN re-enable prep): error() previously called print() on EVERY
// longjmp. print() -> CLogger::Write(LogDebug) is a SYSLOG UDP write — blocking
// I/O. AGENTS.md's load-bearing rule is "never do blocking I/O on the hot path;
// CLogger::Write (syslog UDP) causes periodic audio gaps / wedges the control
// plane". error() IS a hot path (it fires on every transient SDIO/driver hiccup,
// and storms during a stackptr leak), so the per-error syslog write is exactly
// the blocking-UDP wedge the WLAN-disable comments blamed. Since the longjmp is
// non-fatal control flow, the log line is pure liability: REMOVED. Replaced with
// a free-running, non-blocking counter (p9ErrorCount) so the error rate stays
// observable (telemetry/poll) without any I/O on the error path.
//
// Wired into both build paths (scripts/build-local.ps1 + .github/workflows/
// build.yml) as a copy over circle/addon/wlan/p9error.cpp.

#include "p9error.h"
#include "p9proc.h"
#include "p9util.h"

// Non-blocking observability: count error() longjmps without touching syslog.
// Read via p9ErrorCount() from the control plane (e.g. the :4445 WLAN verb).
static volatile unsigned g_p9ErrorCount = 0;
extern "C" unsigned p9ErrorCount (void) { return g_p9ErrorCount; }

// Largest valid slot index for longjmp target = ERROR_STACK_SIZE - 1.
static inline unsigned clamp_top (unsigned p)
{
	// p is the post-increment value used as stack[p-1]; keep p-1 in range.
	if (p == 0) return 1;                       // would index stack[-1]
	if (p > ERROR_STACK_SIZE) return ERROR_STACK_SIZE;
	return p;
}

void error (const char *str)
{
	// NO print() here — see HARDENING note above. error() is non-fatal control
	// flow on a hot path; a syslog UDP write per longjmp wedges the control plane.
	g_p9ErrorCount++;

	up->errstr = str;

	error_stack_t *s = get_error_stack ();
	if (s == 0) return;

	// Underflow guard: if the down-counter has leaked past the seed (nothing
	// was actually pushed), reseed to empty rather than longjmp into garbage.
	if (s->stackptr >= ERROR_STACK_SIZE)
	{
		s->stackptr = ERROR_STACK_SIZE;         // empty: no handler to jump to
		return;                                 // swallow — caller stays on
	}

	s->stackptr = clamp_top (s->stackptr + 1);
	longjmp (s->stack[s->stackptr - 1], 1);
}

jmp_buf *pusherror (void)
{
	error_stack_t *s = get_error_stack ();
	if (s == 0)
	{
		// No proc context: hand back a dummy slot so setjmp() is harmless.
		static jmp_buf dummy;
		return &dummy;
	}

	// Overflow guard: don't run the free-slot index below 0 (unsigned wrap).
	if (s->stackptr == 0)
		s->stackptr = 1;                        // reuse slot 0 rather than wrap

	return &s->stack[--s->stackptr];
}

void nexterror (void)
{
	error_stack_t *s = get_error_stack ();
	if (s == 0) return;

	if (s->stackptr >= ERROR_STACK_SIZE)
	{
		s->stackptr = ERROR_STACK_SIZE;
		return;
	}

	s->stackptr = clamp_top (s->stackptr + 1);
	longjmp (s->stack[s->stackptr - 1], 1);
}

void poperror (void)
{
	error_stack_t *s = get_error_stack ();
	if (s == 0) return;

	// A pop with nothing pushed must NOT drive stackptr above the seed (that is
	// the leak that eventually trips the old assert). Clamp at the empty mark.
	if (s->stackptr >= ERROR_STACK_SIZE)
	{
		s->stackptr = ERROR_STACK_SIZE;
		return;
	}

	s->stackptr++;
}

void okay (int status)
{
	(void) status;
}
