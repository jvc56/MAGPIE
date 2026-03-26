#ifndef SIMMER_H
#define SIMMER_H

#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/sim_args.h"
#include "../ent/sim_results.h"
#include "../util/io_util.h"
#include "random_variable.h"

typedef struct SimCtx SimCtx;

void simulate(SimArgs *sim_args, SimCtx **sim_ctx, SimResults *sim_results,
              ErrorStack *error_stack);
void simulate_without_ctx(SimArgs *sim_args, SimResults *sim_results,
                          ErrorStack *error_stack);
void sim_ctx_destroy(SimCtx *sim_ctx);
const Move *get_top_simming_move(Game *game, int movegen_index,
                                 MoveList *move_list, SimArgs *sim_args,
                                 SimCtx **sim_ctx, SimResults *sim_results,
                                 ErrorStack *error_stack);
#endif
