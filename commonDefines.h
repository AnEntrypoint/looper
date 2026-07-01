// Defines that are common between rPi Looper and Arduino TeensyExpression projects.
// This file is currently denormalized and stored in both the Arduino-TeensyExpresiona
// project and in the circle-Looper projecte

#pragma once

// 20 flat loopers on the APC's 4-col × 5-row pad area (cols 2-5). The buffer
// pool is sized by (TRACKS × LAYERS × LOOP_TRACK_SECONDS-per-clip seconds);
// 20×1 keeps the total identical to the previous 5×4 layout, so SDRAM use
// is unchanged. Each pad maps to one (track,layer=0) pair, fully independent.
// Cols 0-1 are repurposed as 10 preset slots (see apcKey25Notes.cpp).
#define LOOPER_NUM_TRACKS     20
#define LOOPER_NUM_LAYERS     1
#define LOOPER_NUM_PRESETS    10

#define TRACK_STATE_EMPTY               0x0000
#define TRACK_STATE_RECORDING           0x0001
#define TRACK_STATE_PLAYING             0x0002
#define TRACK_STATE_STOPPED             0x0004
#define TRACK_STATE_PENDING_RECORD      0x0008
#define TRACK_STATE_PENDING_PLAY        0x0010
#define TRACK_STATE_PENDING_STOP        0x0020
#define TRACK_STATE_PENDING			    (TRACK_STATE_PENDING_RECORD | TRACK_STATE_PENDING_PLAY | TRACK_STATE_PENDING_STOP)

#define LOOP_COMMAND_NONE               0x00
#define LOOP_COMMAND_CLEAR_ALL          0x01
#define LOOP_COMMAND_STOP_IMMEDIATE     0x02      // stop the looper immediately
#define LOOP_COMMAND_STOP               0x03      // stop at next cycle point
#define LOOP_COMMAND_DUB_MODE           0x04      // the dub mode is handled by rPi and modeled here
#define LOOP_COMMAND_ABORT_RECORDING    0x06      // abort the current recording if any
#define LOOP_COMMAND_LOOP_IMMEDIATE     0x08      // immediatly loop back to all clip starts ...
#define LOOP_COMMAND_SET_LOOP_START     0x09      // immediatly set the "restart point" for the clips in the track
#define LOOP_COMMAND_CLEAR_LOOP_START   0x0A      // immediatly set the "restart point" for the clips in the track
#define LOOP_COMMAND_DUMP_TRACKS        0x0B      // dump every recorded track to the USB flash drive as WAV files
#define LOOP_COMMAND_HALFSPEED_ON       0x0C      // momentary: all PLAYING tracks read at 0.5x rate (held)
#define LOOP_COMMAND_HALFSPEED_OFF      0x0D      // release of the above -> back to native rate
#define LOOP_COMMAND_DOUBLESPEED_ON     0x0E      // momentary: all PLAYING tracks read at 2x rate (held)
#define LOOP_COMMAND_DOUBLESPEED_OFF    0x0F      // release of the above -> back to native rate
#define LOOP_COMMAND_TRACK_BASE         0x20      // 20 slots: 0x20-0x33 (per-track rec/play toggle)
#define LOOP_COMMAND_ERASE_TRACK_BASE   0x60      // 20 slots: 0x60-0x73 (whole-track erase)
#define LOOP_COMMAND_GET_STATE			0x30	  // NEW the looper will dump all state
	// dumping the state will send
	//      LOOP_STOP_CMD_STATE_CC
	//      LOOP_DUB_STATE_CC
	// 		TRACK_STATE_BASE_CC for NUM_TRACKS
	//		CLIP_VOL_BASE_CC for NUM_TRACKS * NUM_LAYERS
	// 		CLIP_MUTE_BASE_CC for NUM_TRACKS * NUM_LAYERS
	// We do not currently send
	//		LOOP_CONTROL_BASE for LOOPER_NUM_CONTROLS(6)
	// Because TE just always pushes the volume controls

#define LOOP_COMMAND_STOP_TRACK_BASE     0x40    // 20 slots: 0x40-0x53 (pause-at-cycle)
#define LOOP_COMMAND_CLEAR_LAYER_BASE    0xA0    // 20 slots: 0xA0-0xB3 (arm-rec when empty / clear-layer when filled)
#define LOOP_COMMAND_HALVE_TRACK_BASE    0xC0    // 20 slots: 0xC0-0xD3 (vestigial, kept for now)
#define LOOP_COMMAND_DOUBLE_TRACK_BASE   0xE0    // 20 slots: 0xE0-0xF3

#define LOOP_COMMAND_RECORD             0x80
#define LOOP_COMMAND_PLAY               0x81
    // theaw are for internal "pending" command use only


// Looper Serial CC numbers             // TE       rPi         descrip
#define LOOP_COMMAND_CC        0x01		// send     recv        the value is the LOOP command
#define LOOP_STOP_CMD_STATE_CC 0x02		// recv     send        the value is 0, LOOP_COMMAND_STOP or STOP_IMMEDIATE
#define LOOP_DUB_STATE_CC      0x03		// recv     send        value is currently only the DUB state
#define NOTIFY_LOOP            0x05     // recv     send        value=number of pending loop notifies
#define LOOP_CONTROL_BASE_CC   0x08     // send     recv        RANGED for 0..LOOPER_NUM_CONTROLS the value is the volume control (Looper pedal == 0x67)
#define TRACK_STATE_BASE_CC    0x10		// recv     send        RANGED for NUM_TRACKS, upto 16 tracks, value is track state
#define CLIP_VOL_BASE_CC       0x20		// both     both        RANGED for NUM_TRACKS * NUM_LAYERS, upto 32 total - value is the clip volume
#define CLIP_MUTE_BASE_CC      0x40		// both     both        RANGED for NUM_TRACKS * NUM_LAYERS, upto 24 total - value is mute state
	// this leaves a little space for expansion from 0x60 to 0x7f


// Looper Volume Controls
// these are based off of LOOP_CONTROL_BASE_CC

#define LOOPER_CONTROL_INPUT_GAIN      0
#define LOOPER_CONTROL_THRU_VOLUME     1
#define LOOPER_CONTROL_LOOP_VOLUME     2
#define LOOPER_CONTROL_MIX_VOLUME      3
#define LOOPER_CONTROL_OUTPUT_GAIN     4
#define LOOPER_NUM_CONTROLS            5
