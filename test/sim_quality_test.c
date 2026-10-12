// Sim quality check at a fixed sample budget.
//
// Player 1 sims every move with a fixed iteration budget and no clock;
// player 2 plays the top static equity move. Game pairs (-gp true) replay
// identical tile draws with the seats swapped. Every sim gets one thread
// (-mtmode pgp), so the results depend only on how the scheduler allocates a
// sim's samples, not on thread timing, and a build's results can be compared
// with another's on the same seed: a scheduler that allocates worse wins
// fewer games against the same static opponent.
//
// 200 game pairs on seed 1, one concurrent game per core. Player 1 sims 15
// plays at 2 plies with -iterations 1000 and -minplayiterations 10.

#include "sim_quality_test.h"

#include "../src/compat/memory_info.h"
#include "../src/ent/autoplay_results.h"
#include "../src/impl/config.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <stdio.h>
#include <stdlib.h>

void test_sim_quality(void) {
  char *settings = get_formatted_string(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-pl1 2 -pl2 0 -np1 15 -np2 1 -i1 1000 -mi1 10 -sc1 none -tl1 0 "
      "-threads %d -mtmode pgp -savesettings false -autosavegcg false",
      get_num_cores());
  Config *config = config_create_or_die(settings);
  free(settings);
  load_and_exec_config_or_die(config, "autoplay games 200 -gp true -seed 1");
  char *results = autoplay_results_to_string(
      config_get_autoplay_results(config), true, true);
  printf("SIMQ pairs=200 iters=1000 plies=2 plays=15 minplay=10 seed=1\n%s\n",
         results);
  free(results);
  config_destroy(config);
}
