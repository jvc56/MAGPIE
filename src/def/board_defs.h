#ifndef BOARD_DEFS_H
#define BOARD_DEFS_H

#include "rack_defs.h"
#include <stdbool.h>

// This should be defined in the Makefile
// but is conditionally defined here
// as a fallback and so the IDE doesn't
// complain about a missing definition.
#ifndef BOARD_DIM
#define BOARD_DIM DEFAULT_BOARD_DIM
#endif

// In printed board, columns are labeled using A-Z
#define BOARD_NUM_COLUMN_LABELS 26

enum {
  BOARD_HORIZONTAL_DIRECTION = 0,
  BOARD_VERTICAL_DIRECTION = 1,
  DEFAULT_BOARD_DIM = 15,
  DEFAULT_SUPER_BOARD_DIM = 21,
  ANCHOR_HEAP_CAPACITY = BOARD_DIM * BOARD_DIM * RACK_SIZE * RACK_SIZE
};

#endif