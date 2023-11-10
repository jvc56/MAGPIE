#ifndef CONSTANTS_H
#define CONSTANTS_H

#define ALPHABET_EMPTY_SQUARE_MARKER 0
#define PLAYED_THROUGH_MARKER 0
#define BLANK_MACHINE_LETTER 0
#define TRIVIAL_CROSS_SET (uint64_t)((uint64_t)1 << MAX_ALPHABET_SIZE) - 1
#define ASCII_PLAYED_THROUGH '.'
#define MAX_DATA_FILENAME_LENGTH 64
#define BINGO_BONUS 50
#define KWG_FILEPATH "data/lexica/"
#define KWG_FILE_EXTENSION ".kwg"
#define LETTER_DISTRIBUTION_FILEPATH "data/letterdistributions/"
#define LETTER_DISTRIBUTION_FILE_EXTENSION ".csv"
#define KLV_FILEPATH "data/lexica/"
#define KLV_FILE_EXTENSION ".klv2"
#define WIN_PCT_FILEPATH "data/strategy/"
#define WIN_PCT_FILE_EXTENSION ".csv"
#define DEFAULT_WIN_PCT "winpct"

// Shared enums

typedef enum {
  GAME_VARIANT_UNKNOWN,
  GAME_VARIANT_CLASSIC,
  GAME_VARIANT_WORDSMOG,
} game_variant_t;

typedef enum {
  BOARD_LAYOUT_UNKNOWN,
  BOARD_LAYOUT_CROSSWORD_GAME,
  BOARD_LAYOUT_SUPER_CROSSWORD_GAME,
} board_layout_t;

typedef enum {
  MOVE_SORT_EQUITY,
  MOVE_SORT_SCORE,
} move_sort_t;

typedef enum {
  MOVE_RECORD_ALL,
  MOVE_RECORD_BEST,
} move_record_t;

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