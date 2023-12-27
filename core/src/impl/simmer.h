#ifndef SIMMER_H
#define SIMMER_H

#include "../ent/config.h"
#include "../ent/game.h"
#include "../ent/sim_results.h"

sim_status_t simulate(const Config *config, const Game *game,
                      SimResults *sim_results);

#endif
