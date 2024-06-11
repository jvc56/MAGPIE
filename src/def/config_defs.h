#ifndef CONFIG_DEFS_H
#define CONFIG_DEFS_H

#define EMPTY_RACK_STRING "-"
#define DEFAULT_BINGO_BONUS 50
#define DEFAULT_CHALLENGE_BONUS 5

typedef enum {
  CONFIG_LOAD_STATUS_SUCCESS,
  CONFIG_LOAD_STATUS_UNRECOGNIZED_ARG,
  CONFIG_LOAD_STATUS_DUPLICATE_ARG,
  CONFIG_LOAD_STATUS_MISPLACED_COMMAND,
  CONFIG_LOAD_STATUS_AMBIGUOUS_COMMAND,
  CONFIG_LOAD_STATUS_INT_ARG_OUT_OF_BOUNDS,
  CONFIG_LOAD_STATUS_DOUBLE_ARG_OUT_OF_BOUNDS,
  CONFIG_LOAD_STATUS_LEXICON_MISSING,
  CONFIG_LOAD_STATUS_INSUFFICIENT_NUMBER_OF_VALUES,
  CONFIG_LOAD_STATUS_UNRECOGNIZED_GAME_VARIANT,
  CONFIG_LOAD_STATUS_UNRECOGNIZED_EXEC_MODE,
  CONFIG_LOAD_STATUS_MALFORMED_BINGO_BONUS,
  CONFIG_LOAD_STATUS_MALFORMED_MOVE_SORT_TYPE,
  CONFIG_LOAD_STATUS_MALFORMED_MOVE_RECORD_TYPE,
  CONFIG_LOAD_STATUS_MALFORMED_RACK,
  CONFIG_LOAD_STATUS_MALFORMED_INT_ARG,
  CONFIG_LOAD_STATUS_MALFORMED_DOUBLE_ARG,
  CONFIG_LOAD_STATUS_MALFORMED_PLIES,
  CONFIG_LOAD_STATUS_MALFORMED_MAX_ITERATIONS,
  CONFIG_LOAD_STATUS_MALFORMED_STOPPING_CONDITION,
  CONFIG_LOAD_STATUS_MALFORMED_PLAYER_INDEX,
  CONFIG_LOAD_STATUS_MALFORMED_SCORE,
  CONFIG_LOAD_STATUS_MALFORMED_FLOAT_ARG,
  CONFIG_LOAD_STATUS_MALFORMED_BOOL_ARG,
  CONFIG_LOAD_STATUS_MALFORMED_NUMBER_OF_TILES_EXCHANGED,
  CONFIG_LOAD_STATUS_MALFORMED_RANDOM_SEED,
  CONFIG_LOAD_STATUS_MALFORMED_CONVERSION_TYPE,
  CONFIG_LOAD_STATUS_MALFORMED_NUMBER_OF_THREADS,
  CONFIG_LOAD_STATUS_EXCEEDED_MAX_NUMBER_OF_THREADS,
  CONFIG_LOAD_STATUS_MALFORMED_PRINT_INFO_INTERVAL,
  CONFIG_LOAD_STATUS_MALFORMED_CHECK_STOP_INTERVAL,
  CONFIG_LOAD_STATUS_INCOMPATIBLE_LEXICONS,
  CONFIG_LOAD_STATUS_INCOMPATIBLE_LETTER_DISTRIBUTION,
  CONFIG_LOAD_STATUS_MULTIPLE_EXEC_MODES,
  CONFIG_LOAD_STATUS_BOARD_LAYOUT_ERROR,
} config_load_status_t;

typedef enum {
  EXEC_MODE_UNKNOWN,
  EXEC_MODE_CONSOLE,
  EXEC_MODE_UCGI,
} exec_mode_t;

#define DEFAULT_GAME_VARIANT GAME_VARIANT_CLASSIC
#define DEFAULT_MOVE_LIST_CAPACITY 15

#endif