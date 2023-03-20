#ifndef CONFIG_H
#define CONFIG_H

#include "kwg.h"
#include "letter_distribution.h"
#include "leaves.h"

typedef struct StrategyParams {
    Laddag * laddag;
    int move_sorting;
    int play_recorder_type;
} StrategyParams;

typedef struct Config {
    KWG * kwg;
    LetterDistribution * letter_distribution;
    char * cgp;
    int laddag_is_shared;
    int game_pairs;
    int number_of_games_or_pairs;
    StrategyParams * player_1_strategy_params;
    StrategyParams * player_2_strategy_params;
} Config;

Config * create_config(const char * kwg_filename, const char * alphabet_filename, const char * letter_distribution_filename, const char * cgp,
                       const char * laddag_filename_1, int move_sorting_1, int play_recorder_type_1,
                       const char * laddag_filename_2, int move_sorting_2, int play_recorder_type_2, int game_pair_flag, int number_of_games_or_pairs);
Config * create_config_from_args(int argc, char *argv[]);
void destroy_config(Config * config);

#endif
