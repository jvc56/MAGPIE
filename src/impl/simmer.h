#ifndef SIMMER_H
#define SIMMER_H

#include "../def/simmer_defs.h"

#include "../ent/config.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/sim_results.h"

sim_status_t simulate(const Config *config, const Game *game,
                      const MoveList *move_list, SimResults *sim_results);

#endif
