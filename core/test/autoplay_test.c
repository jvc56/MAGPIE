#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../src/autoplay.h"
#include "../src/thread_control.h"

#include "test_util.h"
#include "testconfig.h"

void autoplay_game_pairs_test(TestConfig *testconfig) {
  Config *csw_config = get_csw_config(testconfig);

  uint64_t seed = time(NULL);
  seed = 1703698211;  // DO NOT MERGE
  char *options_string =
      get_formatted_string("setoptions i 2500 gp threads 11 rs %ld", seed);  // DO NOT MERGE

  load_config_or_die(csw_config, options_string);

  printf("running autoplay with: %s\n", options_string);

  free(options_string);

  AutoplayResults *autoplay_results = create_autoplay_results();

  autoplay_status_t status = autoplay(csw_config, autoplay_results);
  assert(status == AUTOPLAY_STATUS_SUCCESS);
  assert(autoplay_results->total_games == csw_config->max_iterations * 2);
  assert(autoplay_results->p1_firsts == csw_config->max_iterations);
  assert(get_weight(autoplay_results->p1_score) ==
         get_weight(autoplay_results->p2_score));
  assert(get_cardinality(autoplay_results->p1_score) ==
         get_cardinality(autoplay_results->p2_score));
  assert(within_epsilon(get_mean(autoplay_results->p1_score),
                        get_mean(autoplay_results->p2_score)));
  assert(within_epsilon(get_stdev(autoplay_results->p1_score),
                        get_stdev(autoplay_results->p2_score)));

/*
  load_config_or_die(csw_config, "setoptions i 7 nogp threads 2");

  // Autoplay should reset the stats
  status = autoplay(csw_config, autoplay_results);
  assert(status == AUTOPLAY_STATUS_SUCCESS);
  assert(autoplay_results->total_games == csw_config->max_iterations);
*/
  destroy_autoplay_results(autoplay_results);
}

void test_autoplay(TestConfig *testconfig) {
  autoplay_game_pairs_test(testconfig);
}