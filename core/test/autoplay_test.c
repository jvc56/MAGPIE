#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../src/autoplay.h"

#include "testconfig.h"

void autoplay_game_pairs_test(TestConfig *testconfig) {
  Config *csw_config = get_csw_config(testconfig);
  int game_pairs = 1000;
  int number_of_threads = 11;
  int original_number_of_game_pairs = csw_config->number_of_games_or_pairs;
  csw_config->number_of_games_or_pairs = game_pairs;
  int original_number_of_threads = csw_config->number_of_threads;
  csw_config->number_of_threads = number_of_threads;
  int original_use_game_pairs = csw_config->use_game_pairs;
  csw_config->use_game_pairs = 1;

  ThreadControl *thread_control = create_thread_control_from_config(csw_config);
  AutoplayResults *autoplay_results = create_autoplay_results();

  autoplay_status_t status =
      autoplay(thread_control, autoplay_results, csw_config, 0);
  assert(status == AUTOPLAY_STATUS_SUCCESS);
  assert(autoplay_results->total_games == game_pairs * 2);
  assert(autoplay_results->p1_firsts == game_pairs);
  assert(get_weight(autoplay_results->p1_score) ==
         get_weight(autoplay_results->p2_score));
  assert(get_cardinality(autoplay_results->p1_score) ==
         get_cardinality(autoplay_results->p2_score));
  assert(within_epsilon(get_mean(autoplay_results->p1_score),
                        get_mean(autoplay_results->p2_score)));
  assert(within_epsilon(get_stdev(autoplay_results->p1_score),
                        get_stdev(autoplay_results->p2_score)));
  csw_config->number_of_games_or_pairs = original_number_of_game_pairs;
  csw_config->number_of_threads = original_number_of_threads;
  csw_config->use_game_pairs = original_use_game_pairs;

  destroy_thread_control(thread_control);
  destroy_autoplay_results(autoplay_results);
}

void test_autoplay(TestConfig *testconfig) {
  autoplay_game_pairs_test(testconfig);
}