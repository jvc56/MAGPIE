#ifndef SIMMER_H
#define SIMMER_H

#include "../ent/sim_results.h"
#include "../util/error_stack.h"

#include "random_variable.h"

void simulate(const SimArgs *sim_args, SimResults *sim_results,
              ErrorStack *error_stack);

#endif
