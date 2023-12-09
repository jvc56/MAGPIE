#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../src/ent/autoplay_results.h"
#include "../src/ent/thread_control.h"

#include "../src/util/string_util.h"

#include "../src/impl/autoplay.h"

#include "test_util.h"
#include "testconfig.h"

void autoplay_game_pairs_test(TestConfig *testconfig) {
  Config *csw_config = get_csw_config(testconfig);

  uint64_t seed = time(NULL);

  char *options_string =
      get_formatted_string("setoptions i 1000 gp threads 11 rs %ld", seed);

  load_config_or_die(csw_config, options_string);

  printf("running autoplay with: %s\n", options_string);

  free(options_string);

  AutoplayResults *autoplay_results = create_autoplay_results();

  autoplay_status_t status = autoplay(csw_config, autoplay_results);
  assert(status == AUTOPLAY_STATUS_SUCCESS);
  int max_iterations = config_get_max_iterations(csw_config);
  assert(get_total_games(autoplay_results) == max_iterations * 2);
  assert(get_p1_firsts(autoplay_results) == max_iterations);
  assert(get_weight(get_p1_score(autoplay_results)) ==
         get_weight(get_p2_score(autoplay_results)));
  assert(get_cardinality(get_p1_score(autoplay_results)) ==
         get_cardinality(get_p2_score(autoplay_results)));
  assert(within_epsilon(get_mean(get_p1_score(autoplay_results)),
                        get_mean(get_p2_score(autoplay_results))));
  assert(within_epsilon(get_stdev(get_p1_score(autoplay_results)),
                        get_stdev(get_p2_score(autoplay_results))));

  load_config_or_die(csw_config, "setoptions i 7 nogp threads 2");
  max_iterations = config_get_max_iterations(csw_config);

  // Autoplay should reset the stats
  status = autoplay(csw_config, autoplay_results);
  assert(status == AUTOPLAY_STATUS_SUCCESS);
  assert(get_total_games(autoplay_results) == max_iterations);

  destroy_autoplay_results(autoplay_results);
}

void test_autoplay(TestConfig *testconfig) {
  autoplay_game_pairs_test(testconfig);
}