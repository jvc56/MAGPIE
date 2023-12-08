#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "../def/move_defs.h"
#include "../def/movegen_defs.h"

#include "anchor.h"
#include "bag.h"
#include "board.h"
#include "leave_map.h"
#include "move.h"
#include "player.h"

struct Generator;
typedef struct Generator Generator;

Board *gen_get_board(Generator *gen);
Bag *gen_get_bag(Generator *gen);
LetterDistribution *gen_get_ld(Generator *gen);
MoveList *gen_get_move_list(Generator *gen);
bool gen_get_kwgs_are_distinct(const Generator *gen);
AnchorList *gen_get_anchor_list(Generator *gen);
double *gen_get_best_leaves(Generator *gen);
LeaveMap *gen_get_leave_map(Generator *gen);
int gen_get_current_row_index(const Generator *gen);
int gen_get_current_anchor_col(const Generator *gen);
int get_cross_set_index(const Generator *gen, int player_index);

void gen_set_move_sort_type(Generator *gen, move_sort_t move_sort_type);
void gen_set_move_record_type(Generator *gen, move_record_t move_record_type);
void gen_set_apply_placement_adjustment(Generator *gen,
                                        bool apply_placement_adjustment);
void gen_set_current_anchor_col(Generator *gen, int anchor_col);
void gen_set_current_row_index(Generator *gen, int row_index);
void gen_set_last_anchor_col(Generator *gen, int anchor_col);
void gen_set_dir(Generator *gen, int dir);

Generator *create_generator(const Config *config, int move_list_capacity);
Generator *generate_duplicate(const Generator *gen, int move_list_capacity);
void destroy_generator(Generator *gen);
void update_generator(const Config *config, Generator *gen);
void generate_moves(const Rack *opp_rack, Generator *gen, Player *player,
                    bool add_exchange, move_record_t move_record_type,
                    move_sort_t move_sort_type,
                    bool apply_placement_adjustment);
void reset_generator(Generator *gen);
int score_on_rack(const LetterDistribution *letter_distribution,
                  const Rack *rack);
void load_row_letter_cache(Generator *gen, int row);

void recursive_gen(const Rack *opp_rack, Generator *gen, int col,
                   Player *player, uint32_t node_index, int leftstrip,
                   int rightstrip, bool unique_play);
void generate_exchange_moves(Generator *gen, Player *player, uint8_t ml,
                             int stripidx, bool add_exchange);
int score_move(const Board *board,
               const LetterDistribution *letter_distribution, uint8_t word[],
               int word_start_index, int word_end_index, int row, int col,
               int tiles_played, int cross_dir, int cross_set_index);
#endif