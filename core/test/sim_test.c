#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h> // for sleep

#include "../src/move.h"
#include "../src/sim.h"
#include "../src/winpct.h"

#include "test_util.h"
#include "testconfig.h"

void print_sim_stats(Simmer *simmer, const Game *game) {
  pthread_mutex_lock(&simmer->simmed_plays_mutex);
  sort_plays_by_win_rate(simmer->simmed_plays, simmer->num_simmed_plays);
  pthread_mutex_unlock(&simmer->simmed_plays_mutex);

  printf("%-20s%-9s%-16s%-16s\n", "Play", "Score", "Win%", "Equity");
  StringBuilder *move_description = create_string_builder();
  for (int i = 0; i < simmer->num_simmed_plays; i++) {
    const SimmedPlay *play = simmer->simmed_plays[i];
    double wp_mean = play->win_pct_stat->mean * 100.0;
    double wp_se = get_standard_error(play->win_pct_stat, STATS_Z99) * 100.0;

    double eq_mean = play->equity_stat->mean;
    double eq_se = get_standard_error(play->equity_stat, STATS_Z99);

    char *wp = get_formatted_string("%.3f±%.3f", wp_mean, wp_se);
    char *eq = get_formatted_string("%.3f±%.3f", eq_mean, eq_se);

    const char *ignore = play->ignore ? "❌" : "";
    string_builder_add_move_description(
        play->move, game->gen->letter_distribution, move_description);
    printf("%-20s%-9d%-16s%-16s%s\n", string_builder_peek(move_description),
           play->move->score, wp, eq, ignore);
    string_builder_clear(move_description);
    free(wp);
    free(eq);
  }
  printf("Iterations: %d\n", simmer->iteration_count);
  destroy_string_builder(move_description);
}

void test_win_pct(TestConfig *testconfig) {
  const Config *config = get_csw_config(testconfig);
  assert(within_epsilon(win_pct(config->win_pcts, 118, 90), 0.844430));
}

void test_sim_single_iteration(TestConfig *testconfig) {
  Config *config = get_nwl_config(testconfig);
  Game *game = create_game(config);
  draw_rack_to_string(game->gen->bag, game->players[0]->rack, "AAADERW",
                      game->gen->letter_distribution);
  Simmer *simmer = create_simmer(config);
  load_config_or_die(config, "setoptions rack " EMPTY_RACK_STRING
                             " plies 2 threads 1 numplays 15 i 1 cond none");
  sim_status_t status = simulate(config, game, simmer);
  assert(status == SIM_STATUS_SUCCESS);
  assert(config->thread_control->halt_status == HALT_STATUS_MAX_ITERATIONS);

  assert(game->gen->board->tiles_played == 0);

  destroy_game(game);
  destroy_simmer(simmer);
}

void test_more_iterations(TestConfig *testconfig) {
  Config *config = get_nwl_config(testconfig);
  Game *game = create_game(config);
  draw_rack_to_string(game->gen->bag, game->players[0]->rack, "AEIQRST",
                      game->gen->letter_distribution);
  Simmer *simmer = create_simmer(config);
  load_config_or_die(config, "setoptions rack " EMPTY_RACK_STRING
                             " plies 2 threads 1 numplays 15 i 400 cond none");
  sim_status_t status = simulate(config, game, simmer);
  assert(status == SIM_STATUS_SUCCESS);
  assert(config->thread_control->halt_status == HALT_STATUS_MAX_ITERATIONS);
  sort_plays_by_win_rate(simmer->simmed_plays, simmer->num_simmed_plays);

  StringBuilder *move_string_builder = create_string_builder();
  string_builder_add_move_description(simmer->simmed_plays[0]->move,
                                      game->gen->letter_distribution,
                                      move_string_builder);

  assert(strings_equal(string_builder_peek(move_string_builder), "8G QI"));

  destroy_game(game);
  destroy_simmer(simmer);
  destroy_string_builder(move_string_builder);
}

void perf_test_sim(Config *config, ThreadControl *thread_control) {
  Game *game = create_game(config);

  load_cgp(game, config->cgp);
  Simmer *simmer = create_simmer(config);

  int iters = 10000;
  char *setoptions_string = get_formatted_string(
      "setoptions rack %s plies 2 threads 1 numplays 15 i %d cond none",
      EMPTY_RACK_STRING, iters);
  load_config_or_die(config, setoptions_string);
  free(setoptions_string);
  clock_t begin = clock();
  sim_status_t status = simulate(config, game, simmer);
  clock_t end = clock();
  assert(status == SIM_STATUS_SUCCESS);
  assert(thread_control->halt_status == HALT_STATUS_MAX_ITERATIONS);
  printf("%d iters took %0.6f seconds\n", iters,
         (double)(end - begin) / CLOCKS_PER_SEC);
  print_sim_stats(simmer, game);
  sort_plays_by_win_rate(simmer->simmed_plays, simmer->num_simmed_plays);

  StringBuilder *move_string_builder = create_string_builder();
  string_builder_add_move_description(simmer->simmed_plays[0]->move,
                                      game->gen->letter_distribution,
                                      move_string_builder);

  assert(strings_equal(string_builder_peek(move_string_builder), "14F ZI.E"));

  destroy_string_builder(move_string_builder);
  destroy_game(game);
  destroy_simmer(simmer);
}

