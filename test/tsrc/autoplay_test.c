#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../../src/def/autoplay_defs.h"

#include "../../src/ent/autoplay_results.h"
#include "../../src/ent/stats.h"

#include "../../src/impl/autoplay.h"
#include "../../src/impl/config.h"

#include "../../src/util/math_util.h"
#include "../../src/util/string_util.h"

#include "test_constants.h"
#include "test_util.h"

void test_odds_that_player_is_better(void) {
  assert(within_epsilon(odds_that_player_is_better(0.6, 10), 73.645537));
  assert(within_epsilon(odds_that_player_is_better(0.6, 100), 97.724987));
  assert(within_epsilon(odds_that_player_is_better(0.6, 1000), 100.0));
  assert(within_epsilon(odds_that_player_is_better(0.7, 10), 89.704839));
  assert(within_epsilon(odds_that_player_is_better(0.7, 100), 99.996833));
  assert(within_epsilon(odds_that_player_is_better(0.7, 1000), 100.0));
  assert(within_epsilon(odds_that_player_is_better(0.9, 10), 99.429398));
  assert(within_epsilon(odds_that_player_is_better(0.9, 100), 100.0));
  assert(within_epsilon(odds_that_player_is_better(0.9, 1000), 100.0));
}

void test_autoplay_default(void) {
  Config *csw_config =
      config_create_or_die("set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 "
                           "all -numplays 1  -gp true -threads 11");

  load_and_exec_config_or_die(csw_config, "autoplay games 100 -seed 26");

  char *ar1_str = autoplay_results_to_string(
      config_get_autoplay_results(csw_config), false, false);
  assert_strings_equal(ar1_str, "autoplay games 200 100 100 0 100 460.940000 "
                                "60.687820 460.940000 60.687820 0 200 0 \n");

  load_and_exec_config_or_die(csw_config,
                              "autoplay games 100 -r1 best -r2 best -seed 26");

  char *ar2_str = autoplay_results_to_string(
      config_get_autoplay_results(csw_config), false, false);
  // Autoplay using the "best" move recorder should be the same
  // as autoplay using the "all" move recorder.
  assert_strings_equal(ar1_str, ar2_str);

  load_and_exec_config_or_die(csw_config,
                              "autoplay games 7 -gp false -threads 2 -seed 27");

  // Autoplay should reset the stats
  char *ar3_str = autoplay_results_to_string(
      config_get_autoplay_results(csw_config), false, false);
  assert_strings_equal(ar3_str, "autoplay games 7 1 6 0 4 422.000000 58.657196 "
                                "475.000000 89.701356 0 7 0 \n");
  // Ensure pseudo-randomness is consistent for any number of threads
  char *single_thread_str = NULL;
  char *multi_thread_str = NULL;
  for (int i = 0; i < 11; i++) {
    char *options_string =
        get_formatted_string("autoplay games 20 -r1 best -r2 best -gp false "
                             "-threads %d -seed 28",
                             i + 1);

    load_and_exec_config_or_die(csw_config, options_string);

    free(options_string);

    if (i == 0) {
      single_thread_str = autoplay_results_to_string(
          config_get_autoplay_results(csw_config), false, false);
    } else {
      free(multi_thread_str);
      multi_thread_str = autoplay_results_to_string(
          config_get_autoplay_results(csw_config), false, false);
      assert_strings_equal(single_thread_str, multi_thread_str);
    }
  }

  free(ar1_str);
  free(ar2_str);
  free(ar3_str);
  free(single_thread_str);
  free(multi_thread_str);
  config_destroy(csw_config);
}

