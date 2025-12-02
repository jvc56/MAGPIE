
#ifndef SIM_STRING_H
#define SIM_STRING_H

#include "../ent/game.h"
#include "../ent/sim_results.h"
#include "../ent/thread_control.h"
#include "../util/string_util.h"

char *string_builder_add_sim_stats(StringBuilder *sb, const Game *game,
                                   SimResults *sim_results,
                                   bool use_ucgi_format);
char *sim_results_get_string(const Game *game, SimResults *sim_results,
                             int max_num_display_plays, bool use_ucgi_format);
void sim_results_print(ThreadControl *thread_control, const Game *game,
                       SimResults *sim_results, int max_num_display_plays,
                       bool use_ucgi_format);

#endif
