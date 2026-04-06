// 11-aLooper

#include <audio/Audio.h>
#ifdef LOOPER_USB_AUDIO
	#include <audio/input_usb.h>
	#include <audio/output_usb.h>
#endif
#include "Looper.h"
#include "LooperVersion.h"
#include <circle/logger.h>

#define log_name "audio"

// You may define one of the following.
// You can also connect to the teensy audio card (STGL500)
// or a teensy running the AudioInputI2SQuad device.

// LOOPER2 uses the CS42448 octo
// LOOPER3 (TE3) uses the TEENSY_QUAD_SLAVE
// LOOPER_USB_AUDIO uses a generic class-compliant USB audio device (e.g. UCA222)
//   at 44100Hz stereo — set AUDIO_BLOCK_SAMPLES=64 in circle-prh AudioConfig.h for low latency

#ifdef LOOPER_USB_AUDIO
	#pragma message("compiling Looper::audio.cpp for LOOPER_USB_AUDIO (UCA222 / generic USB)")
	#define USE_CS42448				0
	#define USE_TEENSY_QUAD_SLAVE	0
	#define USE_STANDARD_I2S		0
	#define USE_USB_AUDIO			1
#elif defined(LOOPER3)
	#pragma message("compiling Looper::audio.cpp for LOOPER3")
	#define USE_CS42448				0		// MUST BE SET FOR ACTUAL OLD LOOPER2 BUILD!!!
	#define USE_TEENSY_QUAD_SLAVE	1
	#define USE_STANDARD_I2S		0
	#define USE_USB_AUDIO			0
#else
	#pragma message("compiling Looper::audio.cpp for LOOPER2")
	#define USE_CS42448				1		// MUST BE SET FOR ACTUAL OLD LOOPER2 BUILD!!!
	#define USE_TEENSY_QUAD_SLAVE	0
	#define USE_STANDARD_I2S		0
	#define USE_USB_AUDIO			0
#endif

// These two are not in mainline use
#define USE_WM8731				0
#define USE_STGL5000			0


#if USE_CS42448

	#pragma message("Looper::audio.cpp using CS42448 (octo)")

	// Octo is always the master
	AudioInputTDM input;
	AudioOutputTDM output;
	AudioControlCS42448 control;

#elif USE_WM8731

	#pragma message("Looper::audio.cpp using WM8731")

	// wm8731 in master or slave mode
	// Connect the outputs to the RCA jacks
	// Connect the input to the microphone jack,
	// or use the control.inputSelect(AUDIO_INPUT_LINEIN);

	#define WM8731_IS_I2S_MASTER    1
		// the rPi is a horrible i2s master.
		// It is much better with the wm831 as the master i2s device

	AudioInputI2S input;
	AudioOutputI2S output;

	#if WM8731_IS_I2S_MASTER
		AudioControlWM8731 control;
	#else
		AudioControlWM8731Slave control;
	#endif

#elif USE_STGL5000  // only in slave mode

	#pragma message("Looper::audio.cpp using STGL5000")

	// the rpi cannot be a master to an sgtl5000.
	// the sgtl5000 requires 3 clocks and the rpi can only generate 2
	AudioInputI2S input;
	AudioOutputI2S output;
	AudioControlSGTL5000 control;

#elif USE_TEENSY_QUAD_SLAVE

	#pragma message("Looper::audio.cpp using TEENSY_QUAD_SLAVE")

	// I am currently using this with a mod to the bcm_pcm::static_init() call

	AudioInputTeensyQuad   input;
	AudioOutputTeensyQuad  output;

#elif USE_STANDARD_I2S

	// prh - the reason this doesn't work at all is that there is
	// no bcm_pcm setup() and crucially, no call to bcm_pcm.begin()!!!

	AudioInputI2S input;
	AudioOutputI2S output;
	#pragma message("Looper::audio.cpp using STANDARD I2S")

#elif USE_USB_AUDIO

	// Generic class-compliant USB audio (e.g. Behringer UCA222).
	// Requires USB host support in circle (lib/usb/usbmidi.cpp etc).
	// Set AUDIO_BLOCK_SAMPLES=64 in circle-prh AudioConfig.h for 64-sample / ~1.5ms latency.
	// The USB audio device must be class-compliant UAC1 (44100Hz stereo 16-bit).
	// AudioInputUSB / AudioOutputUSB are defined in circle-prh audio/AudioInputUSB and
	// audio/AudioOutputUSB — they bridge the USB isochronous transfer to the AudioSystem.

	AudioInputUSB  input;
	AudioOutputUSB output;
	#pragma message("Looper::audio.cpp using USB AUDIO (UCA222 / generic class-compliant)")

#endif



// SINGLE GLOBAL STATIC INSTANCE

loopMachine *pTheLoopMachine = 0;
publicLoopMachine *pTheLooper = 0;


extern "C" void debug_blink(int n);

void setup()
{
	// this is the first line of code executed in the _apps/Looper folder,
	// so we show the version here ...

	debug_blink(10);
	LOG("Looper " LOOPER_VERSION " starting at audio.cpp setup(%dx%d)",
		LOOPER_NUM_TRACKS,
		LOOPER_NUM_LAYERS);
	#ifdef LOOPER3
		LOG("audio.cpp compiled for LOOPER3",0);
	#else
		LOG("audio.cpp compiled for LOOPER2",0);
	#endif

	debug_blink(11);
	pTheLoopMachine = new loopMachine();
	pTheLooper = (publicLoopMachine *) pTheLoopMachine;
	debug_blink(12);

	new AudioConnection(input,			0,  *pTheLooper,	0);
	new AudioConnection(input,			1,  *pTheLooper,	1);
	new AudioConnection(*pTheLooper,	0,  output,			0);
	new AudioConnection(*pTheLooper,	1,  output,			1);
	debug_blink(13);

	AudioSystem::initialize(200);
	debug_blink(14);

	pTheLooper->setControl(LOOPER_CONTROL_OUTPUT_GAIN,0);
	pTheLooper->setControl(LOOPER_CONTROL_INPUT_GAIN,0);
	delay(100);

	#if USE_WM8731
		// some devices do not have these controls
		control.inputSelect(AUDIO_INPUT_LINEIN);
	#endif

	#if USE_STGL5000
		// some devices do not have these controls
		control.setDefaults();
	#endif


	// set all volumes except output
	// and then ramp up the output volume

	for (int i=LOOPER_CONTROL_THRU_VOLUME; i<=LOOPER_CONTROL_MIX_VOLUME; i++)
	{
		pTheLooper->setControl(i,pTheLooper->getControlDefault(i));
	}

	float default_out_val = pTheLooper->getControlDefault(LOOPER_CONTROL_OUTPUT_GAIN);
	float default_in_val = pTheLooper->getControlDefault(LOOPER_CONTROL_INPUT_GAIN);

	// bring the volume up over 1 second
	// the delays seem to help with conflict of Wire.cpp
	// versus rPi touch screen ... it always flails a little
	// bit, but not fatally, here ..

	for (int j=0; j<20; j++)
	{
		u8 in_val = roundf(default_in_val * ((float)j)/20.00);
		u8 out_val = roundf(default_out_val * ((float)j)/20.00 );
		pTheLooper->setControl(LOOPER_CONTROL_INPUT_GAIN,in_val);
		delay(30);
		pTheLooper->setControl(LOOPER_CONTROL_OUTPUT_GAIN,out_val);
		delay(30);
	}

	LOG("aLooper::audio.cpp setup() finished",0);

}



void loop()
{
}
