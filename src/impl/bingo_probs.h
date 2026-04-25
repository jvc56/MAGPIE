#ifndef BINGO_PROBS_H
#define BINGO_PROBS_H

#include "../ent/game.h"
#include "../util/io_util.h"
#include <stdint.h>

// Computes opponent-bingo and self-bingo probabilities for the current
// game state and returns a formatted report (caller frees).
//
// Two scenarios reported:
// 1. Opponent bingo: probability that the opponent (not on turn) holds
//    a bingo-able rack, given that their actual rack is unknown to us
//    and was drawn from the unseen pool (bag + opponent's rack).
// 2. Self bingo after opp pass + replenish: if opponent passes (board
//    unchanged) and we draw from the bag to refill our rack to
//    RACK_SIZE, probability the resulting rack can bingo.
//
// If sample_count > 0, runs Monte Carlo with that many samples per
// scenario. Otherwise enumerates every distinct multiset exhaustively.
// num_threads >= 1 partitions the work.
//
// Requires a WMP-enabled lexicon (uses bingo_exists internally).
char *bingo_probs_run(const Game *game, int num_threads, uint64_t sample_count,
                      ErrorStack *error_stack);

#endif
