#ifndef PEG_DEFS_H
#define PEG_DEFS_H

#include "rack_defs.h"

// Bag-size range a pre-endgame (PEG) position may have. PEG handles 1..4 tiles
// in the bag; larger positions are midgame and are rejected by the solver.
enum {
  PEG_MIN_BAG = 1,
  PEG_MAX_BAG = 4,
  // Most tiles that can be unseen to the mover in a PEG position: a full
  // opponent rack plus the largest allowed bag. Derived from RACK_SIZE so it
  // tracks the build's rack size instead of assuming 7.
  PEG_MAX_UNSEEN = RACK_SIZE + PEG_MAX_BAG,
};

#endif
