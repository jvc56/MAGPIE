#ifndef MOVEGEN_H
#define MOVEGEN_H

#include <stdint.h>

#include "alphabet.h"
#include "bag.h"
#include "board.h"
#include "config.h"
#include "constants.h"
#include "gaddag.h"
#include "leaves.h"
#include "letter_distribution.h"
#include "move.h"
#include "rack.h"

typedef struct Generator {
    int current_row_index;
    int current_anchor_col;
    int last_anchor_col;

    int vertical;

    int tiles_played;
    int number_of_plays;
    int number_of_possible_letters;
    int sorting_parameter;
    int play_recorder_type;

    uint8_t strip[BOARD_DIM];
    uint8_t exchange_strip[(RACK_ARRAY_SIZE)];
    double preendgame_adjustment_values[PREENDGAME_ADJUSTMENT_VALUES_LENGTH];

    MoveList * move_list;
    Gaddag * gaddag;
    Board * board;
    Bag * bag;
    LetterDistribution * letter_distribution;
    Laddag * laddag;
} Generator;

Generator * create_generator(Config * config);
void destroy_generator(Generator * gen);
void generate_moves(Generator * gen, Rack * rack, Rack * opp_rack, int add_exchange);
void recursive_gen(Generator * gen, int col, Rack * rack, Rack * opp_rack, uint32_t node_index, int leftstrip, int rightstrip, int unique_play);
void reset_generator(Generator * gen);
void set_gen_play_recorder_type(Generator * gen, int play_recorder_type);
void set_gen_sorting_parameter(Generator * gen, int move_sorting);
void set_start_leave_index(Generator * gen, Rack * rack);

#endif