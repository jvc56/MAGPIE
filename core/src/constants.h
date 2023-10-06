#ifndef CONSTANTS_H
#define CONSTANTS_H

#define ALPHABET_EMPTY_SQUARE_MARKER 0
#define PLAYED_THROUGH_MARKER 0
#define BLANK_MACHINE_LETTER 0
#define TRIVIAL_CROSS_SET (uint64_t)((uint64_t)1 << MAX_ALPHABET_SIZE) - 1
#define ASCII_PLAYED_THROUGH '.'
#define MAX_DATA_FILENAME_LENGTH 64
#define BINGO_BONUS 50
// Shared enums

typedef enum {
  MOVE_SORT_EQUITY,
  MOVE_SORT_SCORE,
} move_sort_t;

typedef enum {
  MOVE_RECORDER_ALL,
  MOVE_RECORDER_BEST,
} move_recorder_t;

typedef enum {
  SIM_STOPPING_CONDITION_NONE,
  SIM_STOPPING_CONDITION_95PCT,
  SIM_STOPPING_CONDITION_98PCT,
  SIM_STOPPING_CONDITION_99PCT,
} sim_stopping_condition_t;

typedef enum {
  BACKUP_MODE_OFF,
  BACKUP_MODE_SIMULATION,
} backup_mode_t;

#endif