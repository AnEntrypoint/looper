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
#include "patches/microRepeat.h"
#include "patches/sampler.h"
#include "patches/gamepadInput.h"
#include "patches/audioTelemetry.h"
#ifdef ARM_ALLOW_MULTI_CORE
#include "patches/coreBusy.h"
#include "usbWavRecorder.h"
#include "loopDump.h"
#endif

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
microRepeat *pMicroRepeat = 0;
sampler *pSampler = 0;

// On-demand engine query/control entry point, called from the Core-2 debug
// socket (kernel_run pollSockets). Returns reply length, or 0 to fall through
// to the legacy link/bpm reply. Only GET/SET requests are handled here. Zero
// audio-path impact: it just reads/writes plain engine fields on request.
extern "C" int engineQueryDispatch(const char *req, char *out, int outsz)
{
	if (!pLivePitchWrapper || !req || !out) return 0;
	if (!((req[0]=='G'||req[0]=='S') && req[1]=='E' && req[2]=='T')) return 0;
	pLivePitchWrapper->engineQuery(req, out, outsz);
	int len = 0; while (len < outsz && out[len]) len++;
	return len;
}

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
	pTheLoopMachine = new loopMachine();   // ctor runs cbInit() (continuous buffer ready)
	pTheLooper = (publicLoopMachine *) pTheLoopMachine;
	usbWavInit();   // continuous ring-WAV dump to a USB drive (auto-detect, Core-2 drained)
	loopDumpInit(); // per-track WAV dump-on-demand (APC note 93), reuses the USB: mount above

	debug_blink(1);
	pLivePitchWrapper = new RubberBandWrapper(AUDIO_SAMPLE_RATE, LOOPER_NUM_CHANNELS);
	pEffectsProcessor = new apcEffectsProcessor(AUDIO_SAMPLE_RATE);
	pMicroRepeat = new microRepeat();
	pSampler = new sampler();
	pTheGamepad = new gamepadInput();   // USB gamepad control surface (Core-2 driven)
	// Note: pLivePitchWrapper and pEffectsProcessor are NOT AudioStreams; they're fed directly in loopMachine::update()
	// Audio path: input → loopMachine (which internally feeds pLivePitchWrapper and pEffectsProcessor) → output
	// MONO: single connection per stage; LOOPER_NUM_CHANNELS=1 sizes every
	// AudioStream's port count, so only port 0 exists on any of these nodes.
	new AudioConnection(input,      0,  *pTheLooper,    0);
	new AudioConnection(*pTheLooper,	0,  output,			0);

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
#ifdef ARM_ALLOW_MULTI_CORE
extern volatile unsigned g_dispatchDropped;
#endif

