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

#include "../../src/util/string_util.h"

#include "test_util.h"

void test_autoplay_default(void) {
  Config *csw_config =
      config_create_or_die("set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 "
                           "all -numplays 1  -gp true -threads 11");

  load_and_exec_config_or_die(csw_config, "autoplay games 100 -seed 26");

  char *ar1_str = autoplay_results_to_string(
      config_get_autoplay_results(csw_config), false);
  assert_strings_equal(ar1_str, "autoplay games 200 100 100 0 100 460.940000 "
                                "60.687820 460.940000 60.687820\n");

  load_and_exec_config_or_die(csw_config,
                              "autoplay games 100 -r1 best -r2 best -seed 26");

  char *ar2_str = autoplay_results_to_string(
      config_get_autoplay_results(csw_config), false);
  // Autoplay using the "best" move recorder should be the same
  // as autoplay using the "all" move recorder.
  assert_strings_equal(ar1_str, ar2_str);

  load_and_exec_config_or_die(csw_config,
                              "autoplay games 7 -gp false -threads 2 -seed 27");

  // Autoplay should reset the stats
  char *ar3_str = autoplay_results_to_string(
      config_get_autoplay_results(csw_config), false);
  assert_strings_equal(
      ar3_str,
      "autoplay games 7 1 6 0 4 422.000000 58.657196 475.000000 89.701356\n");
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
          config_get_autoplay_results(csw_config), false);
    } else {
      free(multi_thread_str);
      multi_thread_str = autoplay_results_to_string(
          config_get_autoplay_results(csw_config), false);
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
  Config *csw_config =
      config_create_or_die("set -lex CSW21 -s1 equity -s2 equity -r1 best -r2 "
                           "best -numplays 1 -threads 1");

  load_and_exec_config_or_die(csw_config, "create klv CSW21_zeroed");
  load_and_exec_config_or_die(csw_config, "set -leaves CSW21_zeroed");
  load_and_exec_config_or_die(csw_config, "leavegen 2 200 1 0 60 -seed 0");
  load_and_exec_config_or_die(csw_config, "create klv CSW21_zeroed_ml");
  load_and_exec_config_or_die(csw_config,
                              "set -leaves CSW21_zeroed_ml -threads 11");
  load_and_exec_config_or_die(csw_config, "leavegen 2 100 1 0 200");

  config_destroy(csw_config);
}

void test_autoplay(void) {
  test_autoplay_default();
  test_autoplay_leavegen();
}