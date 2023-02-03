#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "constants.h"
#include "gaddag.h"
#include "letter_distribution.h"
#include "leaves.h"

Config * create_config(const char * gaddag_filename, const char * alphabet_filename, const char * letter_distribution_filename, const char * laddag_filename, int move_list_capacity, int move_sorting, int play_recorder_type, int preendgame_adjustment_values_type) {
    Config * config = malloc(sizeof(Config));
    config->gaddag = create_gaddag(gaddag_filename, alphabet_filename);
    config->letter_distribution = create_letter_distribution(letter_distribution_filename);
	config->laddag = create_laddag(laddag_filename);
    config->move_list_capacity = move_list_capacity;
    config->move_sorting = move_sorting;
    config->play_recorder_type = play_recorder_type;
    config->preendgame_adjustment_values_type = preendgame_adjustment_values_type;
    return config;
}

void destroy_config(Config * config) {
	destroy_laddag(config->laddag);
	destroy_letter_distribution(config->letter_distribution);
	destroy_gaddag(config->gaddag);
    free(config);
}
