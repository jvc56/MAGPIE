#ifndef CENSUS_PLAY_SCORES_H
#define CENSUS_PLAY_SCORES_H

#include "../def/board_defs.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "census.h"
#include <stdatomic.h>
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

// census_play_scores_build's work for the units it claims from next_unit, in
// the order a serial build visits them, until none are left, so that threads
// can share it. A unit is a first tile of an anchor's plays and one of
// second_tile_groups groups of letters for the tile placed after it; with one
// group, a unit is a first tile. The threads build into the same play_scores,
// each score raised by an atomic max (catomic_max_int32), and the play scores
// they leave are census_play_scores_build's. Only the anchors of the lanes in
// lane_mask (CENSUS_ALL_LANES for every lane) are units. Returns the number of
// plays found in the units claimed.
uint64_t
census_play_scores_build_claimed(const Game *game, const CensusLattice *lattice,
                                 const uint8_t *unseen_tally,
                                 Equity *play_scores, int second_tile_groups,
                                 _Atomic int *next_unit, uint64_t lane_mask);

// Every lane: a lane is a row in one direction, lane dir * BOARD_DIM + row.
#define CENSUS_ALL_LANES ((UINT64_C(1) << (BOARD_DIM * 2)) - 1)

// census_play_scores_build for the plays of the lanes in lane_mask alone. A
// lane's plays depend only on its squares (as board_get_row_cache has them),
// the word graph and the pool, so the build of every lane is the max of
// the builds of any lanes that partition them.
uint64_t census_play_scores_build_lanes(const Game *game,
                                        const CensusLattice *lattice,
                                        const uint8_t *unseen_tally,
                                        Equity *play_scores,
                                        uint64_t lane_mask);

// census_play_scores_build_lanes for the
// plays of one anchor, the one at col of lane, alone: the plays the
// generator finds from it, its leftmost anchor.
uint64_t census_play_scores_build_anchor(const Game *game,
                                         const CensusLattice *lattice,
                                         const uint8_t *unseen_tally,
                                         Equity *play_scores, int lane,
                                         int col);

// The lanes whose squares differ between the boards of two games of the same
// lexicon: the lanes whose plays census_play_scores_build_lanes must find
// again after a move.
uint64_t census_play_scores_changed_lanes(const Game *before,
                                          const Game *after);

#endif
