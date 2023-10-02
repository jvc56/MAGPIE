#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h> // for sleep

#include "../src/move.h"
#include "../src/sim.h"
#include "../src/winpct.h"

#include "superconfig.h"
#include "test_util.h"

void print_sim_stats(Simmer *simmer, Game *game) {
  pthread_mutex_lock(&simmer->simmed_plays_mutex);
  sort_plays_by_win_rate(simmer->simmed_plays, simmer->num_simmed_plays);
  pthread_mutex_unlock(&simmer->simmed_plays_mutex);

  printf("%-20s%-9s%-16s%-16s\n", "Play", "Score", "Win%", "Equity");
  StringBuilder *move_description = create_string_builder();
  for (int i = 0; i < simmer->num_simmed_plays; i++) {
    SimmedPlay *play = simmer->simmed_plays[i];
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

void test_win_pct(SuperConfig *superconfig) {
  Config *config = get_csw_config(superconfig);
  assert(within_epsilon(win_pct(config->win_pcts, 118, 90), 0.844430));
}

void test_sim_single_iteration(SuperConfig *superconfig,
                               ThreadControl *thread_control) {
  Config *config = get_nwl_config(superconfig);
  Game *game = create_game(config);
  draw_rack_to_string(game->gen->bag, game->players[0]->rack, "AAADERW",
                      game->gen->letter_distribution);
  Simmer *simmer = create_simmer(config);
  assert(thread_control->halt_status == HALT_STATUS_NONE);
  simulate(thread_control, simmer, game, NULL, 2, 1, 15, 1,
           SIM_STOPPING_CONDITION_NONE, 0);
  assert(thread_control->halt_status == HALT_STATUS_MAX_ITERATIONS);

  assert(game->gen->board->tiles_played == 0);

  assert(unhalt(thread_control));
  destroy_game(game);
  destroy_simmer(simmer);
}

void test_more_iterations(SuperConfig *superconfig,
                          ThreadControl *thread_control) {
  Config *config = get_nwl_config(superconfig);
  Game *game = create_game(config);
  draw_rack_to_string(game->gen->bag, game->players[0]->rack, "AEIQRST",
                      game->gen->letter_distribution);
  Simmer *simmer = create_simmer(config);
  assert(thread_control->halt_status == HALT_STATUS_NONE);
  simulate(thread_control, simmer, game, NULL, 2, 1, 15, 400,
           SIM_STOPPING_CONDITION_NONE, 0);
  assert(thread_control->halt_status == HALT_STATUS_MAX_ITERATIONS);
  sort_plays_by_win_rate(simmer->simmed_plays, simmer->num_simmed_plays);

  StringBuilder *move_string_builder = create_string_builder();
  string_builder_add_move_description(simmer->simmed_plays[0]->move,
                                      game->gen->letter_distribution,
                                      move_string_builder);

  assert(strcmp(string_builder_peek(move_string_builder), "8G QI") == 0);

  assert(unhalt(thread_control));
  destroy_game(game);
  destroy_simmer(simmer);
  destroy_string_builder(move_string_builder);
}

void perf_test_sim(Config *config, ThreadControl *thread_control) {
  Game *game = create_game(config);

  load_cgp(game, config->cgp);
  Simmer *simmer = create_simmer(config);

  int iters = 10000;
  assert(thread_control->halt_status == HALT_STATUS_NONE);
  clock_t begin = clock();
  simulate(thread_control, simmer, game, NULL, 2, 1, 15, iters,
           SIM_STOPPING_CONDITION_NONE, 0);
  clock_t end = clock();
  assert(thread_control->halt_status == HALT_STATUS_MAX_ITERATIONS);
  printf("%d iters took %0.6f seconds\n", iters,
         (double)(end - begin) / CLOCKS_PER_SEC);
  print_sim_stats(simmer, game);
  sort_plays_by_win_rate(simmer->simmed_plays, simmer->num_simmed_plays);

  StringBuilder *move_string_builder = create_string_builder();
  string_builder_add_move_description(simmer->simmed_plays[0]->move,
                                      game->gen->letter_distribution,
                                      move_string_builder);

  assert(strcmp(string_builder_peek(move_string_builder), "14F ZI.E") == 0);

  assert(unhalt(thread_control));
  destroy_string_builder(move_string_builder);
  destroy_game(game);
  destroy_simmer(simmer);
}

void perf_test_multithread_sim(Config *config, ThreadControl *thread_control) {
  Game *game = create_game(config);
  int num_threads = config->number_of_threads;
  printf("Using %d threads\n", num_threads);
  load_cgp(game, config->cgp);
  Simmer *simmer = create_simmer(config);
  assert(thread_control->halt_status == HALT_STATUS_NONE);
  simulate(thread_control, simmer, game, NULL, 2, 1, 15, 1000,
           SIM_STOPPING_CONDITION_NONE, 0);
  assert(thread_control->halt_status == HALT_STATUS_MAX_ITERATIONS);

  print_sim_stats(simmer, game);
  sort_plays_by_win_rate(simmer->simmed_plays, simmer->num_simmed_plays);

  StringBuilder *move_string_builder = create_string_builder();
  string_builder_add_move_description(simmer->simmed_plays[0]->move,
                                      game->gen->letter_distribution,
                                      move_string_builder);

  assert(strcmp(string_builder_peek(move_string_builder), "14F ZI.E") == 0);

  assert(unhalt(thread_control));
  destroy_string_builder(move_string_builder);
  destroy_game(game);
  destroy_simmer(simmer);
}

void perf_test_multithread_blocking_sim(Config *config,
                                        ThreadControl *thread_control) {
  Game *game = create_game(config);
  int num_threads = config->number_of_threads;
  printf("Using %d threads\n", num_threads);
  load_cgp(game, config->cgp);

  Simmer *simmer = create_simmer(config);
  simulate(thread_control, simmer, game, NULL, 2, 1, 15, 1000000,
           SIM_STOPPING_CONDITION_99PCT, 0);

  print_sim_stats(simmer, game);
  sort_plays_by_win_rate(simmer->simmed_plays, simmer->num_simmed_plays);

  StringBuilder *move_string_builder = create_string_builder();
  string_builder_add_move_description(simmer->simmed_plays[0]->move,
                                      game->gen->letter_distribution,
                                      move_string_builder);

  assert(strcmp(string_builder_peek(move_string_builder), "14F ZI.E") == 0);
  destroy_string_builder(move_string_builder);
  destroy_game(game);
  destroy_simmer(simmer);
}

void test_play_similarity(SuperConfig *superconfig,
                          ThreadControl *thread_control) {
  Config *config = superconfig->nwl_config;
  Game *game = create_game(config);
  draw_rack_to_string(game->gen->bag, game->players[0]->rack, "ACEIRST",
                      game->gen->letter_distribution);
  Simmer *simmer = create_simmer(config);
  assert(thread_control->halt_status == HALT_STATUS_NONE);
  simulate(thread_control, simmer, game, NULL, 2, 1, 15, 0,
           SIM_STOPPING_CONDITION_NONE, 0);
  assert(thread_control->halt_status == HALT_STATUS_MAX_ITERATIONS);
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
      if (strcmp(p1, "8F ATRESIC") == 0 && strcmp(p2, "8F STEARIC") == 0) {
        assert(plays_are_similar(simmer, simmer->simmed_plays[i],
                                 simmer->simmed_plays[j]));
      } else if (strcmp(p2, "8F ATRESIC") == 0 &&
                 strcmp(p1, "8F STEARIC") == 0) {
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
  assert(unhalt(thread_control));
  destroy_game(game);
  destroy_simmer(simmer);
}

void test_sim(SuperConfig *superconfig) {
  ThreadControl *thread_control = create_thread_control(NULL);
  test_win_pct(superconfig);
  test_sim_single_iteration(superconfig, thread_control);
  test_more_iterations(superconfig, thread_control);
  test_play_similarity(superconfig, thread_control);
  // And run a perf test.
  int threads = superconfig->nwl_config->number_of_threads;
  char *backup_cgp = superconfig->nwl_config->cgp;
  superconfig->nwl_config->number_of_threads = 4;
  superconfig->nwl_config->cgp =
      "C14/O2TOY9/mIRADOR8/F4DAB2PUGH1/I5GOOEY3V/T4XI2MALTHA/14N/6GUM3OWN/"
      "7PEW2DOE/9EF1DOR/2KUNA1J1BEVELS/3TURRETs2S2/7A4T2/7N7/7S7 EEEIILZ/ "
      "336/298 0 lex NWL20;";
  perf_test_multithread_sim(superconfig->nwl_config, thread_control);
  // restore superconfig
  superconfig->nwl_config->cgp = backup_cgp;
  superconfig->nwl_config->number_of_threads = threads;
  destroy_thread_control(thread_control);
}
