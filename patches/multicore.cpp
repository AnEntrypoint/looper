#ifdef ARM_ALLOW_MULTI_CORE
#include "kernel.h"
#include <audio/AudioSystem.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include "coreDispatch.h"
#include "coreBusy.h"
#include "paramSnapshot.h"

// 4-core partition for rPi4:
//   Core 0 — hard-RT dispatch: USB ISRs (GIC routes here), AudioSystem::startUpdate
//            enqueues + SEV; main loop (CKernel::Run) only handles reboot poll.
//   Core 1 — DSP worker: WFE-blocked, drains coreDispatch audio jobs, runs
//            AudioSystem::doUpdate (which fires the entire audio graph including
//            loopMachine, RubberBand feed/retrieve, signalsmith/granular octaver,
//            apcEffectsProcessor).
//   Core 2 — control plane: USB plug-and-play, Net.Process, usbMidiProcess,
//            audio.cpp::loop (telemetry drain + watchdog), pTheAPC->update,
//            linkProcess, WiFi DHCP, pollSockets, m_Scheduler.Yield.
//   Core 3 — reserved idle (WFE forever) + busy/idle accounting hook so we can
//            verify "truly idle" via per-core busy% telemetry from Core 2.
//
// IPC: SPSC lock-free rings only. No mutexes in audio path.
// Atomic-snapshot publish/load for control→DSP shared params (paramSnapshot).
// Wakeup: SEV from producers, WFE on consumers — no busy-spin.
//
// Audio readiness handshake:
//   Core 0 calls coreSignalAudioReady() once setup() returns from CKernel::Run.
//   Cores 1/2/3 spin on g_coreAudioReady before entering their work loops.
//
// Telemetry: each core's loop bookends doWork() / wfe() with CTimer::GetClockTicks()
// reads to accumulate busy/idle totals. Core 2's control plane samples these at
// 0.5Hz and emits a log line showing busy% per core. Confirms (a) Core 1 isn't
// saturated, (b) Core 3 is truly idle, (c) DSP work isn't leaking to Core 2.

CCoreTask *CCoreTask::s_pThis = 0;

volatile bool g_coreAudioReady = false;

extern void loop(void);                                    // audio.cpp
extern void usbMidiProcess(bool bPlugAndPlayUpdated);      // usbMidi.cpp
extern void linkProcess(void);                              // abletonLink.cpp
extern bool wlanDhcpPoll(class CBcm4343Device *);          // wlanDHCP.cpp
extern void wlanDhcpServe(void);                            // wlanDHCPServer.cpp
extern void coreControlPlaneTick(void);                    // kernel_run.cpp

CCoreTask::CCoreTask(CKernel *pKernel)
	: CMultiCoreSupport(&pKernel->m_Memory)
{
	s_pThis = this;
}

void coreSignalAudioReady(void)
{
	DataMemBarrier();
	g_coreAudioReady = true;
	DataMemBarrier();
	asm volatile ("sev" ::: "memory");
}

static inline void coreWaitAudioReady(void)
{
	while (!g_coreAudioReady) {
		asm volatile ("wfe" ::: "memory");
	}
	DataMemBarrier();
}

void CCoreTask::Run(unsigned nCore)
{
	if (nCore == 1)
	{
		coreWaitAudioReady();
		while (1)
		{
			u64 t0 = CTimer::GetClockTicks();
			u32 code;
			while (coreDispatchPop(&code))
			{
				if (code == DISPATCH_AUDIO)
					AudioSystem::doUpdate();
			}
			u64 t1 = CTimer::GetClockTicks();
			g_coreBusyTicks[1] += t1 - t0;
			coreDispatchWait();          // WFE — blocks until SEV
			u64 t2 = CTimer::GetClockTicks();
			g_coreIdleTicks[1] += t2 - t1;
		}
	}
	else if (nCore == 2)
	{
		coreWaitAudioReady();
		while (1)
		{
			u64 t0 = CTimer::GetClockTicks();
			coreControlPlaneTick();
			u64 t1 = CTimer::GetClockTicks();
			g_coreBusyTicks[2] += t1 - t0;
			// coreControlPlaneTick() yields internally; the time between
			// returning and the next call is logically idle.
		}
	}
	else  // Core 3 — reserved idle
	{
		while (1) {
			u64 t0 = CTimer::GetClockTicks();
			asm volatile ("wfe" ::: "memory");
			u64 t1 = CTimer::GetClockTicks();
			g_coreIdleTicks[3] += t1 - t0;
		}
	}
}

void CCoreTask::IPIHandler(unsigned, unsigned)
{
}

#endif
