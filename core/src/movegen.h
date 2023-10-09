#ifndef MOVEGEN_H
#define MOVEGEN_H

#include <stdint.h>

#include "anchor.h"
#include "bag.h"
#include "board.h"
#include "config.h"
#include "constants.h"
#include "klv.h"
#include "kwg.h"
#include "leave_map.h"
#include "letter_distribution.h"
#include "move.h"
#include "player.h"
#include "rack.h"

#define OPENING_HOTSPOT_PENALTY -0.7
#define PREENDGAME_ADJUSTMENT_VALUES_LENGTH 13
#define BINGO_BONUS 50
#define NON_OUTPLAY_LEAVE_SCORE_MULTIPLIER_PENALTY 2.0
#define NON_OUTPLAY_CONSTANT_PENALTY 10.0

typedef struct Generator {
  int current_row_index;
  int current_anchor_col;
  int last_anchor_col;
  int vertical;
  int tiles_played;
  int number_of_plays;
  int apply_placement_adjustment;
  int kwgs_are_distinct;

  uint8_t row_letter_cache[(BOARD_DIM)];
  uint8_t strip[(BOARD_DIM)];
  uint8_t *exchange_strip;
  double preendgame_adjustment_values[PREENDGAME_ADJUSTMENT_VALUES_LENGTH];

  MoveList *move_list;
  Board *board;
  Bag *bag;

  LeaveMap *leave_map;
  LetterDistribution *letter_distribution;

  // Shadow plays
  int current_left_col;
  int current_right_col;
  double highest_shadow_equity;
  uint64_t rack_cross_set;
  int move_sorting_type;
  int number_of_letters_on_rack;
  int descending_tile_scores[(RACK_SIZE)];
  double best_leaves[(RACK_SIZE)];
  AnchorList *anchor_list;
} Generator;

Generator *create_generator(Config *config);
Generator *copy_generator(Generator *gen, int move_list_size);
void destroy_generator(Generator *gen);
void generate_moves(Generator *gen, Player *player, Rack *opp_rack,
                    int add_exchange);
void recursive_gen(Generator *gen, int col, Player *player, Rack *opp_rack,
                   uint32_t node_index, int leftstrip, int rightstrip,
                   int unique_play);
void reset_generator(Generator *gen);
void load_row_letter_cache(Generator *gen, int row);
int get_cross_set_index(Generator *gen, int player_index);
int score_move(Board *board, uint8_t word[], int word_start_index,
               int word_end_index, int row, int col, int tiles_played,
               int cross_dir, int cross_set_index,
               LetterDistribution *letter_distribution);
#endif