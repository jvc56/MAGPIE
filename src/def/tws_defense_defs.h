#ifndef TWS_DEFENSE_DEFS_H
#define TWS_DEFENSE_DEFS_H

#include "rack_defs.h"

// Feature layout for the TWS defense weights (see src/ent/tws_defense.h).
//
// Every feature measures one channel of opponent access to an empty triple
// word square, binned by the number of empty squares a word reaching the TWS
// must fill ("d"), which is the number of tiles the opponent must play. Both
// channels start at d = 1: a hook at d = 1 is a hookable TWS square itself (a
// one-tile triple play), and a floater at d = 1 is a playthrough tile directly
// adjacent to the TWS.
enum {
  TWD_HOOK_BIN_COUNT = RACK_SIZE,
  TWD_FLOATER_BIN_COUNT = RACK_SIZE,
  TWD_FEATURE_HOOK_START = 0,
  TWD_FEATURE_FLOAT_FLEX_START = TWD_FEATURE_HOOK_START + TWD_HOOK_BIN_COUNT,
  TWD_FEATURE_FLOAT_SCORE_START =
      TWD_FEATURE_FLOAT_FLEX_START + TWD_FLOATER_BIN_COUNT,
  TWD_FEATURE_TT_FLOATER =
      TWD_FEATURE_FLOAT_SCORE_START + TWD_FLOATER_BIN_COUNT,
  TWD_FEATURE_TT_HOOK_ONLY = TWD_FEATURE_TT_FLOATER + 1,
  TWD_NUM_FEATURES = TWD_FEATURE_TT_HOOK_ONLY + 1,
};

#define TWD_MAGIC_HEADER "magpie_twd_v1"

#endif
