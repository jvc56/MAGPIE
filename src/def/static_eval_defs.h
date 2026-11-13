#ifndef STATIC_EVAL_DEFS_H
#define STATIC_EVAL_DEFS_H

#include "../def/equity_defs.h"

#define OPENING_HOTSPOT_PENALTY ((int)(-0.7 * EQUITY_RESOLUTION))
#define NON_OUTPLAY_CONSTANT_PENALTY ((int)(10.0 * EQUITY_RESOLUTION))
// A table of pre-endgame adjustments has a value for each index up to its
// last (see peg_adjust.h). A table of the standard length stops where
// Quackle's did, at the plays that leave RACK_SIZE - 2 tiles in the bag; one
// of the maximum length reaches the plays that leave seven racks less one.
#define PEG_ADJUST_STANDARD_VALUES_LENGTH ((RACK_SIZE * 2) - 1)
#define PEG_ADJUST_MAX_VALUES_LENGTH (RACK_SIZE * 8)
// The largest pre-endgame adjustment, in points, either way. Fitted values
// are tens of points; the bound keeps equities far from overflow.
#define PEG_ADJUST_MAX_ABS_VALUE 1000.0

#endif
