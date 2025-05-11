#ifndef SIMMER_H
#define SIMMER_H

#include "../def/simmer_defs.h"

#include "../ent/error_stack.h"
#include "../ent/sim_results.h"

#include "random_variable.h"

sim_status_t simulate(const SimArgs *sim_args, SimResults *sim_results,
                      ErrorStack *error_stack);

#endif
