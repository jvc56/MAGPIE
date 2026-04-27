#ifndef BINGO_PROB_H
#define BINGO_PROB_H

#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/xoshiro.h"
#include <stdint.h>

// Sample-based estimate of the probability that a rack of the form
//   leave + (random hypergeometric draw of draw_size from pool)
// can play a bingo on the current board.
//
// Uses bingo_exists internally. Temporarily mutates and restores the
// on-turn player's rack. Caller is responsible for setting on-turn to
// the player whose perspective we're computing for (cross_set_index
// follows on-turn when KWGs aren't shared).
//
// Returns 0.0 if num_samples <= 0 or draw_size > pool_size.
double bingo_prob_sampled(Game *game, MoveList *mv_list, int thread_index,
                          const uint8_t *leave_counts,
                          const uint8_t *pool_counts, int pool_size,
                          int draw_size, int num_samples, XoshiroPRNG *prng);

#endif
