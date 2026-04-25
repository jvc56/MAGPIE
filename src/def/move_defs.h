#ifndef MOVE_DEFS_H
#define MOVE_DEFS_H

#include "board_defs.h"
#include "rack_defs.h"

// Passes and exchanges can exceed the board size when racks are bigger than the
// board dimension.
enum {
  MOVE_MAX_TILES = ((BOARD_DIM) > (RACK_SIZE) ? (BOARD_DIM) : (RACK_SIZE)),
  UNSET_LEAVE_SIZE = -1
};

typedef enum {
  MOVE_SORT_EQUITY,
  MOVE_SORT_SCORE,
} move_sort_t;

typedef enum {
  MOVE_RECORD_ALL,
  MOVE_RECORD_BEST,
  MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST,
  MOVE_RECORD_ALL_SMALL,
  MOVE_RECORD_TILES_PLAYED,
  MOVE_RECORD_BEST_SMALL,
  // Boolean "any bingo exists" check. Sets gen->bingo_found and
  // short-circuits all further generation on the first valid bingo
  // placement (no more anchors visited, no exchanges, no pass recording).
  // The movelist is not used. Used by the outcome_model bingo-probability
  // feature, which evaluates many random rack draws per position.
  // Requires a WMP-enabled lexicon. Read the result via bingo_exists().
  MOVE_RECORD_BINGO_EXISTS,
  // Like MOVE_RECORD_BINGO_EXISTS but only considers 7- and 8-letter
  // bingos (anchors with word_length <= 8). 7-letter bingos with no
  // playthrough are the inline-RIT fast path; 8-letter bingos use one
  // playthrough tile via the WMP subrack path. Skipping word_length > 8
  // drops the rare multi-playthrough bingos and gives an approximate
  // answer that may miss some real bingos. Used as a faster alternative
  // when the outcome_model can tolerate some false negatives.
  MOVE_RECORD_BINGO_EXISTS_APPROX,
} move_record_t;

#define MOVE_SORT_EQUITY_STRING "equity"
#define MOVE_SORT_SCORE_STRING "score"

#define MOVE_RECORD_ALL_STRING "all"
#define MOVE_RECORD_BEST_STRING "best"
#define MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST_STRING "equity"
#endif
