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
  int dir;
  int max_tiles_to_play;
  int tiles_played;
  int number_of_plays;
  int move_sort_type;
  int move_record_type;
  bool kwgs_are_distinct;
  bool apply_placement_adjustment;

  uint8_t row_letter_cache[(BOARD_DIM)];
  bool is_cross_word_cache[(BOARD_DIM)];
  uint8_t bonus_square_cache[(BOARD_DIM)];
  uint64_t cross_set_cache[(BOARD_DIM)];
  int bag_tiles_remaining;
  
  uint8_t strip[(BOARD_DIM)];
  uint8_t *exchange_strip;
  double preendgame_adjustment_values[PREENDGAME_ADJUSTMENT_VALUES_LENGTH];

  MoveList *move_list;
  Board *board;
  Bag *bag;

  LeaveMap *leave_map;
  const LetterDistribution *letter_distribution;

  // Shadow plays
  int current_left_col;
  int current_right_col;
  double highest_shadow_equity;
  uint64_t rack_cross_set;
  int number_of_letters_on_rack;
  int descending_tile_scores[(RACK_SIZE)];
  double best_leaves[(RACK_SIZE)];
  AnchorList *anchor_list;
} Generator;

Generator *create_generator(const Config *config, int move_list_capacity);
Generator *generate_duplicate(const Generator *gen, int move_list_capacity);
void destroy_generator(Generator *gen);
void update_generator(const Config *config, Generator *gen);
void generate_moves(const Rack *opp_rack, Generator *gen, Player *player,
                    bool add_exchange, move_record_t move_record_type,
                    move_sort_t move_sort_type,
                    bool apply_placement_adjustment);
void generate_exchange_moves(Generator *gen, Player *player, uint8_t ml,
                             int stripidx, bool add_exchange);
void recursive_gen(const Rack *opp_rack, Generator *gen, int col,
                   Player *player, uint32_t node_index, int leftstrip,
                   int rightstrip, bool unique_play);
void reset_generator(Generator *gen);
void load_row_letter_cache(Generator *gen, int row);
int get_cross_set_index(const Generator *gen, int player_index);
int score_move(const Board *board,
               const LetterDistribution *letter_distribution, uint8_t word[],
               int word_start_index, int word_end_index, int row, int col,
               int tiles_played, int cross_dir, int cross_set_index);
#endif