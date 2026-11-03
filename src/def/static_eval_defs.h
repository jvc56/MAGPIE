#ifndef STATIC_EVAL_DEFS_H
#define STATIC_EVAL_DEFS_H

#include "../def/equity_defs.h"

#define OPENING_HOTSPOT_PENALTY ((int)(-0.7 * EQUITY_RESOLUTION))
#define NON_OUTPLAY_CONSTANT_PENALTY ((int)(10.0 * EQUITY_RESOLUTION))
#define PEG_ADJUST_VALUES_LENGTH ((RACK_SIZE * 2) - 1)
// The largest pre-endgame adjustment, in points, either way. Fitted values
// are tens of points; the bound keeps equities far from overflow.
#define PEG_ADJUST_MAX_ABS_VALUE 1000.0

#endif
