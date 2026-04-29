#include <audio/Audio.h>
#ifdef LOOPER_USB_AUDIO
	#include <audio/input_usb.h>
	#include <audio/output_usb.h>
#endif
#ifdef LOOPER_OTG_AUDIO
	#include <audio/input_otg.h>
	#include <audio/output_otg.h>
#endif
#include "Looper.h"
#include "LooperVersion.h"
#include "apcKey25.h"
#include <circle/logger.h>
#include <circle/timer.h>
#include "patches/RubberBandWrapper.h"
#include "patches/apcEffectsProcessor.h"
#include "patches/audioTelemetry.h"

#define log_name "audio"

#if defined(LOOPER_USB_AUDIO) && defined(LOOPER_OTG_AUDIO)
	#pragma message("compiling for LOOPER_USB_AUDIO + LOOPER_OTG_AUDIO")
	#define USE_CS42448				0
	#define USE_TEENSY_QUAD_SLAVE	0
	#define USE_STANDARD_I2S		0
	#define USE_USB_AUDIO			1
	#define USE_OTG_AUDIO			1
#elif defined(LOOPER_USB_AUDIO)
	#pragma message("compiling for LOOPER_USB_AUDIO")
	#define USE_CS42448				0
	#define USE_TEENSY_QUAD_SLAVE	0
	#define USE_STANDARD_I2S		0
	#define USE_USB_AUDIO			1
	#define USE_OTG_AUDIO			0
#elif defined(LOOPER3)
	#pragma message("compiling for LOOPER3")
	#define USE_CS42448				0
	#define USE_TEENSY_QUAD_SLAVE	1
	#define USE_STANDARD_I2S		0
	#define USE_USB_AUDIO			0
	#define USE_OTG_AUDIO			0
#else
	#pragma message("compiling for LOOPER2")
	#define USE_CS42448				1
	#define USE_TEENSY_QUAD_SLAVE	0
	#define USE_STANDARD_I2S		0
	#define USE_USB_AUDIO			0
	#define USE_OTG_AUDIO			0
#endif

#define USE_WM8731				0
#define USE_STGL5000			0

#if USE_CS42448
	#pragma message("Looper::audio.cpp using CS42448 (octo)")
	AudioInputTDM input;
	AudioOutputTDM output;
	AudioControlCS42448 control;
#elif USE_WM8731
	#pragma message("Looper::audio.cpp using WM8731")
	#define WM8731_IS_I2S_MASTER    1
	AudioInputI2S input;
	AudioOutputI2S output;
	#if WM8731_IS_I2S_MASTER
		AudioControlWM8731 control;
	#else
		AudioControlWM8731Slave control;
	#endif
#elif USE_STGL5000
	#pragma message("Looper::audio.cpp using STGL5000")
	AudioInputI2S input;
	AudioOutputI2S output;
	AudioControlSGTL5000 control;
#elif USE_TEENSY_QUAD_SLAVE
	#pragma message("Looper::audio.cpp using TEENSY_QUAD_SLAVE")
	AudioInputTeensyQuad   input;
	AudioOutputTeensyQuad  output;
#elif USE_STANDARD_I2S
	AudioInputI2S input;
	AudioOutputI2S output;
	#pragma message("Looper::audio.cpp using STANDARD I2S")
#elif USE_USB_AUDIO
	AudioInputUSB  input;
	AudioOutputUSB output;
	#pragma message("Looper::audio.cpp using USB AUDIO")
#endif

#if USE_OTG_AUDIO
	AudioInputOTG  otgIn;
	AudioOutputOTG otgOut;
#endif

RubberBandWrapper *pLivePitchWrapper = 0;
apcEffectsProcessor *pEffectsProcessor = 0;

loopMachine *pTheLoopMachine = 0;
publicLoopMachine *pTheLooper = 0;

extern "C" void debug_blink(int n);

void setup()
{
	debug_blink(1);
	LOG("Looper " LOOPER_VERSION " starting at audio.cpp setup(%dx%d)",
		LOOPER_NUM_TRACKS,
		LOOPER_NUM_LAYERS);

	debug_blink(1);
	pTheLoopMachine = new loopMachine();
	pTheLooper = (publicLoopMachine *) pTheLoopMachine;

	debug_blink(1);
	pLivePitchWrapper = new RubberBandWrapper(AUDIO_SAMPLE_RATE, LOOPER_NUM_CHANNELS);
	pEffectsProcessor = new apcEffectsProcessor(AUDIO_SAMPLE_RATE);
	// Note: pLivePitchWrapper and pEffectsProcessor are NOT AudioStreams; they're fed directly in loopMachine::update()
	// Audio path: input → loopMachine (which internally feeds pLivePitchWrapper and pEffectsProcessor) → output
	new AudioConnection(input,      0,  *pTheLooper,    0);
	new AudioConnection(input,      1,  *pTheLooper,    1);
	new AudioConnection(*pTheLooper,	0,  output,			0);
	new AudioConnection(*pTheLooper,	1,  output,			1);

	debug_blink(1);
	AudioSystem::initialize(200);
	debug_blink(1);

#if USE_OTG_AUDIO
	otgIn.start();
	otgOut.start();
#endif

	pTheLooper->setControl(LOOPER_CONTROL_OUTPUT_GAIN,0);
	pTheLooper->setControl(LOOPER_CONTROL_INPUT_GAIN,0);
	delay(100);

#if USE_WM8731
	control.inputSelect(AUDIO_INPUT_LINEIN);
#endif
#if USE_STGL5000
	control.setDefaults();
#endif

	for (int i=LOOPER_CONTROL_THRU_VOLUME; i<=LOOPER_CONTROL_MIX_VOLUME; i++)
	{
		pTheLooper->setControl(i,pTheLooper->getControlDefault(i));
	}

	float default_out_val = pTheLooper->getControlDefault(LOOPER_CONTROL_OUTPUT_GAIN);
	float default_in_val = pTheLooper->getControlDefault(LOOPER_CONTROL_INPUT_GAIN);

	for (int j=0; j<20; j++)
	{
		u8 in_val  = roundf(default_in_val  * ((float)j)/20.00);
		u8 out_val = roundf(default_out_val * ((float)j)/20.00);
		pTheLooper->setControl(LOOPER_CONTROL_INPUT_GAIN,in_val);
		delay(30);
		pTheLooper->setControl(LOOPER_CONTROL_OUTPUT_GAIN,out_val);
		delay(30);
	}

	new apcKey25();
	LOG("aLooper::audio.cpp setup() finished",0);
}

