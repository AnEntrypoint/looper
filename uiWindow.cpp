
#include "uiWindow.h"
#include <circle/logger.h>
#include <utils/myUtils.h>
#include "Looper.h"
#include "uiStatusBar.h"
#include "uiTrack.h"
#include "vuSlider.h"
#include "apcKey25.h"
#include "usbMidi.h"

#define log_name  "loopwin"

#define ID_LOOP_STATUS_BAR     		101
#define ID_VU1                		201
#define ID_VU2                		202
#define ID_VU3                		203
#define ID_VU4                		204
#define ID_VU_SLIDER           		250
#define ID_TRACK_CONTROL_BASE  		300
#define ID_LOOP_STOP_BUTTON         401
#define ID_LOOP_DUB_BUTTON    		402
#define ID_LOOP_TRACK_BUTTON_BASE   410
#define ID_ERASE_TRACK_BUTTON_BASE  430

#define TOP_MARGIN					2
#define TOP_HEIGHT					26
#define TOP_BUTTON_MARGIN			5
#define TOP_BUTTON_HEIGHT			24
#define TRACK_TOP_MARGIN			5
#define TRACK_BOTTOM_MARGIN			5
#define BUTTON_HEIGHT				34
#define BOTTOM_MARGIN				5

#define TOP_VU_SY					(TOP_MARGIN)
#define TOP_VU_EY					(TOP_VU_SY + TOP_HEIGHT - 1)
#define STATUS_SY					(TOP_VU_SY)
#define STATUS_EY					(TOP_VU_EY)
#define TOP_BUTTON_SY				(TOP_VU_EY + TOP_BUTTON_MARGIN)
#define TOP_BUTTON_EY				(TOP_BUTTON_SY + TOP_BUTTON_HEIGHT - 1)
#define TRACK_SY					(TOP_BUTTON_EY + TRACK_TOP_MARGIN)
#define TRACK_EY					(BUTTON_SY - TRACK_BOTTOM_MARGIN)
#define VU_SY						(TRACK_SY)
#define VU_EY						(TRACK_EY)
#define BUTTON_SY					(height - BOTTOM_MARGIN - BUTTON_HEIGHT - 1)
#define BUTTON_EY					(BUTTON_SY + BUTTON_HEIGHT - 1)

#define TOP_VU_WIDTH				144
#define TOP_LEFT_MARGIN				5
#define TOP_RIGHT_MARGIN			5
#define STATUS_MARGIN				5

#define TOP_LEFT_VU_SX		(TOP_LEFT_MARGIN)
#define TOP_LEFT_VU_EX		(TOP_LEFT_VU_SX + TOP_VU_WIDTH - 1)
#define TOP_RIGHT_VU_SX		(width-TOP_VU_WIDTH-TOP_RIGHT_MARGIN - 1)
#define TOP_RIGHT_VU_EX		(TOP_RIGHT_VU_SX + TOP_VU_WIDTH - 1)

#define STATUS_SX		(TOP_LEFT_VU_EX + STATUS_MARGIN)
#define STATUS_EX		(TOP_RIGHT_VU_SX - STATUS_MARGIN)

#define VU_WIDTH					30
#define VU_RIGHT_MARGIN				10
#define VU_INTER_MARGIN				8

#define VU_RIGHT_SX					(width - VU_RIGHT_MARGIN - VU_WIDTH - 1)
#define VU_MIDDLE_SX				(VU_RIGHT_SX - VU_WIDTH - VU_INTER_MARGIN)
#define VU_LEFT_SX					(VU_MIDDLE_SX - VU_WIDTH - VU_INTER_MARGIN)
#define VU_LEFT_EX					(VU_LEFT_SX + VU_WIDTH - 1)
#define VU_MIDDLE_EX				(VU_MIDDLE_SX + VU_WIDTH - 1)
#define VU_RIGHT_EX					(VU_RIGHT_SX + VU_WIDTH - 1)

