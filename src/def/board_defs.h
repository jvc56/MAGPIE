#ifndef BOARD_DEFS_H
#define BOARD_DEFS_H

#define DEFAULT_BOARD_DIM 15
#define DEFAULT_SUPER_BOARD_DIM 21

// This should be defined in the Makefile
// but is conditionally defined here
// as a fallback and so the IDE doesn't
// complain about a missing definition.
#ifndef BOARD_DIM
#define BOARD_DIM DEFAULT_BOARD_DIM
#endif

#define BOARD_HORIZONTAL_DIRECTION 0
#define BOARD_VERTICAL_DIRECTION 1

#define ANCHOR_HEAP_CAPACITY BOARD_DIM * BOARD_DIM

#endif