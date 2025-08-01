#ifndef STATIC_EVAL_DEFS_H
#define STATIC_EVAL_DEFS_H

#include "../def/equity_defs.h"

enum { NON_OUTPLAY_LEAVE_SCORE_MULTIPLIER_PENALTY = 2 };

#define OPENING_HOTSPOT_PENALTY ((int)(-0.7 * EQUITY_RESOLUTION))
#define NON_OUTPLAY_CONSTANT_PENALTY ((int)(10.0 * EQUITY_RESOLUTION))
#define PEG_ADJUST_VALUES_LENGTH ((RACK_SIZE * 2) - 1)

#endif
