#ifndef MOVE_GEN_H
#define MOVE_GEN_H

#include "../def/move_defs.h"

#include "../ent/game.h"
#include "../ent/move.h"

void gen_destroy_cache(void);

// If override_kwg is NULL, the full KWG for the on-turn player is used,
// but if it is nonnull, override_kwg is used. The only use case for this
// so far is using a reduced wordlist kwg (done with wordprune) for endgame
// solving.
void generate_moves(Game *game, move_record_t move_record_type,
                    move_sort_t move_sort_type, int thread_index,
                    MoveList *move_list, const KWG *override_kwg);

#endif