#ifndef CENSUS_PLAY_SCORES_H
#define CENSUS_PLAY_SCORES_H

#include "../ent/equity.h"
#include "../ent/game.h"
#include "census.h"
#include <stdint.h>

// The tiles not on the game's board, as a tally: the letter distribution
// less every tile on the board, a played blank counting as a blank.
void census_get_unseen_tally(const Game *game, uint8_t *unseen_tally);

// Builds the census's play scores for the game's board: for every
// multiset P of at most RACK_SIZE tiles, raises play_scores[P] to the best
// score of any word play placing exactly P, drawing its tiles from
// unseen_tally. The caller starts play_scores at 0, the value of exchanging P
// away. One move generation runs with the whole unseen pool as the rack (each
// letter capped at RACK_SIZE copies) and stores no play. Each word is spelled
// once, with a blank only where no real copy of a letter is left, and its blank
// spellings are recorded by arithmetic (census_record_blank_variants). Uses the
// first player's word graph. Returns the number of plays found.
uint64_t census_play_scores_build(const Game *game,
                                  const CensusLattice *lattice,
                                  const uint8_t *unseen_tally,
                                  Equity *play_scores);

#endif
