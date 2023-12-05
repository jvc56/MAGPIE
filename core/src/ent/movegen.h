#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "../def/move_defs.h"
#include "../def/movegen_defs.h"

#include "player.h"

struct Generator;
typedef struct Generator Generator;

Board *gen_get_board(Generator *gen);
Bag *gen_get_bag(Generator *gen);
LetterDistribution *gen_get_ld(Generator *gen);
MoveList *gen_get_move_list(Generator *gen);
bool *gen_get_kwgs_are_distinct(Generator *gen);
AnchorList *gen_get_anchor_list(Generator *gen);
double *gen_get_best_leaves(Generator *gen);
LeaveMap *gen_get_leave_map(Generator *gen);

Generator *create_generator(const Config *config, int move_list_capacity);
Generator *generate_duplicate(const Generator *gen, int move_list_capacity);
void destroy_generator(Generator *gen);
void update_generator(const Config *config, Generator *gen);
void generate_moves(const Rack *opp_rack, Generator *gen, Player *player,
                    bool add_exchange, move_record_t move_record_type,
                    move_sort_t move_sort_type,
                    bool apply_placement_adjustment);
int score_on_rack(const LetterDistribution *letter_distribution,
                  const Rack *rack);
#endif