
#ifndef SIM_STRING_H
#define SIM_STRING_H

#include "../ent/game.h"
#include "../ent/sim_results.h"
#include "../ent/thread_control.h"
#include "../util/string_util.h"

void print_sim_results_table(const Game *game, SimResults *sim_results,
                          ThreadControl *thread_control, double nps,
                          bool print_best_play);
#endif
