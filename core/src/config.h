#ifndef CONFIG_H
#define CONFIG_H

#include "gaddag.h"
#include "letter_distribution.h"
#include "leaves.h"
typedef struct Config {
    Gaddag * gaddag;
    LetterDistribution * letter_distribution;
    Laddag * laddag;
    int move_list_capacity;
    int move_sorting;
    int play_recorder_type;
    int preendgame_adjustment_values_type;
} Config;

Config * create_config(const char * gaddag_filename, const char * alphabet_filename, const char * letter_distribution_filename, const char * laddag_filename, int move_list_capacity, int move_sorting, int play_recorder_type, int preendgame_adjustment_values_type);
void destroy_config(Config * config);

#endif
