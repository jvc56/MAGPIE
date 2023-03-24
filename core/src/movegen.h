#ifndef MOVEGEN_H
#define MOVEGEN_H

#include <stdint.h>

#include "alphabet.h"
#include "anchor.h"
#include "bag.h"
#include "board.h"
#include "config.h"
#include "constants.h"
#include "kwg.h"
#include "leaves.h"
#include "letter_distribution.h"
#include "move.h"
#include "player.h"
#include "rack.h"

typedef struct Generator {
    int current_row_index;
    int current_anchor_col;
    int last_anchor_col;
    int vertical;
    int tiles_played;
    int number_of_plays;

    uint8_t strip[(BOARD_DIM)];
    uint8_t * exchange_strip;
    double preendgame_adjustment_values[PREENDGAME_ADJUSTMENT_VALUES_LENGTH];

    MoveList * move_list;
    Board * board;
    Bag * bag;

    KWG * kwg;
    LetterDistribution * letter_distribution;

    // Shadow plays
    AnchorList * anchor_list;
} Generator;

Generator * create_generator(Config * config);
void destroy_generator(Generator * gen);
void generate_moves(Generator * gen, Player * player, Rack * opp_rack, int add_exchange);
void recursive_gen(Generator * gen, int col, Player * player, Rack * opp_rack, uint32_t node_index, int leftstrip, int rightstrip, int unique_play);
void reset_generator(Generator * gen);
void set_start_leave_index(Player * player);

#endif