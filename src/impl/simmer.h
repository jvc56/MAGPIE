#ifndef SIMMER_H
#define SIMMER_H

#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/sim_args.h"
#include "../ent/sim_results.h"
#include "../util/io_util.h"
#include "random_variable.h"
#include <stdbool.h>

typedef struct SimCtx SimCtx;

// How many plays a simulating player's candidates can hold beyond its best:
// a pass, an exchange, the exchange that throws every tile worth two or more,
// and a play of one tile and of two.
enum { SIM_NUMBER_OF_TEMPO_CANDIDATES = 5 };

void simulate(SimArgs *sim_args, SimCtx **sim_ctx, SimResults *sim_results,
              ErrorStack *error_stack);
void simulate_without_ctx(SimArgs *sim_args, SimResults *sim_results,
                          ErrorStack *error_stack);
void sim_ctx_destroy(SimCtx *sim_ctx);
// Fills move_list with the candidates of a simulating player on turn: its
// num_plays best plays, and any of these that they don't include: a pass, the
// best exchange, the exchange that throws back every tile worth two points or
// more, the best play of one tile and the best of two. move_list is resized
// to hold them. Creates *sim_ctx if it is NULL.
void sim_generate_candidates(Game *game, MoveList *move_list, int num_plays,
                             SimCtx **sim_ctx);
const Move *get_top_simming_move(Game *game, MoveList *move_list,
                                 SimArgs *sim_args, SimCtx **sim_ctx,
                                 SimResults *sim_results, bool *simulated,
                                 ErrorStack *error_stack);
#endif
