#ifndef SIMMER_H
#define SIMMER_H

#include <pthread.h>
#include <stdbool.h>

#include "../ent/config.h"
#include "../ent/game.h"
#include "../ent/sim_results.h"

#include "move_gen.h"

struct Simmer;
typedef struct Simmer Simmer;

// FIXME: only used for testing
bool plays_are_similar(const SimmedPlay *m1, const SimmedPlay *m2,
                       Simmer *simmer);

Simmer *create_simmer(const Config *config);
void destroy_simmer(Simmer *simmer);
int simmer_get_iteration_count(Simmer *simmer);
sim_status_t simulate(const Config *config, Game *game, MoveGen *gen,
                      Simmer *simmer);
SimResults *simmer_get_sim_results(Simmer *simmer);

#endif
