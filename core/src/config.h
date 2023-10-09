#ifndef CONFIG_H
#define CONFIG_H

#include "klv.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "rack.h"
#include "thread_control.h"
#include "winpct.h"

#define DEFAULT_MOVE_LIST_CAPACITY 1000000

typedef enum {
  CONFIG_LOAD_STATUS_SUCCESS,
  CONFIG_LOAD_STATUS_FAILURE,
  CONFIG_LOAD_STATUS_MULTIPLE_COMMANDS,
  CONFIG_LOAD_STATUS_NO_COMMAND_SPECIFIED,
  CONFIG_LOAD_STATUS_UNRECOGNIZED_ARG,
  CONFIG_LOAD_STATUS_UNRECOGNIZED_COMMAND,
  CONFIG_LOAD_STATUS_MISSING_LETTER_DISTRIBUTION,
} config_load_status_t;

typedef struct StrategyParams {
  KWG *kwg;
  char *kwg_filename;
  KLV *klv;
  char *klv_filename;
  int move_sorting;
  int play_recorder_type;
} StrategyParams;
typedef enum {
  COMMAND_TYPE_UNKNOWN,
  COMMAND_TYPE_LOAD_CGP,
  COMMAND_TYPE_SIM,
  COMMAND_TYPE_INFER,
  COMMAND_TYPE_AUTOPLAY,
} command_t;

typedef struct Config {
  command_t command_type;
  // Game
  LetterDistribution *letter_distribution;
  char *ld_filename;
  char *cgp;
  bool kwg_is_shared;
  bool klv_is_shared;
  StrategyParams *player_strategy_params[2];
  // Inference
  Rack *actual_tiles_played;
  int player_to_infer_index;
  int actual_score;
  int number_of_tiles_exchanged;
  double equity_margin;
  // Sim
  WinPct *win_pcts;
  char *win_pct_filename;
  int move_list_capacity;
  // Autoplay
  int use_game_pairs;
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
