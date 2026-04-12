
#ifndef AudioTypes_h
#define AudioTypes_h

#include "Arduino.h"

class AudioControl;
class AudioConnection;
class AudioStream;


#define AUDIO_BLOCK_SAMPLES  		64
#define AUDIO_BLOCK_BYTES  			(AUDIO_BLOCK_SAMPLES * sizeof(s16))
#define AUDIO_SAMPLE_RATE           48000

typedef struct audio_block_struct
{
	u16 ref_count;
    audio_block_struct *next;
    audio_block_struct *prev;
	int16_t data[AUDIO_BLOCK_SAMPLES];
} audio_block_t;

#define AUDIO_INPUT_LINEIN  0
#define AUDIO_INPUT_MIC     1


#define AUDIO_DEVICE_CODEC      0x0001
#define AUDIO_DEVICE_INPUT      0x0004
#define AUDIO_DEVICE_SYNTH      0x0010
#define AUDIO_DEVICE_EFFECT     0x0040
#define AUDIO_DEVICE_MIXER      0x0100
#define AUDIO_DEVICE_TOOL       0x0400
#define AUDIO_DEVICE_OTHER      0x1000
#define AUDIO_DEVICE_OUTPUT     0x8000

#define AC_TYPE_OUTPUT          0x0000
#define AC_TYPE_INPUT           0x0001

typedef struct audioControl
{
    u8              type;
    const char      *name;
    u8              channel;
	u8              ccnum;
}   audioControl_t;

#define AudioMemory(n)


#endif