void perf_test_multithread_sim(Config *config) {
  Game *game = create_game(config);
  int num_threads = config->thread_control->number_of_threads;
  printf("Using %d threads\n", num_threads);
  load_cgp(game, config->cgp);
  Simmer *simmer = create_simmer(config);
  load_config_or_die(config, "setoptions rack " EMPTY_RACK_STRING
                             " plies 2 threads 1 numplays 15 i 1000 cond none");
  sim_status_t status = simulate(config, game, simmer);
  assert(status == SIM_STATUS_SUCCESS);
  assert(config->thread_control->halt_status == HALT_STATUS_MAX_ITERATIONS);

  print_sim_stats(simmer, game);
  sort_plays_by_win_rate(simmer->simmed_plays, simmer->num_simmed_plays);

  StringBuilder *move_string_builder = create_string_builder();
  string_builder_add_move_description(simmer->simmed_plays[0]->move,
                                      game->gen->letter_distribution,
                                      move_string_builder);

  assert(strings_equal(string_builder_peek(move_string_builder), "14F ZI.E"));

  destroy_string_builder(move_string_builder);
  destroy_game(game);
  destroy_simmer(simmer);
}

void perf_test_multithread_blocking_sim(Config *config) {
  Game *game = create_game(config);
  int num_threads = config->thread_control->number_of_threads;
  printf("Using %d threads\n", num_threads);
  load_cgp(game, config->cgp);

  Simmer *simmer = create_simmer(config);
  load_config_or_die(config,
                     "setoptions rack " EMPTY_RACK_STRING
                     " plies 2 threads 1 numplays 15 i 1000000 cond 99");
  sim_status_t status = simulate(config, game, simmer);
  assert(status == SIM_STATUS_SUCCESS);
  print_sim_stats(simmer, game);
  sort_plays_by_win_rate(simmer->simmed_plays, simmer->num_simmed_plays);

  StringBuilder *move_string_builder = create_string_builder();
  string_builder_add_move_description(simmer->simmed_plays[0]->move,
                                      game->gen->letter_distribution,
                                      move_string_builder);

  assert(strings_equal(string_builder_peek(move_string_builder), "14F ZI.E"));
  destroy_string_builder(move_string_builder);
  destroy_game(game);
  destroy_simmer(simmer);
}

void test_play_similarity(TestConfig *testconfig) {
  Config *config = testconfig->nwl_config;
  Game *game = create_game(config);
  draw_rack_to_string(game->gen->bag, game->players[0]->rack, "ACEIRST",
                      game->gen->letter_distribution);
  Simmer *simmer = create_simmer(config);
  load_config_or_die(config, "setoptions rack " EMPTY_RACK_STRING
                             " plies 2 threads 1 numplays 15 i 0 cond none");
  sim_status_t status = simulate(config, game, simmer);
  assert(status == SIM_STATUS_SUCCESS);
  assert(config->thread_control->halt_status == HALT_STATUS_MAX_ITERATIONS);
  // The first four plays all score 74. Only
  // 8F ATRESIC and 8F STEARIC should show up as similar, though.
  // These are play indexes 1 and 2.

  StringBuilder *p1_string_builder = create_string_builder();
  StringBuilder *p2_string_builder = create_string_builder();

  for (int i = 0; i < 4; i++) {
    for (int j = i + 1; j < 4; j++) {
      string_builder_clear(p1_string_builder);
      string_builder_add_move_description(simmer->simmed_plays[i]->move,
                                          game->gen->letter_distribution,
                                          p1_string_builder);
      string_builder_clear(p2_string_builder);
      string_builder_add_move_description(simmer->simmed_plays[j]->move,
                                          game->gen->letter_distribution,
                                          p2_string_builder);

      const char *p1 = string_builder_peek(p1_string_builder);
      const char *p2 = string_builder_peek(p2_string_builder);
      if (strings_equal(p1, "8F ATRESIC") && strings_equal(p2, "8F STEARIC")) {
        assert(plays_are_similar(simmer, simmer->simmed_plays[i],
                                 simmer->simmed_plays[j]));
      } else if (strings_equal(p2, "8F ATRESIC") &&
                 strings_equal(p1, "8F STEARIC")) {
        assert(plays_are_similar(simmer, simmer->simmed_plays[i],
                                 simmer->simmed_plays[j]));
      } else {
        assert(!plays_are_similar(simmer, simmer->simmed_plays[i],
                                  simmer->simmed_plays[j]));
      }
    }
  }
  destroy_string_builder(p1_string_builder);
  destroy_string_builder(p2_string_builder);

  assert(!plays_are_similar(simmer, simmer->simmed_plays[3],
                            simmer->simmed_plays[4]));
  destroy_game(game);
  destroy_simmer(simmer);
}

void test_sim(TestConfig *testconfig) {
  test_win_pct(testconfig);
  test_sim_single_iteration(testconfig);
  test_more_iterations(testconfig);
  test_play_similarity(testconfig);
  // And run a perf test.
  int threads = testconfig->nwl_config->thread_control->number_of_threads;
  char *backup_cgp = testconfig->nwl_config->cgp;
  testconfig->nwl_config->thread_control->number_of_threads = 4;
  testconfig->nwl_config->cgp =
      "C14/O2TOY9/mIRADOR8/F4DAB2PUGH1/I5GOOEY3V/T4XI2MALTHA/14N/6GUM3OWN/"
      "7PEW2DOE/9EF1DOR/2KUNA1J1BEVELS/3TURRETs2S2/7A4T2/7N7/7S7 EEEIILZ/ "
      "336/298 0 lex NWL20;";
  perf_test_multithread_sim(testconfig->nwl_config);
  // restore testconfig
  testconfig->nwl_config->cgp = backup_cgp;
  testconfig->nwl_config->thread_control->number_of_threads = threads;
}
