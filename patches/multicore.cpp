#ifdef ARM_ALLOW_MULTI_CORE
#include "kernel.h"
#include <audio/AudioSystem.h>

#define IPI_AUDIO_UPDATE 11

CCoreTask *CCoreTask::s_pThis = 0;

CCoreTask::CCoreTask(CKernel *pKernel)
	: CMultiCoreSupport(&pKernel->m_Memory)
{
	s_pThis = this;
}

void CCoreTask::Run(unsigned nCore)
{
	while (1)
		;
}

void CCoreTask::IPIHandler(unsigned nCore, unsigned nIPI)
{
	if (nCore == 1 && nIPI == IPI_AUDIO_UPDATE)
		AudioSystem::doUpdate();
}

#endif