#define VU_LABEL1_SX				(VU_LEFT_SX - 2)
#define VU_LABEL1_EX				(VU_LEFT_EX + 2)
#define VU_LABEL2_SX				(VU_MIDDLE_SX - 2)
#define VU_LABEL2_EX				(VU_MIDDLE_EX + 2)
#define VU_LABEL3_SX				(VU_RIGHT_SX - 2)
#define VU_LABEL3_EX				(VU_RIGHT_EX + 2)

#define TRACK_LEFT_MARGIN			5
#define TRACK_RIGHT_MARGIN			8
#define TRACK_INTER_MARGIN			5

#define TRACK_AREA_SX				(TRACK_LEFT_MARGIN)
#define TRACK_AREA_EX				(VU_LEFT_SX - TRACK_RIGHT_MARGIN)
#define TRACK_AREA_WIDTH			(TRACK_AREA_EX - TRACK_AREA_SX + 1)
#define TRACK_STEP					(TRACK_AREA_WIDTH/LOOPER_NUM_TRACKS)
#define TRACK_WIDTH					((TRACK_AREA_WIDTH - TRACK_INTER_MARGIN * (LOOPER_NUM_TRACKS-1))/LOOPER_NUM_TRACKS)

#define BOTTOM_BUTTON_MARGIN		5
#define STOP_BUTTON_SX				(TRACK_AREA_EX + 2)
#define OTHER_BUTTON_AREA_WIDTH		(width - STOP_BUTTON_SX - 1)
#define OTHER_BUTTON_WIDTH 			((OTHER_BUTTON_AREA_WIDTH - BOTTOM_BUTTON_MARGIN)/2)
#define STOP_BUTTON_EX				(STOP_BUTTON_SX + OTHER_BUTTON_WIDTH - 1 + 5)
#define DUB_BUTTON_SX				(STOP_BUTTON_EX + BOTTOM_BUTTON_MARGIN)
#define DUB_BUTTON_EX				(DUB_BUTTON_SX + OTHER_BUTTON_WIDTH - 1 - 5)

#if PIN_LED_LOOPER_BUSY
	static unsigned busy_timer = 0;
#endif


