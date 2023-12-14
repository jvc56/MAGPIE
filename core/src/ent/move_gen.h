#ifndef MOVE_GEN_H
#define MOVE_GEN_H

#include "../def/move_defs.h"
#include "../def/move_gen_defs.h"

#include "anchor.h"
#include "bag.h"
#include "board.h"
#include "game.h"
#include "leave_map.h"
#include "move.h"
#include "player.h"

struct MoveGen;
typedef struct MoveGen MoveGen;

void gen_reset_output(MoveGen *gen);
MoveList *gen_get_move_list(MoveGen *gen);

bool gen_get_kwgs_are_distinct(const MoveGen *gen);
AnchorList *gen_get_anchor_list(MoveGen *gen);
double *gen_get_best_leaves(MoveGen *gen);
LeaveMap *gen_get_leave_map(MoveGen *gen);
int gen_get_current_row_index(const MoveGen *gen);
int gen_get_current_anchor_col(const MoveGen *gen);
int get_cross_set_index(bool kwgs_are_distinct, int player_index);

void gen_set_move_sort_type(MoveGen *gen, move_sort_t move_sort_type);
void gen_set_move_record_type(MoveGen *gen, move_record_t move_record_type);
void gen_set_current_anchor_col(MoveGen *gen, int anchor_col);
void gen_set_current_row_index(MoveGen *gen, int row_index);
void gen_set_last_anchor_col(MoveGen *gen, int anchor_col);
void gen_set_dir(MoveGen *gen, int dir);

MoveGen *create_generator(int letter_distribution_size);
void destroy_generator(MoveGen *gen);
void update_generator(const Config *config, MoveGen *gen);
void generate_moves(const LetterDistribution *ld, const KWG *kwg,
                    const KLV *klv, const Rack *opponent_rack, MoveGen *gen,
                    MoveList *move_list, Board *board, Rack *player_rack,
                    int player_index, int number_of_tiles_in_bag,
                    move_record_t move_record_type, move_sort_t move_sort_type,
                    bool kwgs_are_distinct);
int score_on_rack(const LetterDistribution *letter_distribution,
                  const Rack *rack);
void load_row_letter_cache(MoveGen *gen, int row);
int allowed(uint64_t cross_set, uint8_t letter);

void generate_exchange_moves(MoveGen *gen, uint8_t ml, int stripidx,
                             bool add_exchange);
int score_move(const Board *board,
               const LetterDistribution *letter_distribution, uint8_t word[],
               int word_start_index, int word_end_index, int row, int col,
               int tiles_played, int cross_dir, int cross_set_index);
#endif