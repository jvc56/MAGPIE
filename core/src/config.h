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
  CONFIG_LOAD_STATUS_UNRECOGNIZED_COMMAND,
  CONFIG_LOAD_STATUS_UNRECOGNIZED_ARG,
} config_load_status_t;

typedef struct StrategyParams {
  KWG *kwg;
  char *kwg_filename;
  KLV *klv;
  char *klv_filename;
  int move_sorting;
  int play_recorder_type;
} StrategyParams;

typedef struct GameConfig {
  LetterDistribution *letter_distribution;
  char *ld_filename;
  char *cgp;
  bool kwg_is_shared;
  bool klv_is_shared;
  StrategyParams *player_strategy_params[2];
} GameConfig;

typedef struct InferenceConfig {
  Rack *actual_tiles_played;
  int player_to_infer_index;
  int actual_score;
  int number_of_tiles_exchanged;
  double equity_margin;
} InferenceConfig;

typedef struct SimConfig {
  WinPct *win_pcts;
  char *win_pct_filename;
  int move_list_capacity;
} SimConfig;

typedef struct AutoplayConfig {
  int use_game_pairs;
  int number_of_games_or_pairs;
} AutoplayConfig;

typedef enum {
  COMMAND_TYPE_UNKNOWN,
  COMMAND_TYPE_LOAD_CGP,
  COMMAND_TYPE_SIM,
  COMMAND_TYPE_INFER,
  COMMAND_TYPE_AUTOPLAY,
} command_t;

typedef struct Config {
  command_t command;
  GameConfig *game_config;
  InferenceConfig *inference_config;
  SimConfig *sim_config;
  AutoplayConfig *autoplay_config;
  ThreadControl *thread_control;
} Config;

void load_config_from_lexargs(Config **config, const char *cgp,
                              char *lexicon_name, char *ldname);
Config *create_config(
    const char *letter_distribution_filename, const char *cgp,
    const char *kwg_filename_1, const char *klv_filename_1, int move_sorting_1,
    int play_recorder_type_1, const char *kwg_filename_2,
    const char *klv_filename_2, int move_sorting_2, int play_recorder_type_2,
    int game_pair_flag, int number_of_games_or_pairs, int print_info,
    int checkstop, const char *actual_tiles_played, int player_to_infer_index,
    int actual_score, int number_of_tiles_exchanged, double equity_margin,
    int number_of_threads, const char *winpct_filename, int move_list_capacity);
Config *create_config_from_args(int argc, char *argv[]);
void destroy_config(Config *config);
StrategyParams *copy_strategy_params(StrategyParams *orig);
void destroy_strategy_params(StrategyParams *sp);

#endif
