#ifndef SIMMER_H
#define SIMMER_H

#include "../def/simmer_defs.h"

#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/sim_results.h"
#include "config.h"

sim_status_t simulate(const Config *config, const Game *input_game,
                      const MoveList *move_list, Rack *known_opp_rack,
                      SimResults *sim_results);

#endif