uiWindow::uiWindow(wsApplication *pApp, u16 id, s32 xs, s32 ys, s32 xe, s32 ye) :
	wsTopLevelWindow(pApp,id,xs,ys,xe,ye)
	#if PIN_LED_LOOPER_BUSY
		,busy_led(PIN_LED_LOOPER_BUSY, GPIOModeOutput)
	#endif
{
	#if PIN_LED_LOOPER_BUSY
		busy_led.Write(1);
	#endif

	LOG("uiWindow ctor(%d,%d,%d,%d)",xs,ys,xe,ye);

	send_state = 1;
	last_dub_mode = 0;
	stop_button_cmd = 0;
	for (int i=0; i<LOOPER_NUM_CONTROLS; i++)
		control_vol[i] = 0;
	for (int i=0; i<LOOPER_NUM_TRACKS; i++)
		for (int j=0; j<LOOPER_NUM_LAYERS; j++)
		{
			clip_mute[i][j] = 0;
			clip_vol[i][j] = 0;
		}

	setBackColor(wsDARK_BLUE);

	int height = ye-ys+1;
    int width = xe-xs+1;

	new uiStatusBar(this,ID_LOOP_STATUS_BAR, STATUS_SX, STATUS_SY, STATUS_EX, STATUS_EY);
	LOG("status bar created",0);

	#if WITH_METERS
		new vuSlider(this, ID_VU2,
			TOP_LEFT_VU_SX, TOP_VU_SY, TOP_LEFT_VU_EX, TOP_VU_EY,
			true, 12, METER_INPUT, LOOPER_CONTROL_INPUT_GAIN,
			-1, -1, MIDI_EVENT_TYPE_INC_DEC2, 0x0A);
	#endif

	#if WITH_METERS
		new vuSlider(this,ID_VU2,
			TOP_RIGHT_VU_SX, TOP_VU_SY, TOP_RIGHT_VU_EX, TOP_VU_EY,
			true, 12, -1, LOOPER_CONTROL_OUTPUT_GAIN,
			-1, -1, MIDI_EVENT_TYPE_INC_DEC2, 0x0B);
	#endif

	#if WITH_METERS
		wsStaticText *pt1 = new wsStaticText(this,0,"THRU",
			VU_LABEL1_SX, TOP_BUTTON_SY, VU_LABEL1_EX, TOP_BUTTON_EY);
		pt1->setAlign(ALIGN_BOTTOM_CENTER);
		pt1->setForeColor(wsWHITE);
		pt1->setFont(wsFont7x12);
		new vuSlider(this,ID_VU2,
			VU_LEFT_SX, VU_SY, VU_LEFT_EX, VU_EY,
			false, 12, METER_THRU, LOOPER_CONTROL_THRU_VOLUME,
			-1, -1, MIDI_EVENT_TYPE_INC_DEC1, 0x0E, 0x00);
	#endif

	#if WITH_METERS
		wsStaticText *pt2 = new wsStaticText(this,0,"LOOP",
			VU_LABEL2_SX, TOP_BUTTON_SY, VU_LABEL2_EX, TOP_BUTTON_EY);
		pt2->setAlign(ALIGN_BOTTOM_CENTER);
		pt2->setForeColor(wsWHITE);
		pt2->setFont(wsFont7x12);
		new vuSlider(this,ID_VU3,
			VU_MIDDLE_SX, VU_SY, VU_MIDDLE_EX, VU_EY,
			false, 12, METER_LOOP, LOOPER_CONTROL_LOOP_VOLUME,
			-1, -1, MIDI_EVENT_TYPE_CC, 0x0F);
	#endif

	#if WITH_METERS
		wsStaticText *pt3 = new wsStaticText(this,0,"MIX",
			VU_LABEL3_SX, TOP_BUTTON_SY, VU_LABEL3_EX, TOP_BUTTON_EY);
		pt3->setAlign(ALIGN_BOTTOM_CENTER);
		pt3->setForeColor(wsWHITE);
		pt3->setFont(wsFont7x12);
		new vuSlider(this,ID_VU4,
			VU_RIGHT_SX, VU_SY, VU_RIGHT_EX, VU_EY,
			false, 12, METER_MIX, LOOPER_CONTROL_MIX_VOLUME,
			-1, -1, MIDI_EVENT_TYPE_INC_DEC1, 0x0D, 0x00);
	#endif

	LOG("creating ui_tracks and track buttons",0);

	int start_x = TRACK_AREA_SX;
	for (int i=0; i<LOOPER_NUM_TRACKS; i++)
	{
		LOG("creating ui_track(%d)",i);
		int end_x = start_x + TRACK_WIDTH - 1;

		new uiTrack(i, this, ID_TRACK_CONTROL_BASE + i,
			start_x, TRACK_SY, end_x, TRACK_EY);

		CString track_name;
		track_name.Format("%d",i+1);
		pTrackButtons[i] = new wsButton(
			this, ID_LOOP_TRACK_BUTTON_BASE + i,
			(const char *) track_name,
			start_x+2, BUTTON_SY, end_x-2, BUTTON_EY,
			BTN_STYLE_USE_ALTERNATE_COLORS);
		pTrackButtons[i]->setFont(wsFont12x16);
		pTrackButtons[i]->setAltBackColor(wsSLATE_GRAY);

		pEraseButtons[i] = new wsButton(
			this, ID_ERASE_TRACK_BUTTON_BASE + i,
			"erase",
			start_x+2, TOP_BUTTON_SY, end_x-2, TOP_BUTTON_EY,
			BTN_STYLE_USE_ALTERNATE_COLORS);
		pEraseButtons[i]->setFont(wsFont8x14);
		pEraseButtons[i]->setAltBackColor(wsSLATE_GRAY);
		pEraseButtons[i]->hide();

		start_x += TRACK_STEP;
		LOG("finished creating ui_track(%d)",i);
	}

	pStopButton = new wsButton(
		this, ID_LOOP_STOP_BUTTON,
		"blah",
		STOP_BUTTON_SX, BUTTON_SY, STOP_BUTTON_EX, BUTTON_EY,
		BTN_STYLE_USE_ALTERNATE_COLORS, WIN_STYLE_CLICK_LONG);
	pStopButton->setAltBackColor(wsSLATE_GRAY);
	pStopButton->setFont(wsFont12x16);
	pStopButton->hide();

	pDubButton = new wsButton(
		this, ID_LOOP_DUB_BUTTON,
		getLoopCommandName(LOOP_COMMAND_DUB_MODE),
		DUB_BUTTON_SX, BUTTON_SY, DUB_BUTTON_EX, BUTTON_EY,
		BTN_STYLE_USE_ALTERNATE_COLORS, WIN_STYLE_CLICK_LONG);
	pDubButton->setAltBackColor(wsSLATE_GRAY);
	pDubButton->setFont(wsFont12x16);

	new apcKey25();

	LOG("uiWindow ctor finished",0);
	#if PIN_LED_LOOPER_BUSY
		busy_led.Write(0);
	#endif
}


