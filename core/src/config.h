#ifndef CONFIG_H
#define CONFIG_H

#include "gaddag.h"
#include "letter_distribution.h"
#include "leaves.h"
typedef struct Config {
    Gaddag * gaddag;
    LetterDistribution * letter_distribution;
    Laddag * laddag;
    char * cgp;
    int move_sorting;
    int play_recorder_type;
} Config;

Config * create_config(const char * gaddag_filename, const char * alphabet_filename, const char * letter_distribution_filename, const char * laddag_filename, const char * cgp, int move_sorting, int play_recorder_type);
Config * create_config_from_args(int argc, char *argv[]);
void destroy_config(Config * config);

#endif
