// The Looper Machine
//
// The classes are broken up into "public" versions, available to the UI,
// and "implementation" versions that are only intended to be called in
// the loopMachine-loopTrack-loopClip hierarchy, while still maintaining
// some protection on the private members of the hiearchy (and a public
// API for use within it).
//
// The UI treats the (public) loopMachine as "readonly", except to
// issue commands() to it, or set the volume/mute of a clip or
// volume of a track.
//
// The machine is driven by the loopMachine::update() method, which
// is called by the audio system for every audio buffer.

#ifndef _loopMachine_h_
#define _loopMachine_h_

#include <audio/Audio.h>
#include "commonDefines.h"
    // LOOPER_NUM_TRACKS and LAYERS, TRACK_STATES, LOOP_COMMANDS, and common CC's

#define WITH_METERS   1

#define S32_MAX      32767
#define S32_MIN     -32768


#if 1
    #define LOOPER_LOG(f,...)           pTheLoopMachine->LogUpdate(log_name,f,__VA_ARGS__)
#elif 0
    #define LOOPER_LOG(f,...)           CLogger::Get()->Write(log_name,LogDebug,f,__VA_ARGS__)
#else
    #define LOOPER_LOG(f,...)
#endif


//------------------------------------------------------
// CONSTANTS AND STRUCTS
//------------------------------------------------------

// #define LOOPER_NUM_TRACKS     4
// #define LOOPER_NUM_LAYERS     3
    // in common defines.h

#define LOOPER_NUM_CHANNELS   2
    // the whole thing is stereo

#define CROSSFADE_BLOCKS     4
    // The number of buffers (10 == approx 30ms) to continue recording

#define LOOP_TRACK_SECONDS  (60 * LOOPER_NUM_TRACKS * LOOPER_NUM_LAYERS)
    // Allowing for 60 seconds per clip per track overall, for example,
    // equals 960 overall track seconds.  A stero set of 128 s16 sample
    // buffers is 512 bytes. Rounding up to 346 buffers per second at
    // that's 177,152 bytes per second.  960 track seconds, therefore,
    // is about 170M,  There's still 600M+ of memory available on the pi

#define LOOP_HEAP_BYTES  (LOOP_TRACK_SECONDS * AUDIO_BLOCK_BYTES * 2 * INTEGRAL_BLOCKS_PER_SECOND)
    // 1440 track seconds, 496,800 blocks == 127,1800,00 bytes == approx 128M

#define INTEGRAL_BLOCKS_PER_SECOND  ((AUDIO_SAMPLE_RATE + (AUDIO_BLOCK_SAMPLES-1)) / AUDIO_BLOCK_SAMPLES)
    // 345


// meters

#define METER_INPUT                 0
#define METER_LOOP                  1
#define METER_THRU                  2
#define METER_MIX                   3
#define NUM_METERS                  4

typedef struct
    // the measurements across some number of samples
    // for a single meter, bracketed by calls to getMeter()
{
    s16 min_sample[LOOPER_NUM_CHANNELS];
    s16 max_sample[LOOPER_NUM_CHANNELS];
}   meter_t;


// controls

typedef struct              // avoid byte sized structs
{
    u16 value;              // 0..127
    u16 default_value;      // 0..127
    float scale;            // 0..1.0 for my controls; unused for codec
    int32_t multiplier;     // for WITH_INT_VOLUMES
} controlDescriptor_t;

enum ClipState {
    CS_IDLE,
    CS_RECORDING,
    CS_RECORDING_MAIN,
    CS_RECORDING_TAIL,
    CS_FINISHING,
    CS_RECORDED,
    CS_PLAYING,
    CS_LOOPING,
    CS_STOPPING
};


// An in memory log message

typedef struct logString_type
{
    const char *lname;
    CString *string;
    logString_type *next;
} logString_t;


// forwards

class loopClip;
class loopTrack;

// static externs

extern CString *getClipStateName(ClipState state);
extern const char *getLoopStateName(u16 state);
extern const char *getLoopCommandName(u16 name);
extern CString *getTrackStateName(u16 track_state);


class loopBuffer
{
    public:

        loopBuffer(u32 size=LOOP_HEAP_BYTES);
        ~loopBuffer();

        void init()          { m_top = 0; }

        s16 *getBuffer()     { return &m_buffer[m_top]; }

