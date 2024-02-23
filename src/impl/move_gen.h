#ifndef MOVE_GEN_H
#define MOVE_GEN_H

#include "../def/move_defs.h"

#include "../ent/game.h"
#include "../ent/move.h"

void gen_destroy_cache();
void generate_moves(const Game *game, move_record_t move_record_type,
                    move_sort_t move_sort_type, int thread_index,
                    MoveList *move_list);

#endif