#ifndef SIM_FWD_H
#define SIM_FWD_H

// Minimal forward declarations for the simmer API.  This header exists so
// that modules (e.g. inference.c) that call simulate() but are also included
// by simmer.c's dependency chain (via random_variable.h → inference.h) can
// get SimCtx and simulate() without creating a circular include dependency.
// simmer.h includes this header so consumers still get the same declarations.

#include "../ent/sim_args.h"
#include "../ent/sim_results.h"
#include "../util/io_util.h"

typedef struct SimCtx SimCtx;

void simulate(SimArgs *sim_args, SimCtx **sim_ctx, SimResults *sim_results,
              ErrorStack *error_stack);
void sim_ctx_destroy(SimCtx *sim_ctx);

#endif
