#include "coreBusy.h"

volatile u64 g_coreBusyTicks[4] = { 0, 0, 0, 0 };
volatile u64 g_coreIdleTicks[4] = { 0, 0, 0, 0 };