void uiWindow::enableEraseButton(int i, bool enable)
{
	if (enable)
		pEraseButtons[i]->show();
	else
		pEraseButtons[i]->hide();
}


void uiWindow::updateFrame()
{
	usbMidiProcess(true);

	logString_t *msg;
	while ((msg = pTheLooper->getNextLogString()) != nullptr)
	{
		CLogger::Get()->Write(msg->lname,LogNotice,*msg->string);
		delete msg->string;
		delete msg;
	}

	bool running = pTheLooper->getRunning();
	u16  pending = pTheLooper->getPendingCommand();
	u16 t_command = running ?
		pending == LOOP_COMMAND_STOP ?
			LOOP_COMMAND_STOP_IMMEDIATE :
			LOOP_COMMAND_STOP : 0;

	if (stop_button_cmd != t_command)
	{
		stop_button_cmd = t_command;
		pStopButton->setText(getLoopCommandName(t_command));
		if (!t_command)
			pStopButton->hide();
		else
			pStopButton->show();
	}

	bool dub_mode = pTheLooper->getDubMode();
	if (dub_mode != last_dub_mode)
	{
		LOG("updateState dub mode=%d",dub_mode);
		last_dub_mode = dub_mode;
		int bc = dub_mode ? wsORANGE : defaultButtonReleasedColor;
		pDubButton->setBackColor(bc);
		pDubButton->setStateBits(WIN_STATE_DRAW);
	}

	for (int i=0; i<LOOPER_NUM_CONTROLS; i++)
	{
		int vol = pTheLooper->getControlValue(i);
		control_vol[i] = vol;
	}

	for (int i=0; i<LOOPER_NUM_TRACKS; i++)
	{
		publicTrack *pTrack = pTheLooper->getPublicTrack(i);
		for (int j=0; j<LOOPER_NUM_LAYERS; j++)
		{
			clip_mute[i][j] = pTrack->getPublicClip(j)->isMuted();
			clip_vol[i][j] = pTrack->getPublicClip(j)->getVolume();
		}
	}

	if (pTheAPC)
		pTheAPC->update();

	wsWindow::updateFrame();

	#if PIN_LED_LOOPER_BUSY
		if (busy_timer)
		{
			unsigned now = CTimer::Get()->GetClockTicks();
			if (now - busy_timer > 700000)
			{
				busy_timer = 0;
				busy_led.Write(0);
			}
		}
	#endif
}


