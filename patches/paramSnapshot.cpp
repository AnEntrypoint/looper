#include "paramSnapshot.h"

volatile u32 g_paramActiveIdx = 0;
LiveParams   g_paramSlots[2]  = {
    { false, 0.0f, 0.0f, false, 0.0f, 0 },
    { false, 0.0f, 0.0f, false, 0.0f, 0 }
};