static unsigned s_watchdogForces  = 0;
static unsigned s_lastStatTicks   = 0;
static u64      s_lastEngTicks    = 0;
#define USB_WATCHDOG_TICKS  (CLOCKHZ / 200)
#define USB_STAT_TICKS      CLOCKHZ
// Free-run interval ~= one audio block at 48kHz (64 samples) in CTimer micros.
#define USB_FREERUN_TICKS   (CLOCKHZ * AUDIO_BLOCK_SAMPLES / 48000)
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
	else if (!g_inLastTicks)
	{
		// No USB-IN device has EVER delivered (only an unsupported UAC2 device, or
		// no audio interface at all). The IN ring-position push that normally
		// drives startUpdate never fires, so without this the whole audio engine
		// freezes -- cbwr never advances, no loops/sampler/effects/VU. Free-run the
		// graph at ~block rate so the engine stays alive (input silent) until a
		// real device appears; the per-block push takes over the moment it does.
		static unsigned s_lastFreeRun = 0;
		if ((now - s_lastFreeRun) >= USB_FREERUN_TICKS)
		{
			s_lastFreeRun = now;
			AudioSystem::startUpdate();
			s_watchdogForces++;
		}
	}

	// Drain ISR-safe event ring on main thread. CRITICAL: per-event logging
	// was causing audible glitches because each CLogger::Write blocks Core 2
	// on UDP syslog. A single IN ring underrun emits up to 64 TELEM_IN_UNDERRUN
	// events (one per sample in the empty block), drained 32-at-a-time → 32
	// blocking writes back-to-back. The logging WAS amplifying the glitch.
	//
	// New scheme: drain silently, accumulate per-code stats with min/max args,
	// then emit ONE summary line per code per 500ms stat tick (or when count
	// crosses a 32-event high-water mark). Real underrun/resync events still
	// fully visible via the counters (g_inUnderruns etc) shown in cores stat.
	static unsigned s_telCounts[8] = {0};
	static u32 s_telMinArg[8] = {0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu,
	                              0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu};
	static u32 s_telMaxArg[8] = {0};
	AudioTelemEvent ev;
	unsigned drained = 0;
	while (drained < 256 && audioTelemetryPop(&ev))
	{
		unsigned code = ev.code & 0x7;
		s_telCounts[code]++;
		if (ev.arg < s_telMinArg[code]) s_telMinArg[code] = ev.arg;
		if (ev.arg > s_telMaxArg[code]) s_telMaxArg[code] = ev.arg;
		drained++;
	}
	// Burst alarm: if we drained the cap, the ring was overflowing — emit
	// ONE diagnostic line so the operator knows there was a flood.
	static u64 s_lastBurstLogTicks = 0;
	if (drained >= 256 && (now - s_lastBurstLogTicks) > USB_STAT_TICKS) {
		CLogger::Get()->Write(log_name, LogNotice,
			"telem burst — drained 256 events this tick (more queued)");
		s_lastBurstLogTicks = now;
	}

	if ((now - s_lastStatTicks) > USB_STAT_TICKS * 2)   // 2 Hz summary
	{
		s_lastStatTicks = now;
		static unsigned prev_inUR=0, prev_outUR=0, prev_inRS=0, prev_otgRS=0;
		static unsigned prev_wd=0, prev_drop=0;
#ifdef ARM_ALLOW_MULTI_CORE
		static unsigned prev_disp=0;
#endif
		unsigned inAv  = AudioInputUSB_inAvail();
		unsigned outAv = AudioOutputUSB_outAvail();
		unsigned d_inUR  = g_inUnderruns - prev_inUR;
		unsigned d_outUR = g_outUnderruns - prev_outUR;
		unsigned d_inRS  = g_inResyncs - prev_inRS;
		unsigned d_otgRS = g_otgResyncs - prev_otgRS;
		unsigned d_wd    = s_watchdogForces - prev_wd;
		unsigned d_drop  = g_telemDropped - prev_drop;
#ifdef ARM_ALLOW_MULTI_CORE
		unsigned d_disp  = g_dispatchDropped - prev_disp;
#else
		unsigned d_disp  = 0;
#endif
		bool any = d_inUR | d_outUR | d_inRS | d_otgRS | d_wd | d_drop | d_disp;
		if (any)
		{
			// Include min/max args from the silent telemetry drainer so
			// the operator still sees the diagnostic detail (was per-event
			// log lines before — that itself caused glitches). Args of zero
			// for IN_UR mean "ring fully empty when block sampled" — i.e.,
			// the underrun fallback (repeat-last-sample) fired this block.
			CLogger::Get()->Write(log_name, LogNotice,
				"stat in_av=%u out_av=%u in_ur+%u(arg=%u..%u) out_ur+%u in_rs+%u(arg=%u..%u) otg_rs+%u wd+%u drop+%u disp+%u",
				inAv, outAv,
				d_inUR,
				s_telCounts[TELEM_IN_UNDERRUN]  ? s_telMinArg[TELEM_IN_UNDERRUN]  : 0u,
				s_telCounts[TELEM_IN_UNDERRUN]  ? s_telMaxArg[TELEM_IN_UNDERRUN]  : 0u,
				d_outUR,
				d_inRS,
				s_telCounts[TELEM_IN_RESYNC]    ? s_telMinArg[TELEM_IN_RESYNC]    : 0u,
				s_telCounts[TELEM_IN_RESYNC]    ? s_telMaxArg[TELEM_IN_RESYNC]    : 0u,
				d_otgRS, d_wd, d_drop, d_disp);
		}
		// Reset per-stat-tick min/max accumulators.
		for (int i = 0; i < 8; i++) {
			s_telCounts[i] = 0;
			s_telMinArg[i] = 0xFFFFFFFFu;
			s_telMaxArg[i] = 0;
		}
		prev_inUR=g_inUnderruns; prev_outUR=g_outUnderruns;
		prev_inRS=g_inResyncs;   prev_otgRS=g_otgResyncs;
		prev_wd=s_watchdogForces; prev_drop=g_telemDropped;
#ifdef ARM_ALLOW_MULTI_CORE
		prev_disp=g_dispatchDropped;
		// Per-core busy% over the 2Hz interval. busy / (busy+idle) * 100.
		// Confirms (a) Core 1 not saturated, (b) Core 3 truly idle (should ~0%),
		// (c) DSP work isn't leaking to Core 2 control plane.
		static u64 prev_busy[4] = {0,0,0,0};
		static u64 prev_idle[4] = {0,0,0,0};
		u64 d_b1 = g_coreBusyTicks[1] - prev_busy[1];
		u64 d_i1 = g_coreIdleTicks[1] - prev_idle[1];
		u64 d_b2 = g_coreBusyTicks[2] - prev_busy[2];
		u64 d_b3 = g_coreBusyTicks[3] - prev_busy[3];
		u64 d_i3 = g_coreIdleTicks[3] - prev_idle[3];
		unsigned c1pct = (d_b1+d_i1) ? (unsigned)((d_b1 * 100) / (d_b1+d_i1)) : 0;
		unsigned c2act = d_b2 ? 1u : 0u;       // control plane is Yield-cooperative; report active
		unsigned c3pct = (d_b3+d_i3) ? (unsigned)((d_b3 * 100) / (d_b3+d_i3)) : 0;
		// Rate-limited cores log — user reported audible glitches correlating
		// with screen-log output. Emit only when busy% changes by >=5 (real
		// state change worth observing), with a 10s heartbeat as fallback so
		// the line still appears periodically for sanity checks.
		static unsigned s_lastC1 = 999, s_lastC3 = 999;
		static u64 s_lastCoreLogTicks = 0;
		unsigned dc1 = (c1pct > s_lastC1) ? (c1pct - s_lastC1) : (s_lastC1 - c1pct);
		unsigned dc3 = (c3pct > s_lastC3) ? (c3pct - s_lastC3) : (s_lastC3 - c3pct);
		bool heartbeat = (now - s_lastCoreLogTicks) > (USB_STAT_TICKS * 20);
		if (dc1 >= 5 || dc3 >= 5 || heartbeat) {
			CLogger::Get()->Write(log_name, LogNotice,
				"cores c1=%u%% c2=%s c3=%u%%",
				c1pct, c2act ? "active" : "idle", c3pct);
			s_lastC1 = c1pct;
			s_lastC3 = c3pct;
			s_lastCoreLogTicks = now;
		}
		prev_busy[1]=g_coreBusyTicks[1]; prev_idle[1]=g_coreIdleTicks[1];
		prev_busy[2]=g_coreBusyTicks[2];
		prev_busy[3]=g_coreBusyTicks[3]; prev_idle[3]=g_coreIdleTicks[3];
#endif
		// Engine introspection — only emit when transpose is engaged (so
		// passthrough stays log-quiet). Shows the actual on-Pi engine state:
		// is m_scale at the commanded ratio, how many splices fired this
		// interval, is the period detector locked. Diagnoses the Pi-only
		// scattered-spectrum issue that doesn't repro on host.
		extern RubberBandWrapper *pLivePitchWrapper;
		if (pLivePitchWrapper && pLivePitchWrapper->isEngaged()) {
			static unsigned prev_splice = 0;
			unsigned sc = pLivePitchWrapper->engineSpliceCount();
			unsigned d_splice = sc - prev_splice;
			prev_splice = sc;
			int sci = (int)(pLivePitchWrapper->engineScale() * 1000.0f);
			static unsigned prev_emerg = 0;
			unsigned em = pLivePitchWrapper->engineEmergency();
			unsigned d_emerg = em - prev_emerg; prev_emerg = em;
			// eff = the ACTUAL effective read rate on the real Pi input
			// (continuous advance + splice jumps)/samples. This is the
			// output/input frequency RATIO measured in-engine, the
			// capture-free witness for the -12 pitch: must read 0.5000.
			int effi = (int)(pLivePitchWrapper->engineEffRate() * 10000.0f);
			// perr = mean |splice fractional-period error| in 1/100 samples.
			// Post phase-anchor must read ~0 => splices frequency-neutral on
			// real Pi input => audible -12 = exact 0.5×.
			int perri = (int)(pLivePitchWrapper->engineSplicePhaseErr() * 100.0f);
			// Formant pre-stage witnesses. preEff = pre-stage net read rate;
			// must equal preTgt = pow(scale,-depth) (the warp ratio) — a
			// deviation means the formant stage is leaking into pitch ("slowing
			// down"). prePerr = mean pre-splice fractional-period error in 1/100
			// samples; must read ~0 when pitched = pre-splices frequency-neutral.
			// Grain-formant witnesses: gmix = crossfade (0=continuous reader,
			// 1000=full grain path); fmf = current grain formant factor (1000 =
			// 1.0 = no shift). If gmix stays 0 with the knob off-center, the
			// crossfade isn't engaging = formant inaudible.
			int gmixi = (int)(pLivePitchWrapper->engineGrainMix() * 1000.0f);
			int fmfi  = (int)(pLivePitchWrapper->engineGrainFactor() * 1000.0f);
			int fdrawi = (int)(pLivePitchWrapper->engineFormantDepthRaw() * 1000.0f);
			CLogger::Get()->Write(log_name, LogNotice,
				"eng scale=%d.%03d eff=%d.%04d perr=%d fdraw=%d gmix=%d fmf=%d gap=%d period=%d lock=%d peakV=%d peakTau=%d splice+%u emerg+%u dtms=%u",
				sci/1000, sci%1000,
				effi/10000, effi%10000, perri,
				fdrawi, gmixi, fmfi,
				pLivePitchWrapper->engineGap(),
				pLivePitchWrapper->enginePeriod(),
				pLivePitchWrapper->enginePeriodOk() ? 1 : 0,
				pLivePitchWrapper->enginetPeakVal1000(),
					pLivePitchWrapper->enginePeakTau(),
					d_splice, d_emerg, (unsigned)((now - s_lastEngTicks)/1000));
				s_lastEngTicks = now;
		}
	}
#endif
	if (pTheAPC) pTheAPC->update();
}
