#ifndef CENSUS_OPENING_H
#define CENSUS_OPENING_H

#include "../ent/equity.h"
#include "../ent/game.h"
#include "census.h"
#include <stdatomic.h>
#include <stdint.h>

// Empty-board counterpart of census_play_scores_build_claimed, storing the
// best raw score plus the legacy opening placement adjustment per multiset.
// Callers initialize scores to CENSUS_UNPLAYABLE and share next_unit across
// workers. PAT must be disabled, as it is in static inference.
uint64_t census_opening_build_claimed(const Game *game,
                                      const CensusLattice *lattice,
                                      const uint8_t *unseen_tally,
                                      Equity *scores, int second_tile_groups,
                                      _Atomic int *next_unit);

#endif
