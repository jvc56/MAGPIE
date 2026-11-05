#ifndef SIMMER_H
#define SIMMER_H

#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/sim_args.h"
#include "../ent/sim_results.h"
#include "../util/io_util.h"
#include "random_variable.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct SimCtx SimCtx;

void simulate(SimArgs *sim_args, SimCtx **sim_ctx, SimResults *sim_results,
              ErrorStack *error_stack);
void simulate_without_ctx(SimArgs *sim_args, SimResults *sim_results,
                          ErrorStack *error_stack);
void sim_ctx_destroy(SimCtx *sim_ctx);
// The memory, in bytes, that the sim context holds for the census of its last
// sim (none if NULL, or once a sim goes without the census).
uint64_t sim_ctx_census_rollout_held_bytes(const SimCtx *sim_ctx);
const Move *get_top_simming_move(Game *game, MoveList *move_list,
                                 SimArgs *sim_args, SimCtx **sim_ctx,
                                 SimResults *sim_results, bool *simulated,
                                 ErrorStack *error_stack);
#endif