        u32 getSize()        { return m_size; }
        u32 getSizeBlocks()  { return m_size / AUDIO_BLOCK_BYTES; }
        u32 getFreeBytes()   { return m_size - m_top; }
        u32 getFreeBlocks()  { return (m_size - m_top) / AUDIO_BLOCK_BYTES; }
        u32 getUsedBytes()   { return m_top; }
        u32 getUsedBlocks()  { return m_top / AUDIO_BLOCK_BYTES; }

        void commitBytes(u32 bytes)    { m_top += bytes; }
        void commitBlocks(u32 blocks)  { m_top += blocks * AUDIO_BLOCK_BYTES; }

    private:

        u32 m_top;
        u32 m_size;
        s16 *m_buffer;
};



class publicClip
{
    public:

        publicClip(u16 track_num, u16 clip_num)
        {
            m_track_num = track_num;
            m_clip_num = clip_num;
            init();
        }
        virtual ~publicClip()       {}

        u16 getClipNum()            { return m_clip_num; }
        u16 getTrackNum()           { return m_track_num; }
        ClipState getClipState()    { return m_state; }
        u32 getNumBlocks()          { return m_num_blocks; }
        u32 getMaxBlocks()          { return m_max_blocks; }
        u32 getPlayBlockNum()       { return m_play_block; }
        u32 getRecordBlockNum()     { return m_record_block; }
        u32 getCrossfadeBlockNum()  { return m_crossfade_start + m_crossfade_offset; }

        bool isMuted()              { return m_mute; }
        void setMute(bool mute)     { m_mute = mute; }

        int getVolume()             { return m_volume * 100.00; }
        void setVolume(int vol)     { m_volume = ((float)vol)/100.00; }
            
    protected:

        void init()
        {
            m_state = CS_IDLE;
            m_num_blocks = 0;
            m_max_blocks = 0;
            m_play_block = 0;
            m_record_block = 0;
            m_crossfade_start = 0;
            m_crossfade_offset = 0;
            m_origNumBlocks = 0;
            m_quantizeTarget = 0;
            m_quantizeWillPlay = false;
            m_recordStartPhaseOffset = 0;
            m_mute = false;
            m_volume = 1.0;
            m_mark_point = -1;
            m_mark_point_active = false;
        }

        u16  m_track_num;
        u16  m_clip_num;
        ClipState  m_state;
        u32  m_num_blocks;      // the number of blocks NOT including the crossfade blocks
        u32  m_max_blocks;      // the number of blocks available for recording
        u32  m_play_block;
        u32  m_record_block;
        u32  m_crossfade_start;
        u32  m_crossfade_offset;
        u32  m_origNumBlocks;
        u32  m_quantizeTarget;
        bool m_quantizeWillPlay;
        u32  m_recordStartPhaseOffset;

        s32  m_mark_point;
        bool m_mark_point_active;

        bool m_mute;

        float m_volume;

};


class loopClip : public publicClip
{
    public:

        loopClip(u16 clip_num, loopTrack* pTrack);
        virtual ~loopClip();

        void init();

        inline s16 *getBlock(u32 block_num)
        {
            return &(m_buffer[block_num * AUDIO_BLOCK_SAMPLES * LOOPER_NUM_CHANNELS]);
        }

        void update(s32 *in, s32 *out);

        void updateState(u16 cur_command);
        void stopImmediate();
        void setMarkPoint();
        void clearMarkPoint();
        void halveLength();
        void doubleLength();

    private:

        loopTrack  *m_pLoopTrack;

        s16 *m_buffer;

        void _startRecording();
        void _startEndingRecording(u32 trimToBlocks, bool willPlay);
        void _finishRecording();

        void _startPlaying();
        void _startFadeOut();
        void _startCrossFade();
        void _endFadeOut();

        u32 _calcQuantizeTarget();
};



class publicTrack
{
    public:

        publicTrack(u16 track_num)
        {
            m_track_num = track_num;
            init();
        }
        virtual ~publicTrack()      {}

        u16 getTrackNum()           { return m_track_num; }
        u16 getNumUsedClips()       { return m_num_used_clips; }
        u16 getNumRecordedClips()   { return m_num_recorded_clips; }
        u16 getNumRunningClips()    { return m_num_running_clips; }
        bool isSelected()           { return m_selected; }
        volatile u32 m_peakLevel;

        virtual publicClip *getPublicClip(u16 clip_num) = 0;
        virtual int getTrackState() = 0;

    protected:

        void init()
        {
            m_num_used_clips = 0;
            m_num_recorded_clips = 0;
            m_num_running_clips = 0;
            m_selected = 0;
            m_peakLevel = 0;
        }

