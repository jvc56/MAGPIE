
#ifndef SIM_STRING_H
#define SIM_STRING_H

#include "../ent/game.h"
#include "../ent/sim_results.h"
#include "../ent/thread_control.h"

#include "../util/string_util.h"

char *ucgi_sim_stats(Game *game, SimResults *sim_results,
                     ThreadControl *thread_control, bool print_best_play);
void print_ucgi_sim_stats(Game *game, SimResults *sim_results,
                          ThreadControl *thread_control, bool print_best_play);
#endif
