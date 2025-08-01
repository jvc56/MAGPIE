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
} move_record_t;

#endif