void test_autoplay_leavegen(void) {
  Config *ab_config = config_create_or_die(
      "set -lex CSW21_ab -ld english_ab -s1 equity -s2 equity -r1 best -r2 "
      "best -numplays 1 -threads 1");

  // The minimum leave count should be achieved well before reaching
  // the maximum number of games, so if this takes too long, we know it
  // failed.
  load_and_exec_config_or_die_timed(ab_config,
                                    "leavegen 1 99999999 2 0 30 -seed 3", 60);

  // The maximum game limit should be reached well before reaching
  // the minimum leave count.
  load_and_exec_config_or_die_timed(ab_config,
                                    "leavegen 1 200 99999999 0 60 -seed 3", 60);

  char *ab_ar_str = autoplay_results_to_string(
      config_get_autoplay_results(ab_config), false, false);
  assert_strings_equal(ab_ar_str, "autoplay games 200 93 105 2 100 287.450000 "
                                  "59.948366 290.285000 66.045315 153 0 47 \n");
  free(ab_ar_str);

  config_destroy(ab_config);

  Config *csw_config =
      config_create_or_die("set -lex CSW21 -s1 equity -s2 equity -r1 best -r2 "
                           "best -numplays 1 -threads 1");

  // Make sure the leavegen command can run without error.
  load_and_exec_config_or_die(csw_config, "create klv CSW21_zeroed english");
  load_and_exec_config_or_die(csw_config, "set -leaves CSW21_zeroed");
  load_and_exec_config_or_die(csw_config, "leavegen 2 200 1 0 60 -seed 0");
  load_and_exec_config_or_die(csw_config, "create klv CSW21_zeroed_ml english");
  load_and_exec_config_or_die(csw_config,
                              "set -leaves CSW21_zeroed_ml -threads 11");
  load_and_exec_config_or_die(csw_config, "leavegen 2 100 1 0 200");

  config_destroy(csw_config);
}

void test_autoplay_divergent_games(void) {
  Config *csw_config =
      config_create_or_die("set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 "
                           "all -numplays 1  -gp true -threads 11");
  Game *game;

  AutoplayResults *ar = autoplay_results_create();

  autoplay_status_t status = autoplay_results_set_options(ar, "games");
  assert(status == AUTOPLAY_STATUS_SUCCESS);

  load_and_exec_config_or_die(csw_config, "cgp " VS_ANDY_CGP);
  game = config_get_game(csw_config);
  autoplay_results_add_game(ar, game, 20, false);

  load_and_exec_config_or_die(csw_config, "cgp " VS_FRENTZ_CGP);
  game = config_get_game(csw_config);
  autoplay_results_add_game(ar, game, 20, true);

  load_and_exec_config_or_die(csw_config, "cgp " MANY_MOVES);
  game = config_get_game(csw_config);
  autoplay_results_add_game(ar, game, 20, false);

  load_and_exec_config_or_die(csw_config, "cgp " UEY_CGP);
  game = config_get_game(csw_config);
  autoplay_results_add_game(ar, game, 20, true);

  char *ar_str = autoplay_results_to_string(ar, false, true);
  assert_strings_equal(ar_str, "autoplay games 4 1 3 0 4 251.750000 78.270365 "
                               "279.250000 67.667693 4 0 0 \n"
                               "autoplay games 2 0 2 0 2 295.500000 34.648232 "
                               "328.000000 63.639610 2 0 0 \n");
  free(ar_str);

  autoplay_results_destroy(ar);

  load_and_exec_config_or_die(csw_config,
                              "autoplay games 50 -seed 50 -lex CSW21 -gp true");

  // There should be no divergent games for CSW21 vs CSW21
  char *ar_gp_same_lex_str = autoplay_results_to_string(
      config_get_autoplay_results(csw_config), false, true);
  assert_strings_equal(
      ar_gp_same_lex_str,
      "autoplay games 100 50 50 0 50 465.610000 61.689495 "
      "465.610000 61.689495 0 100 0 \nautoplay games 0 0 0 0 0 "
      "0.000000 0.000000 0.000000 0.000000 0 0 0 \n");
  free(ar_gp_same_lex_str);

  load_and_exec_config_or_die(
      csw_config, "autoplay games 50 -seed 50 -l1 CSW21 -l2 NWL20 -gp true");

  char *ar_gp_diff_lex_str = autoplay_results_to_string(
      config_get_autoplay_results(csw_config), false, true);
  assert_strings_equal(
      ar_gp_diff_lex_str,
      "autoplay games 100 70 30 0 50 472.030000 59.216510 "
      "423.730000 62.515591 0 100 0 \nautoplay games 100 70 30 0 50 "
      "472.030000 59.216510 423.730000 62.515591 0 100 0 \n");
  free(ar_gp_diff_lex_str);

  config_destroy(csw_config);
}

void test_autoplay(void) {
  test_odds_that_player_is_better();
  test_autoplay_default();
  test_autoplay_leavegen();
  test_autoplay_divergent_games();
}