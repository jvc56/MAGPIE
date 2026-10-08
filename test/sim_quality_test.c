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
// Env vars:
//   SIMQ_PAIRS    game pairs (default 200)
//   SIMQ_ITERS    -iterations per sim (default 1000)
//   SIMQ_PLIES    sim plies (default 2)
//   SIMQ_PLAYS    candidate plays simmed (default 15)
//   SIMQ_MINPLAY  -minplayiterations (default 10)
//   SIMQ_THREADS  concurrent games (default <cores>)
//   SIMQ_SEED     autoplay seed (default 1)

#include "sim_quality_test.h"

#include "../src/compat/memory_info.h"
#include "../src/ent/autoplay_results.h"
#include "../src/impl/config.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <stdio.h>
#include <stdlib.h>

static const char *simq_env_or(const char *name, const char *fallback) {
  const char *value = getenv(name);
  return value != NULL && value[0] != '\0' ? value : fallback;
}

void test_sim_quality(void) {
  char default_threads[16];
  (void)snprintf(default_threads, sizeof(default_threads), "%d",
                 get_num_cores());
  const char *pairs = simq_env_or("SIMQ_PAIRS", "200");
  const char *iters = simq_env_or("SIMQ_ITERS", "1000");
  const char *plies = simq_env_or("SIMQ_PLIES", "2");
  const char *plays = simq_env_or("SIMQ_PLAYS", "15");
  const char *min_play = simq_env_or("SIMQ_MINPLAY", "10");
  const char *threads = simq_env_or("SIMQ_THREADS", default_threads);
  const char *seed = simq_env_or("SIMQ_SEED", "1");

  char *settings = get_formatted_string(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-pl1 %s -pl2 0 -np1 %s -np2 1 -i1 %s -mi1 %s -sc1 none -tl1 0 "
      "-threads %s -mtmode pgp -savesettings false -autosavegcg false",
      plies, plays, iters, min_play, threads);
  Config *config = config_create_or_die(settings);
  free(settings);
  char *command =
      get_formatted_string("autoplay games %s -gp true -seed %s", pairs, seed);
  load_and_exec_config_or_die(config, command);
  free(command);
  char *results = autoplay_results_to_string(
      config_get_autoplay_results(config), true, true);
  printf("SIMQ pairs=%s iters=%s plies=%s plays=%s minplay=%s seed=%s\n%s\n",
         pairs, iters, plies, plays, min_play, seed, results);
  free(results);
  config_destroy(config);
}
