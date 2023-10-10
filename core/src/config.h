#ifndef CONFIG_H
#define CONFIG_H

#include "klv.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "rack.h"
#include "thread_control.h"
#include "winpct.h"

typedef enum {
  CONFIG_LOAD_STATUS_SUCCESS,
  CONFIG_LOAD_STATUS_UNRECOGNIZED_ARG,
  CONFIG_LOAD_STATUS_INVALID_ARG_FOR_COMMAND,
  CONFIG_LOAD_STATUS_DUPLICATE_ARG,
  CONFIG_LOAD_STATUS_UNRECOGNIZED_COMMAND,
  CONFIG_LOAD_STATUS_MISPLACED_COMMAND,
  CONFIG_LOAD_STATUS_MISSING_LETTER_DISTRIBUTION,
  CONFIG_LOAD_STATUS_INSUFFICIENT_NUMBER_OF_VALUES,
  CONFIG_LOAD_STATUS_UNKNOWN_BOARD_LAYOUT,
  CONFIG_LOAD_STATUS_UNKNOWN_GAME_VARIANT,
  CONFIG_LOAD_STATUS_MALFORMED_BINGO_BONUS,
  CONFIG_LOAD_STATUS_MALFORMED_MOVE_SORT_TYPE,
  CONFIG_LOAD_STATUS_MALFORMED_MOVE_RECORD_TYPE,
} config_load_status_t;

typedef enum {
  COMMAND_TYPE_UNKNOWN,
  COMMAND_TYPE_LOAD_CGP,
  COMMAND_TYPE_SIM,
  COMMAND_TYPE_INFER,
  COMMAND_TYPE_AUTOPLAY,
  COMMAND_TYPE_SET_OPTIONS,
} command_t;

typedef struct StrategyParams {
  KWG *kwg;
  char *kwg_name;
  KLV *klv;
  char *klv_name;
  move_sort_t move_sort_type;
  move_record_t move_record_type;
} StrategyParams;

typedef struct Config {
  command_t command_type;
  // Game
  LetterDistribution *letter_distribution;
  char *ld_name;
  char *cgp;
  int bingo_bonus;
  board_layout_t board_layout;
  game_variant_t game_variant;
  bool kwg_is_shared;
  bool klv_is_shared;
  StrategyParams *player_strategy_params[2];
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
  int move_list_capacity;
  // Autoplay
  bool use_game_pairs;
  int number_of_games_or_pairs;
  // Thread Control
  ThreadControl *thread_control;
} Config;

Config *create_config();
void destroy_config(Config *config);
StrategyParams *copy_strategy_params(StrategyParams *orig);
void destroy_strategy_params(StrategyParams *sp);
config_load_status_t load_config(Config *config, const char *cmd) {

#endif