void uiWindow::handleCC(u8 cc_num, u8 value)
{
	if (cc_num >= LOOP_CONTROL_BASE_CC &&
		cc_num < LOOP_CONTROL_BASE_CC + LOOPER_NUM_CONTROLS)
	{
		int control_num = cc_num - LOOP_CONTROL_BASE_CC;
		pTheLooper->setControl(control_num,value);
		control_vol[control_num] = value;
	}
	else if (cc_num == LOOP_COMMAND_CC)
	{
		if (value == LOOP_COMMAND_GET_STATE)
			send_state = 1;
		else
			pTheLooper->command(value);
	}
	else if (cc_num >= CLIP_VOL_BASE_CC &&
			 cc_num < CLIP_VOL_BASE_CC + LOOPER_NUM_TRACKS * LOOPER_NUM_LAYERS)
	{
		int num = cc_num - CLIP_VOL_BASE_CC;
		publicTrack *pTrack = pTheLooper->getPublicTrack(num / LOOPER_NUM_LAYERS);
		pTrack->getPublicClip(num % LOOPER_NUM_LAYERS)->setVolume(value);
	}
	else if (cc_num >= CLIP_MUTE_BASE_CC &&
			 cc_num < CLIP_MUTE_BASE_CC + LOOPER_NUM_TRACKS * LOOPER_NUM_LAYERS)
	{
		int num = cc_num - CLIP_MUTE_BASE_CC;
		publicTrack *pTrack = pTheLooper->getPublicTrack(num / LOOPER_NUM_LAYERS);
		pTrack->getPublicClip(num % LOOPER_NUM_LAYERS)->setMute(value);
	}
}


void uiWindow::sendState()
{
	send_state = 0;
}


u32 uiWindow::handleEvent(wsEvent *event)
{
	u32 result_handled = 0;
	u32 type = event->getEventType();
	u32 event_id = event->getEventID();
	u32 id = event->getID();

	LOG("uiWindow::handleEvent(%08x,%d,%d)",type,event_id,id);

	if (type == EVT_TYPE_WINDOW && event_id == EVENT_LONG_CLICK)
	{
		if (id == ID_LOOP_STOP_BUTTON || id == ID_LOOP_DUB_BUTTON)
			pTheLooper->command(LOOP_COMMAND_CLEAR_ALL);
	}
	else if (type == EVT_TYPE_BUTTON && event_id == EVENT_CLICK)
	{
		if (id == ID_LOOP_STOP_BUTTON)
		{
			if (stop_button_cmd)
				pTheLooper->command(stop_button_cmd);
		}
		else if (id == ID_LOOP_DUB_BUTTON)
		{
			pTheLooper->command(LOOP_COMMAND_DUB_MODE);
		}
		else if (id >= ID_LOOP_TRACK_BUTTON_BASE &&
				 id < ID_LOOP_TRACK_BUTTON_BASE + NUM_TRACK_BUTTONS)
		{
			pTheLooper->command(LOOP_COMMAND_TRACK_BASE + (id - ID_LOOP_TRACK_BUTTON_BASE));
		}
		else if (id >= ID_ERASE_TRACK_BUTTON_BASE &&
				 id < ID_ERASE_TRACK_BUTTON_BASE + NUM_TRACK_BUTTONS)
		{
			pTheLooper->command(LOOP_COMMAND_ERASE_TRACK_BASE + (id - ID_ERASE_TRACK_BUTTON_BASE));
		}
		result_handled = 1;
	}
	else if (type == EVT_TYPE_WINDOW && event_id == EVENT_CLICK &&
			 id >= ID_CLIP_BUTTON_BASE &&
			 id <= ID_CLIP_BUTTON_BASE + LOOPER_NUM_TRACKS*LOOPER_NUM_LAYERS)
	{
		int num = id - ID_CLIP_BUTTON_BASE;
		publicTrack *pTrack = pTheLooper->getPublicTrack(num / LOOPER_NUM_LAYERS);
		publicClip  *pClip  = pTrack->getPublicClip(num % LOOPER_NUM_LAYERS);
		LOG("setting clip(%d,%d) mute=%d", num/LOOPER_NUM_LAYERS, num%LOOPER_NUM_LAYERS, !pClip->isMuted());
		pClip->setMute(!pClip->isMuted());
		result_handled = 1;
	}

	if (!result_handled)
		result_handled = wsTopLevelWindow::handleEvent(event);

	return result_handled;
}
