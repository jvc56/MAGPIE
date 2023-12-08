
#ifndef SIM_STRING_H
#define SIM_STRING_H

#include "../ent/simmer.h"

#include "../util/string_util.h"

char *ucgi_sim_stats(Game *game, Simmer *simmer, ThreadControl *thread_control,
                     bool print_best_play);
void print_ucgi_sim_stats(Game *game, Simmer *simmer,
                          ThreadControl *thread_control, bool print_best_play);
#endif
