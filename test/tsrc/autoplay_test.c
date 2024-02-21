#include "../../src/impl/autoplay.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../../src/def/autoplay_defs.h"
#include "../../src/ent/autoplay_results.h"
#include "../../src/ent/config.h"
#include "../../src/ent/stats.h"
#include "../../src/util/string_util.h"
#include "test_util.h"

void autoplay_game_pairs_test() {
  Config *csw_config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");

  uint64_t seed = time(NULL);

  char *options_string =
      get_formatted_string("setoptions rounds 1000 gp threads 11 rs %ld", seed);

  load_config_or_die(csw_config, options_string);

  printf("running autoplay with: %s\n", options_string);

  free(options_string);

  AutoplayResults *autoplay_results = autoplay_results_create();

  autoplay_status_t status = autoplay(csw_config, autoplay_results);
  assert(status == AUTOPLAY_STATUS_SUCCESS);
  int rounds = config_get_num_autoplay_rounds(csw_config);
  assert(autoplay_results_get_games(autoplay_results) == rounds * 2);
  assert(autoplay_results_get_p1_firsts(autoplay_results) == rounds);
  assert(stat_get_weight(autoplay_results_get_p1_score(autoplay_results)) ==
         stat_get_weight(autoplay_results_get_p2_score(autoplay_results)));
  assert(
      stat_get_cardinality(autoplay_results_get_p1_score(autoplay_results)) ==
      stat_get_cardinality(autoplay_results_get_p2_score(autoplay_results)));
  assert(within_epsilon(
      stat_get_mean(autoplay_results_get_p1_score(autoplay_results)),
      stat_get_mean(autoplay_results_get_p2_score(autoplay_results))));
  assert(within_epsilon(
      stat_get_stdev(autoplay_results_get_p1_score(autoplay_results)),
      stat_get_stdev(autoplay_results_get_p2_score(autoplay_results))));

  load_config_or_die(csw_config, "setoptions rounds 7 nogp threads 2");
  rounds = config_get_num_autoplay_rounds(csw_config);

  // Autoplay should reset the stats
  status = autoplay(csw_config, autoplay_results);
  assert(status == AUTOPLAY_STATUS_SUCCESS);
  assert(autoplay_results_get_games(autoplay_results) == rounds);

  autoplay_results_destroy(autoplay_results);
  config_destroy(csw_config);
}

void test_autoplay() { autoplay_game_pairs_test(); }