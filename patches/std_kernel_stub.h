#ifndef _std_kernel_h
#define _std_kernel_h

// Minimal stub for rsta2/circle build (headless, no display)
// Replaces phorton1 std_kernel.h to avoid CScreenDeviceBase/ws/ dependency

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

#define CORE_FOR_AUDIO_SYSTEM	1
#define CORE_FOR_UI_SYSTEM	2

#include <circle/types.h>
#include <circle/logger.h>

#endif
