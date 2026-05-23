#ifdef ARM_ALLOW_MULTI_CORE
#include "kernel.h"
#include <audio/AudioSystem.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include "coreDispatch.h"
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
//   Core 3 — reserved idle (WFE forever).
//
// IPC: SPSC lock-free rings only. No mutexes in audio path.
// Atomic-snapshot publish/load for control→DSP shared params (paramSnapshot).
// Wakeup: SEV from producers, WFE on consumers — no busy-spin.
//
// Audio readiness handshake:
//   Core 0 calls coreSignalAudioReady() once setup() returns from CKernel::Run.
//   Cores 1/2 spin on g_coreAudioReady before entering their work loops.

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
			u32 code;
			while (coreDispatchPop(&code))
			{
				if (code == DISPATCH_AUDIO)
					AudioSystem::doUpdate();
			}
			coreDispatchWait();
		}
	}
	else if (nCore == 2)
	{
		coreWaitAudioReady();
		while (1)
		{
			coreControlPlaneTick();
		}
	}
	else
	{
		while (1) {
			asm volatile ("wfe" ::: "memory");
		}
	}
}

void CCoreTask::IPIHandler(unsigned, unsigned)
{
}

#endif
