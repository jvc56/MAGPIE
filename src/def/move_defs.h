#ifndef MOVE_DEFS_H
#define MOVE_DEFS_H

#include "board_defs.h"
#include "rack_defs.h"

// Passes and exchanges can exceed the board size when racks are bigger than the
// board dimension.
enum {
  MOVE_MAX_TILES = ((BOARD_DIM) > (RACK_SIZE) ? (BOARD_DIM) : (RACK_SIZE))
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
  // MOVE_RECORD_BEST_SMALL with a play's ranking value also counting the face
  // value of the tiles it places (MoveGenArgs.best_small_face_weight). Kept
  // last, after MOVE_RECORD_BEST_SMALL: see move_record_type_is_best_small.
  MOVE_RECORD_BEST_SMALL_FACE,
} move_record_t;

// Whether a generation records only its best play as a small move, by either
// ranking.
static inline bool move_record_type_is_best_small(move_record_t record_type) {
  return record_type >= MOVE_RECORD_BEST_SMALL;
}

#define MOVE_SORT_EQUITY_STRING "equity"
#define MOVE_SORT_SCORE_STRING "score"

#define MOVE_RECORD_ALL_STRING "all"
#define MOVE_RECORD_BEST_STRING "best"
#define MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST_STRING "equity"
#endif
