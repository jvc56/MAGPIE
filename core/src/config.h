#ifndef CONFIG_H
#define CONFIG_H

#include "klv.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "players_data.h"
#include "rack.h"
#include "thread_control.h"
#include "winpct.h"

#define EMPTY_RACK_STRING "-"

typedef enum {
  CONFIG_LOAD_STATUS_SUCCESS,
  CONFIG_LOAD_STATUS_UNRECOGNIZED_ARG,
  CONFIG_LOAD_STATUS_DUPLICATE_ARG,
  CONFIG_LOAD_STATUS_UNRECOGNIZED_COMMAND,
  CONFIG_LOAD_STATUS_MISPLACED_COMMAND,
  CONFIG_LOAD_STATUS_LEXICON_MISSING,
  CONFIG_LOAD_STATUS_INSUFFICIENT_NUMBER_OF_VALUES,
  CONFIG_LOAD_STATUS_UNKNOWN_BOARD_LAYOUT,
  CONFIG_LOAD_STATUS_UNKNOWN_GAME_VARIANT,
  CONFIG_LOAD_STATUS_MALFORMED_BINGO_BONUS,
  CONFIG_LOAD_STATUS_MALFORMED_MOVE_SORT_TYPE,
  CONFIG_LOAD_STATUS_MALFORMED_MOVE_RECORD_TYPE,
  CONFIG_LOAD_STATUS_MALFORMED_RACK,
  CONFIG_LOAD_STATUS_MALFORMED_NUM_PLAYS,
  CONFIG_LOAD_STATUS_MALFORMED_PLIES,
  CONFIG_LOAD_STATUS_MALFORMED_MAX_ITERATIONS,
  CONFIG_LOAD_STATUS_MALFORMED_STOPPING_CONDITION,
  CONFIG_LOAD_STATUS_MALFORMED_PLAYER_TO_INFER_INDEX,
  CONFIG_LOAD_STATUS_MALFORMED_SCORE,
  CONFIG_LOAD_STATUS_MALFORMED_EQUITY_MARGIN,
  CONFIG_LOAD_STATUS_MALFORMED_NUMBER_OF_TILES_EXCHANGED,
  CONFIG_LOAD_STATUS_MALFORMED_RANDOM_SEED,
  CONFIG_LOAD_STATUS_MALFORMED_NUMBER_OF_THREADS,
  CONFIG_LOAD_STATUS_MALFORMED_PRINT_INFO_INTERVAL,
  CONFIG_LOAD_STATUS_MALFORMED_CHECK_STOP_INTERVAL,
  CONFIG_LOAD_STATUS_INCOMPATIBLE_LEXICONS,
  CONFIG_LOAD_STATUS_FOUND_COLDSTART_ONLY_OPTION,
  CONFIG_LOAD_STATUS_MULTIPLE_EXEC_MODES,
} config_load_status_t;

typedef enum {
  COMMAND_TYPE_UNKNOWN,
  COMMAND_TYPE_LOAD_CGP,
  COMMAND_TYPE_SIM,
  COMMAND_TYPE_INFER,
  COMMAND_TYPE_AUTOPLAY,
  COMMAND_TYPE_SET_OPTIONS,
} command_t;

typedef enum {
  EXEC_MODE_SINGLE_COMMAND,
  EXEC_MODE_CONSOLE,
  EXEC_MODE_UCGI,
  EXEC_MODE_COMMAND_FILE,
} exec_mode_t;

typedef struct Config {
  command_t command_type;
  bool command_set_cgp;
  // Game
  LetterDistribution *letter_distribution;
  char *ld_name;
  char *cgp;
  int bingo_bonus;
  board_layout_t board_layout;
  game_variant_t game_variant;
  PlayersData *players_data;
  // Inference
  // This can act as the known opp tiles
  // or the tiles play in an inference
  Rack *rack;
  int player_to_infer_index;
  int actual_score;
  int number_of_tiles_exchanged;
  double equity_margin;
  // Sim
  WinPct *win_pcts;
  char *win_pct_name;
  int num_plays;
  int plies;
  int max_iterations;
  sim_stopping_condition_t stopping_condition;
  bool static_search_only;
  // Autoplay
  bool use_game_pairs;
  uint64_t random_seed;
  // Thread Control
  ThreadControl *thread_control;
  // Config mode and command file execution
  exec_mode_t exec_mode;
  char *command_file;
} Config;

config_load_status_t load_config(Config *config, const char *cmd,
                                 bool coldstart);
Config *create_default_config();
void destroy_config(Config *config);

#endif
