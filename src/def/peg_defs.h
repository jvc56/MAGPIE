#ifndef PEG_DEFS_H
#define PEG_DEFS_H

#include "rack_defs.h"

// Bag-size range a pre-endgame (PEG) position may have. PEG handles 1..4 tiles
// in the bag; larger positions are midgame and are rejected by the solver.
enum {
  PEG_MIN_BAG = 1,
  PEG_MAX_BAG = 4,
  PEG_MAX_UNSEEN = RACK_SIZE + PEG_MAX_BAG,
};

// Default inner-peg recursion depth when nested lookahead is on: one nested
// peg, then a greedy rollout. Deeper recursion does not improve the decision.
enum {
  PEG_NESTED_DEFAULT_DEPTH = 1,
};

#endif
