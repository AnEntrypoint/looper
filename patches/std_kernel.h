//
// std_kernel.h
//
// A standard kernel that:
//
// - Implements the basic UI timeSlice() loop.
// - also implements arduino like setup() and loop()
//   calls to minimize changes to teensy audio library test programs.
//
// Based on Circle - A C++ bare metal environment for Raspberry Pi
// Copyright (c) 2015-2016  R. Stange <rsta2@o2online.de>
// Copyright (c) 2019 Patrick Horton- no rights reserved,
// please see license details in LICENSE.TXT

#ifndef _std_kernel_h
#define _std_kernel_h

#define USE_UI_SYSTEM 		1
#define USE_AUDIO_SYSTEM 	1
#define USE_MIDI_SYSTEM     0

#define USE_SCREEN  	 	1
#ifdef LOOPER_USB_AUDIO
	#define USE_USB          	1
#else
	#define USE_USB          	0
#endif
#define USE_MINI_SERIAL  	0
#define USE_MAIN_SERIAL  	1
#define USE_FILE_SYSTEM     1

#if defined(LOOPER3) || defined(LOOPER_USB_AUDIO)
	#define USE_ILI_TFT			9488
	#define USE_XPT2046			1
#else
	#define USE_ILI_TFT			0
	#define USE_XPT2046			0
#endif

#define LOG_TO_SCREEN  			0
#define LOG_TO_MINI_UART		1
#define LOG_TO_MAIN_SERIAL		2

#define USE_LOG_TO				LOG_TO_MAIN_SERIAL

#include <circle/memory.h>
#include <utils/myActLED.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#if USE_SCREEN
	#include <circle/screen.h>
#endif
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#if USE_MAIN_SERIAL
	#include <circle/serial.h>
#endif
#if USE_MINI_SERIAL
	#include <circle/miniuart.h>
#endif
#include <circle/logger.h>
#include <circle/sched/scheduler.h>

#if USE_USB
	#include <circle/usb/dwhcidevice.h>
#endif

#if USE_UI_SYSTEM
	#include <ws/wsApp.h>
#endif

#if USE_ILI_TFT
	#if USE_ILI_TFT == 9488
		#include <devices/ili9488.h>
	#elif USE_ILI_TFT == 9486
		#include <devices/ili9486.h>
	#endif
	#if USE_XPT2046
		#include <devices/xpt2046.h>
	#endif
#else
	#include <circle/input/touchscreen.h>
#endif

#if USE_FILE_SYSTEM
	#include <SDCard/emmc.h>
	#include <fatfs/ff.h>
#endif

#if USE_MIDI_SYSTEM
	#include "midiEvent.h"
#endif

#ifdef ARM_ALLOW_MULTI_CORE
#define WITH_MULTI_CORE
#endif

#ifdef WITH_MULTI_CORE
	#include <circle/multicore.h>
	#define CORE_FOR_AUDIO_SYSTEM    1
	#define CORE_FOR_UI_SYSTEM       2
#else
	#define CORE_FOR_AUDIO_SYSTEM    0
	#define CORE_FOR_UI_SYSTEM       0
#endif

#if CORE_FOR_AUDIO_SYSTEM != 0
	#define IPI_AUDIO_UPDATE  11
#endif

class CKernel;
class CCoreTask
	#ifdef WITH_MULTI_CORE
		: public CMultiCoreSupport
	#endif
{
	public:
		CCoreTask(CKernel *pKernel);
		~CCoreTask();
		void Run(unsigned nCore);
		static CCoreTask *Get() {return s_pCoreTask;}
		CKernel *GetKernel() { return m_pKernel; }

		#if USE_AUDIO_SYSTEM
			#if CORE_FOR_AUDIO_SYSTEM != 0
				void IPIHandler(unsigned nCore, unsigned nIPI);
			#endif
		#endif

		#if USE_FILE_SYSTEM
			FATFS *GetFileSystem();
		#endif

	private:
		CKernel *m_pKernel;
		static CCoreTask *s_pCoreTask;

	#if USE_AUDIO_SYSTEM
		void runAudioSystem(unsigned nCore, bool init);
		volatile bool m_bAudioStarted;
	#endif

	#if USE_UI_SYSTEM
		void runUISystem(unsigned nCore, bool init);
		volatile bool m_bUIStarted;
	#endif
};

enum TShutdownMode { ShutdownNone, ShutdownHalt, ShutdownReboot };

class CKernel
{
public:
	CKernel (void);
	~CKernel (void);
	boolean Initialize (void);
	TShutdownMode Run (void);

	#if USE_MAIN_SERIAL
		CSerialDevice *GetSerial()  { return &m_Serial; }
	#endif
	#if USE_XPT2046
		XPT2046 *GetXPT2046() { return &m_xpt2046; }
	#endif
	#if USE_ILI_TFT
		ILISPI_CLASS *getILISPI() { return &m_SPI; }
		#if USE_ILI_TFT == 9488
			ILI9488 *getILITFT() { return &m_tft; }
		#elif USE_ILI_TFT == 9486
			ILI9886 *getILITFT() { return &m_tft; }
		#endif
	#endif

private:
	friend class CCoreTask;

	CMemorySystem		m_Memory;
	myActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;

	#if USE_SCREEN
		CScreenDevice	m_Screen;
	#endif
	#if USE_MINI_SERIAL
		CMiniUartDevice m_MiniUart;
	#endif
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer				m_Timer;
	#if USE_MAIN_SERIAL
		CSerialDevice	m_Serial;
	#endif
	CLogger				m_Logger;
	CScheduler			m_Scheduler;

	#if USE_USB
		CDWHCIDevice	m_DWHCI;
	#endif

	#if USE_UI_SYSTEM
		wsApplication 	m_app;
	#endif

	#if USE_ILI_TFT
		ILISPI_CLASS	m_SPI;
		#if USE_ILI_TFT == 9488
			ILI9488 	m_tft;
		#elif USE_ILI_TFT == 9486
			ILI9886 	m_tft;
		#endif
		#if USE_XPT2046
			XPT2046  m_xpt2046;
		#endif
	#else
		CTouchScreenDevice	m_TouchScreen;
	#endif

	CCoreTask 	m_CoreTask;

	#if USE_FILE_SYSTEM
		CEMMCDevice			m_EMMC;
		FATFS				m_FileSystem;
		void initFileSystem();
	#endif

	#if USE_MIDI_SYSTEM
		midiSystem m_MidiSystem;
	#endif
};

#endif
