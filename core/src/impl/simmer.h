#ifndef SIMMER_H
#define SIMMER_H

#include "../ent/config.h"
#include "../ent/game.h"
#include "../ent/sim_results.h"

#include "move_gen.h"

sim_status_t simulate(const Config *config, Game *game, MoveGen *gen,
                      SimResults **sim_results);

#endif
