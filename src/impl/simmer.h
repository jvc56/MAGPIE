#ifndef SIMMER_H
#define SIMMER_H

#include "../def/simmer_defs.h"

#include "../ent/config.h"
#include "../ent/game.h"
#include "../ent/sim_results.h"
#include "../ent/validated_move.h"

sim_status_t simulate(const Config *config, const Game *game,
                      const ValidatedMoves *vms, SimResults *sim_results);

#endif
