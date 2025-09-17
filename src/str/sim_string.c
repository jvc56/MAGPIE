#include "../def/sim_defs.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/sim_results.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"
#include "../util/string_util.h"
#include "move_string.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void print_ucgi_sim_stats(const Game *game, SimResults *sim_results,
                          ThreadControl *thread_control, double nps,
                          bool print_best_play) {
  char *sim_stats_string =
      ucgi_sim_stats(game, sim_results, nps, print_best_play);
  thread_control_print(thread_control, sim_stats_string);
  free(sim_stats_string);
}