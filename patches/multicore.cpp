#ifdef ARM_ALLOW_MULTI_CORE
#include "kernel.h"
#include <audio/AudioSystem.h>
#include <circle/logger.h>

#define IPI_AUDIO_UPDATE 11

CCoreTask *CCoreTask::s_pThis = 0;

CCoreTask::CCoreTask(CKernel *pKernel)
	: CMultiCoreSupport(&pKernel->m_Memory)
{
	s_pThis = this;
}

void CCoreTask::Run(unsigned nCore)
{
	if (nCore == 0)
	{
		while (1)
			CMultiCoreSupport::Halt();
	}
	while (1)
		CMultiCoreSupport::Halt();
}

void CCoreTask::IPIHandler(unsigned nCore, unsigned nIPI)
{
	if (nCore == 1 && nIPI == IPI_AUDIO_UPDATE)
		AudioSystem::doUpdate();
}

#endif
