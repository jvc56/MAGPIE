#ifndef MOVE_DEFS_H
#define MOVE_DEFS_H

#define PASS_MOVE_EQUITY -10000
#define INITIAL_TOP_MOVE_EQUITY -100000
#define COMPARE_MOVES_EPSILON 1e-6

typedef enum {
  MOVE_SORT_EQUITY,
  MOVE_SORT_SCORE,
} move_sort_t;

typedef enum {
  MOVE_RECORD_ALL,
  MOVE_RECORD_BEST,
} move_record_t;

#endif