        u16  m_track_num;
        u16  m_num_used_clips;
        u16  m_num_recorded_clips;
        u16  m_num_running_clips;

        bool m_selected;
};



class loopTrack : public publicTrack
{
    public:

        loopTrack(u16 track_num);
        virtual ~loopTrack();

        void init();

        virtual int getTrackState();

        loopClip *getClip(u16 num)  { return m_clips[num]; }
        void setSelected(bool selected)  { m_selected = selected; }

        void updateState(u16 cur_command);
        void update(s32 *in, s32 *out);

        void incDecRunning(int inc);
        void incDecNumUsedClips(int inc);
        void incDecNumRecordedClips(int inc);

        void stopImmediate();
        void setMarkPoint();
        void clearMarkPoint();
        void clearClip(int layer);
        void halveLength();
        void doubleLength();

    private:

        virtual publicClip *getPublicClip(u16 clip_num) { return (publicClip *) m_clips[clip_num]; }

        loopClip *m_clips[LOOPER_NUM_LAYERS];

};



class publicLoopMachine : public AudioStream
{
    public:

        publicLoopMachine();
        ~publicLoopMachine();

        virtual void command(u16 command) = 0;

        u16 getRunning()            { return m_running; }
        u16 getPendingCommand()     { return m_pending_command; }
        int getSelectedTrackNum()   { return m_selected_track_num; }

        bool getDubMode()           { return m_dub_mode; }
        void setDubMode(bool dub)   { m_dub_mode = dub; }
        u32 m_masterLoopBlocks;
        u32 m_masterPhase;
        volatile u32 m_outputPeakLevel;

        virtual publicTrack *getPublicTrack(u16 num) = 0;

        float getMeter(u16 meter, u16 channel);
        u8 getControlValue(u16 control);
        u8 getControlDefault(u16 control);
        void setControl(u16 control, u8 value);

        logString_t *getNextLogString();

         int getPendingLoopNotify()
        {
            if (m_pending_loop_notify)
                return m_pending_loop_notify--;
            return 0;
        }

    protected:

        virtual void update() = 0;
        virtual const char *getName() 	{ return "looper"; }
        virtual u16   getType()  		{ return AUDIO_DEVICE_OTHER; }

        void init()
        {
            m_running = 0;
            m_masterLoopBlocks = 0;
            m_masterPhase = 0;
            m_outputPeakLevel = 0;
            m_pending_command = 0;
            m_selected_track_num = -1;
            m_dub_mode = false;
            m_pending_loop_notify = 0;
        }

        AudioCodec *pCodec;
      	audio_block_t *inputQueueArray[LOOPER_NUM_CHANNELS];

        int m_running;
        u16 m_pending_command;
        int m_selected_track_num;
        bool m_dub_mode;

        meter_t m_meter[NUM_METERS];
        controlDescriptor_t m_control[LOOPER_NUM_CONTROLS];

        logString_t *m_pFirstLogString;
        logString_t *m_pLastLogString;

        volatile int m_pending_loop_notify;

};




class loopMachine : public publicLoopMachine
{
    public:

        loopMachine();
        ~loopMachine();

        loopTrack *getTrack(u16 num)            { return m_tracks[num]; }

        void incDecRunning(int inc);

        void LogUpdate(const char *lname, const char *format, ...);


    private:

        virtual void command(u16 command);
        virtual publicTrack *getPublicTrack(u16 num)            { return (publicTrack *) m_tracks[num]; }
        virtual void update(void);

        void init();
        void updateState();

        u16 m_cur_command;
        int m_cur_track_num;
        int m_mark_point_state;

        volatile u16 m_track_pending[LOOPER_NUM_TRACKS];

        loopTrack *m_tracks[LOOPER_NUM_TRACKS];

        static s32 m_input_buffer[ LOOPER_NUM_CHANNELS * AUDIO_BLOCK_SAMPLES ];
        static s32 m_output_buffer[ LOOPER_NUM_CHANNELS * AUDIO_BLOCK_SAMPLES ];

};


//////////////////////////////////////////////////////
////////// STATIC GLOBAL ACCESSOR ////////////////////
//////////////////////////////////////////////////////

extern loopBuffer  *pTheLoopBuffer;
    // in loopBuffer.cpp

extern loopMachine *pTheLoopMachine;
extern publicLoopMachine *pTheLooper;
    // in audio,cpp


#endif  //!_loopMachine_h_
