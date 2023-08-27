#ifndef CONFIG_H
#define CONFIG_H

#include "klv.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "rack.h"
#include "winpct.h"

typedef struct StrategyParams {
  KWG *kwg;
  char kwg_filename[MAX_DATA_FILENAME_LENGTH];
  KLV *klv;
  char klv_filename[MAX_DATA_FILENAME_LENGTH];
  int move_sorting;
  int play_recorder_type;
} StrategyParams;

typedef struct Config {
  LetterDistribution *letter_distribution;
  char ld_filename[MAX_DATA_FILENAME_LENGTH];
  char *cgp;
  int kwg_is_shared;
  int klv_is_shared;
  int use_game_pairs;
  int number_of_games_or_pairs;
  StrategyParams *player_1_strategy_params;
  StrategyParams *player_2_strategy_params;
  // Inference params
  Rack *actual_tiles_played;
  int player_to_infer_index;
  int actual_score;
  int number_of_tiles_exchanged;
  double equity_margin;
  int number_of_threads;
  // Sim params
  WinPct *win_pcts;
  char win_pct_filename[MAX_DATA_FILENAME_LENGTH];
  int move_list_capacity;
} Config;

void load_config_from_lexargs(Config **config, const char *cgp,
                              char *lexicon_name, char *ldname);
Config *create_config(const char *letter_distribution_filename, const char *cgp,
                      const char *kwg_filename_1, const char *klv_filename_1,
                      int move_sorting_1, int play_recorder_type_1,
                      const char *kwg_filename_2, const char *klv_filename_2,
                      int move_sorting_2, int play_recorder_type_2,
                      int game_pair_flag, int number_of_games_or_pairs,
                      const char *actual_tiles_played,
                      int player_to_infer_index, int actual_score,
                      int number_of_tiles_exchanged, double equity_margin,
                      int number_of_threads, const char *winpct_filename,
                      int move_list_capacity);
Config *create_config_from_args(int argc, char *argv[]);
void destroy_config(Config *config);
StrategyParams *copy_strategy_params(StrategyParams *orig);
void destroy_strategy_params(StrategyParams *sp);

#endif
