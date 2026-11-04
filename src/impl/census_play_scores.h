#ifndef CENSUS_PLAY_SCORES_H
#define CENSUS_PLAY_SCORES_H

#include "../def/board_defs.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "census.h"
#include <stdatomic.h>
#include <stdint.h>

// A tile a play places: its letter, unblanked, its column in the lane, its
// face value times its square's letter multiplier, and the word multiplier of
// its square if it makes a cross word there (0 if not).
typedef struct CensusPlayTile {
  MachineLetter ml;
  int col;
  Equity letter_score;
  int cross_word_multiplier;
} CensusPlayTile;

// A word play census_play_scores_search finds. It goes in direction `dir`
// along the lane lane_index, whose squares, of the searching player's
// cross-set index, are lane[0..BOARD_DIM - 1], from leftstrip to rightstrip.
// It places tiles[0..tiles_played - 1], not in column order, and plays
// through the lane's tiles in between. The squares it places on multiply its
// main word by word_multiplier. Its score counts every placed tile at face
// value, as though none were a blank, and the bingo bonus if it places
// RACK_SIZE tiles.
typedef struct CensusPlay {
  int dir;
  int lane_index;
  const Square *lane;
  int leftstrip;
  int rightstrip;
  const CensusPlayTile *tiles;
  int tiles_played;
  int word_multiplier;
  Equity score;
} CensusPlay;

// How much the play's score drops if its tile tile_idx is a blank: a blank
// keeps every multiplier and the bingo bonus, so only the tile's own face
// value comes off, in the main word and in its cross word.
static inline Equity census_play_blank_drop(const CensusPlay *play,
                                            int tile_idx) {
  const CensusPlayTile *tile = &play->tiles[tile_idx];
  return tile->letter_score *
         (play->word_multiplier + tile->cross_word_multiplier);
}

// Takes each play census_play_scores_search finds. The play is valid only
// during the call.
typedef void (*census_play_recorder_func_t)(const CensusPlay *play,
                                            void *context);

// The tiles not on the game's board, as a tally: the letter distribution
// less every tile on the board, a played blank counting as a blank.
void census_get_unseen_tally(const Game *game, uint8_t *unseen_tally);

// Finds every word play the player player_index could make on the game's
// board, in their lexicon and on their cross-sets, from the tiles of
// unseen_tally, playing at most RACK_SIZE of them, and passes each to
// `record` with `context`. One move generation runs with the whole pool as the
// rack (each letter capped at RACK_SIZE copies). Each word is spelled once,
// with a blank only where no real copy of a letter is left; the recorder
// chooses the blanks. A one-tile play is found once in each direction.
// Returns the number of plays found.
uint64_t census_play_scores_search(const Game *game, int player_index,
                                   const uint8_t *unseen_tally,
                                   census_play_recorder_func_t record,
                                   void *context);

// Builds the census's play scores for the game's board: for every
// multiset P of at most RACK_SIZE tiles, raises play_scores[P] to the best
// score of any word play placing exactly P, drawing its tiles from
// unseen_tally. The caller starts play_scores at 0, the value of exchanging P
// away. Searches with census_play_scores_search, storing no play, and records
// each word's blank spellings by arithmetic (census_record_blank_variants).
// Uses the first player's word graph. Returns the number of plays found.
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

// census_play_scores_build for the plays of one anchor, the one at col of
// lane, alone: the plays the generator finds from it, its leftmost anchor,
// on one thread. touched, if set, receives the ranks of the play scores it
// raises from CENSUS_UNPLAYABLE (see census_record_blank_variants_touched).
uint64_t census_play_scores_build_anchor(
    const Game *game, const CensusLattice *lattice, const uint8_t *unseen_tally,
    Equity *play_scores, CensusTouched *touched, int lane, int col);

#endif
