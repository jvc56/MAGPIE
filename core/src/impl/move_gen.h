#ifndef MOVE_GEN_H
#define MOVE_GEN_H

#include "../def/move_defs.h"
#include "../def/move_gen_defs.h"

#include "../ent/anchor.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/game.h"
#include "../ent/leave_map.h"
#include "../ent/move.h"
#include "../ent/player.h"

// FIXME: these functions are only used
// for testing and shouldn't exist, need to rethink
// AnchorList *gen_get_anchor_list(MoveGen *gen);
// double *gen_get_best_leaves(MoveGen *gen);
// LeaveMap *gen_get_leave_map(MoveGen *gen);
// void generate_exchange_moves(MoveGen *gen, uint8_t ml, int stripidx,
//                              bool add_exchange);

void generate_moves(const LetterDistribution *ld, const KWG *kwg,
                    const KLV *klv, const Rack *opponent_rack, int thread_index,
                    Board *board, Rack *player_rack, int player_index,
                    int number_of_tiles_in_bag, move_record_t move_record_type,
                    move_sort_t move_sort_type, bool kwgs_are_distinct,
                    int move_list_capacity, MoveList **move_list);

#endif