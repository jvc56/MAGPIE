#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../../src/def/autoplay_defs.h"

#include "../../src/ent/autoplay_results.h"
#include "../../src/ent/stats.h"
#include "../../src/impl/config.h"

#include "../../src/impl/autoplay.h"

#include "../../src/util/string_util.h"

#include "test_util.h"

void assert_stats_are_equal(Stat *s1, Stat *s2) {
  assert(stat_get_weight(s1) == stat_get_weight(s2));
  assert(stat_get_cardinality(s1) == stat_get_cardinality(s2));
  assert(within_epsilon(stat_get_mean(s1), stat_get_mean(s2)));
  assert(within_epsilon(stat_get_stdev(s1), stat_get_stdev(s2)));
}

void assert_autoplay_results_are_equal(AutoplayResults *ar1,
                                       AutoplayResults *ar2) {
  assert(autoplay_results_get_games(ar1) == autoplay_results_get_games(ar2));
  assert(autoplay_results_get_p1_firsts(ar1) ==
         autoplay_results_get_p1_firsts(ar2));
  assert(autoplay_results_get_p1_wins(ar1) ==
         autoplay_results_get_p1_wins(ar2));
  assert(autoplay_results_get_p1_losses(ar1) ==
         autoplay_results_get_p1_losses(ar2));
  assert(autoplay_results_get_p1_ties(ar1) ==
         autoplay_results_get_p1_ties(ar2));

  Stat *ar1s1 = autoplay_results_get_p1_score(ar1);
  Stat *ar1s2 = autoplay_results_get_p2_score(ar1);
  Stat *ar2s1 = autoplay_results_get_p1_score(ar2);
  Stat *ar2s2 = autoplay_results_get_p2_score(ar2);

  assert_stats_are_equal(ar1s1, ar2s1);
  assert_stats_are_equal(ar1s2, ar2s2);
}

void autoplay_game_pairs_test(void) {
  uint64_t seed = time(NULL);
  char *options_string = get_formatted_string(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -it 200 -gp true -threads 11 -seed %ld",
      seed);
  Config *csw_config = config_create_or_die(options_string);
  printf("running autoplay with: %s\n", options_string);

  free(options_string);

  AutoplayResults *ar1 = autoplay_results_create();

  autoplay_status_t status = config_autoplay(csw_config, ar1);
  assert(status == AUTOPLAY_STATUS_SUCCESS);
  int max_iterations = config_get_max_iterations(csw_config);
  assert(autoplay_results_get_games(ar1) == max_iterations * 2);
  assert(autoplay_results_get_p1_firsts(ar1) == max_iterations);
  assert_stats_are_equal(autoplay_results_get_p1_score(ar1),
                         autoplay_results_get_p2_score(ar1));

  // Random seeds is a transient field, so it must be set again
  options_string =
      get_formatted_string("set -r1 best -r2 best -seed %ld", seed);

  load_and_exec_config_or_die(csw_config, options_string);

  printf("running autoplay with: %s\n", options_string);

  free(options_string);

  AutoplayResults *ar2 = autoplay_results_create();

  status = config_autoplay(csw_config, ar2);
  assert(status == AUTOPLAY_STATUS_SUCCESS);
  max_iterations = config_get_max_iterations(csw_config);
  assert(autoplay_results_get_games(ar2) == max_iterations * 2);
  assert(autoplay_results_get_p1_firsts(ar2) == max_iterations);
  assert_stats_are_equal(autoplay_results_get_p1_score(ar2),
                         autoplay_results_get_p2_score(ar2));

  // Autoplay using the "best" move recorder should be the same
  // as autoplay using the "all" move recorder.
  assert_autoplay_results_are_equal(ar1, ar2);

  load_and_exec_config_or_die(csw_config, "set -it 7 -gp false -threads 2");
  max_iterations = config_get_max_iterations(csw_config);

  // Autoplay should reset the stats
  status = config_autoplay(csw_config, ar1);
  assert(status == AUTOPLAY_STATUS_SUCCESS);
  assert(autoplay_results_get_games(ar1) == max_iterations);

  // Ensure pseudo-randomness is consistent for any number of threads
  for (int i = 0; i < 11; i++) {
    options_string = get_formatted_string(
        "set -r1 best -r2 best -gp false -threads %d -seed %ld -it 20", i + 1,
        seed);

    load_and_exec_config_or_die(csw_config, options_string);

    free(options_string);

    if (i == 0) {
      status = config_autoplay(csw_config, ar1);
      assert(status == AUTOPLAY_STATUS_SUCCESS);
    } else {
      status = config_autoplay(csw_config, ar2);
      assert(status == AUTOPLAY_STATUS_SUCCESS);
      assert_autoplay_results_are_equal(ar1, ar2);
    }
  }

  autoplay_results_destroy(ar1);
  autoplay_results_destroy(ar2);
  config_destroy(csw_config);
}

void test_autoplay(void) { autoplay_game_pairs_test(); }