#if USE_USB_AUDIO
extern volatile unsigned g_inLastTicks;
extern volatile unsigned g_inUnderruns;
extern volatile unsigned g_inResyncs;
extern volatile int      g_inLastRateStep;
extern volatile unsigned g_outUnderruns;
extern volatile unsigned g_otgResyncs;
extern volatile int      g_otgLastRateStep;
extern unsigned AudioInputUSB_inAvail (void);
extern unsigned AudioOutputUSB_outAvail (void);
extern volatile unsigned g_midiOutDropped;
extern volatile unsigned g_midiOutErrors;

static unsigned s_watchdogForces  = 0;
static unsigned s_lastStatTicks   = 0;
#define USB_WATCHDOG_TICKS  (CLOCKHZ / 200)
#define USB_STAT_TICKS      CLOCKHZ
#endif

void loop()
{
	if (pTheLooper) {
		logString_t *msg;
		while ((msg = pTheLooper->getNextLogString()) != nullptr) {
			CLogger::Get()->Write(msg->lname, LogNotice, *msg->string);
			delete msg->string;
			delete msg;
		}
	}
#if USE_USB_AUDIO
	unsigned now = CTimer::GetClockTicks();
	if (g_inLastTicks && (now - g_inLastTicks) > USB_WATCHDOG_TICKS)
	{
		audioTelemetryPush(TELEM_WATCHDOG, now - g_inLastTicks);
		AudioSystem::startUpdate();
		g_inLastTicks = now;
		s_watchdogForces++;
	}

	// Drain ISR-safe event ring on main thread. Events are timestamped at
	// push time; logging happens here where CLogger UDP send is safe.
	AudioTelemEvent ev;
	unsigned drained = 0;
	while (drained < 32 && audioTelemetryPop(&ev))
	{
		const char *name = "?";
		switch (ev.code) {
			case TELEM_IN_UNDERRUN:   name = "IN_UR";   break;
			case TELEM_IN_RESYNC:     name = "IN_RS";   break;
			case TELEM_OUT_UNDERRUN:  name = "OUT_UR";  break;
			case TELEM_OTG_RESYNC:    name = "OTG_RS";  break;
			case TELEM_WATCHDOG:      name = "WD";      break;
			case TELEM_LAG_SAMPLE:    name = "LAG";     break;
			default: break;
		}
		CLogger::Get()->Write(log_name, LogNotice, "telem t=%u %s arg=%u",
			ev.ticks, name, ev.arg);
		drained++;
	}

	if ((now - s_lastStatTicks) > USB_STAT_TICKS * 2)   // 2 Hz summary
	{
		s_lastStatTicks = now;
		static unsigned prev_inUR=0, prev_outUR=0, prev_inRS=0, prev_otgRS=0;
		static unsigned prev_wd=0, prev_drop=0;
		unsigned inAv  = AudioInputUSB_inAvail();
		unsigned outAv = AudioOutputUSB_outAvail();
		unsigned d_inUR  = g_inUnderruns - prev_inUR;
		unsigned d_outUR = g_outUnderruns - prev_outUR;
		unsigned d_inRS  = g_inResyncs - prev_inRS;
		unsigned d_otgRS = g_otgResyncs - prev_otgRS;
		unsigned d_wd    = s_watchdogForces - prev_wd;
		unsigned d_drop  = g_telemDropped - prev_drop;
		bool any = d_inUR | d_outUR | d_inRS | d_otgRS | d_wd | d_drop;
		if (any)
		{
			CLogger::Get()->Write(log_name, LogNotice,
				"stat in_av=%u out_av=%u in_ur+%u out_ur+%u in_rs+%u otg_rs+%u wd+%u drop+%u",
				inAv, outAv, d_inUR, d_outUR, d_inRS, d_otgRS, d_wd, d_drop);
		}
		prev_inUR=g_inUnderruns; prev_outUR=g_outUnderruns;
		prev_inRS=g_inResyncs;   prev_otgRS=g_otgResyncs;
		prev_wd=s_watchdogForces; prev_drop=g_telemDropped;
	}
#endif
	if (pTheAPC) pTheAPC->update();
}
