#ifndef _std_kernel_h
#define _std_kernel_h

#include <circle/sysconfig.h>

#define USE_UI_SYSTEM		0
#define USE_AUDIO_SYSTEM	1
#define USE_MIDI_SYSTEM		0
#define USE_SCREEN		0
#define USE_USB			1
#define USE_MINI_SERIAL		0
#define USE_MAIN_SERIAL		1
#define USE_FILE_SYSTEM		1
#define USE_ILI_TFT		0
#define USE_XPT2046		0

#ifdef ARM_ALLOW_MULTI_CORE
#define CORE_FOR_AUDIO_SYSTEM	1
#define IPI_AUDIO_UPDATE	11
class CCoreTask {
public:
	static CCoreTask *Get();
	void SendIPI(unsigned nCore, unsigned nIPI);
};
#else
#define CORE_FOR_AUDIO_SYSTEM	0
#endif

#define CORE_FOR_UI_SYSTEM	2

#include <circle/types.h>
#include <circle/logger.h>

#endif
