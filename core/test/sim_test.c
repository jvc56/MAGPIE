#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "../src/def/stats_defs.h"

#include "../src/str/move_string.h"
#include "../src/util/string_util.h"

#include "../src/ent/move.h"
#include "../src/ent/simmer.h"
#include "../src/ent/stats.h"
#include "../src/ent/win_pct.h"

#include "test_util.h"
#include "testconfig.h"

void print_sim_stats(Game *game, Simmer *simmer) {
  sort_plays_by_win_rate(simmer);
  Generator *gen = game_get_gen(game);
  LetterDistribution *ld = gen_get_ld(gen);
  printf("%-20s%-9s%-16s%-16s\n", "Play", "Score", "Win%", "Equity");
  StringBuilder *move_description = create_string_builder();
  for (int i = 0; i < simmer_get_number_of_plays(simmer); i++) {
    const SimmedPlay *play = simmer_get_simmed_play(simmer, i);
    Stat *win_pct_stat = simmed_play_get_win_pct_stat(play);
    double wp_mean = get_mean(win_pct_stat) * 100.0;
    double wp_se = get_standard_error(win_pct_stat, STATS_Z99) * 100.0;

    Stat *equity_stat = simmed_play_get_equity_stat(play);
    double eq_mean = get_mean(equity_stat);
    double eq_se = get_standard_error(equity_stat, STATS_Z99);

    char *wp = get_formatted_string("%.3f±%.3f", wp_mean, wp_se);
    char *eq = get_formatted_string("%.3f±%.3f", eq_mean, eq_se);

    const char *ignore = simmed_play_get_ignore(play) ? "❌" : "";
    Move *move = simmed_play_get_move(play);
    string_builder_add_move_description(move, ld, move_description);
    printf("%-20s%-9d%-16s%-16s%s\n", string_builder_peek(move_description),
           get_score(move), wp, eq, ignore);
    string_builder_clear(move_description);
    free(wp);
    free(eq);
  }
  printf("Iterations: %d\n", simmer_get_iteration_count(simmer));
  destroy_string_builder(move_description);
}

void test_win_pct(TestConfig *testconfig) {
  const Config *config = get_csw_config(testconfig);
  assert(
      within_epsilon(win_pct(config_get_win_pcts(config), 118, 90), 0.844430));
}

void test_sim_single_iteration(TestConfig *testconfig) {
  Config *config = get_nwl_config(testconfig);
  Game *game = create_game(config);
  Generator *gen = game_get_gen(game);
  LetterDistribution *ld = gen_get_ld(gen);
  Bag *bag = gen_get_bag(gen);
  Board *board = gen_get_board(gen);
  Player *player0 = game_get_player(game, 0);
  Rack *player0_rack = player_get_rack(player0);
  ThreadControl *thread_control = config_get_thread_control(config);

  draw_rack_to_string(ld, bag, player0_rack, "AAADERW", 0);
  Simmer *simmer = create_simmer(config);
  load_config_or_die(config, "setoptions rack " EMPTY_RACK_STRING
                             " plies 2 threads 1 numplays 15 i 1 cond none");
  sim_status_t status = simulate(config, game, simmer);
  assert(status == SIM_STATUS_SUCCESS);
  assert(get_halt_status(thread_control) == HALT_STATUS_MAX_ITERATIONS);

  assert(get_tiles_played(board) == 0);

  destroy_game(game);
  destroy_simmer(simmer);
}

void test_more_iterations(TestConfig *testconfig) {
  Config *config = get_nwl_config(testconfig);
  Game *game = create_game(config);
  Generator *gen = game_get_gen(game);
  LetterDistribution *ld = gen_get_ld(gen);
  Bag *bag = gen_get_bag(gen);
  Player *player0 = game_get_player(game, 0);
  Rack *player0_rack = player_get_rack(player0);
  ThreadControl *thread_control = config_get_thread_control(config);

  draw_rack_to_string(ld, bag, player0_rack, "AEIQRST", 0);
  Simmer *simmer = create_simmer(config);
  load_config_or_die(config, "setoptions rack " EMPTY_RACK_STRING
                             " plies 2 threads 1 numplays 15 i 400 cond none");
  sim_status_t status = simulate(config, game, simmer);
  assert(status == SIM_STATUS_SUCCESS);
  assert(get_halt_status(thread_control) == HALT_STATUS_MAX_ITERATIONS);
  sort_plays_by_win_rate(simmer);

  SimmedPlay *play = simmer_get_simmed_play(simmer, 0);
  StringBuilder *move_string_builder = create_string_builder();
  string_builder_add_move_description(simmed_play_get_move(play), ld,
                                      move_string_builder);

  assert(strings_equal(string_builder_peek(move_string_builder), "8G QI"));

  destroy_game(game);
  destroy_simmer(simmer);
  destroy_string_builder(move_string_builder);
}

void perf_test_sim(Config *config, ThreadControl *thread_control) {
  Game *game = create_game(config);
  Generator *gen = game_get_gen(game);
  LetterDistribution *ld = gen_get_ld(gen);

  load_cgp(game, config_get_cgp(config));
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
  assert(get_halt_status(thread_control) == HALT_STATUS_MAX_ITERATIONS);
  printf("%d iters took %0.6f seconds\n", iters,
         (double)(end - begin) / CLOCKS_PER_SEC);
  print_sim_stats(game, simmer);
  sort_plays_by_win_rate(simmer);

  SimmedPlay *play = simmer_get_simmed_play(simmer, 0);
  StringBuilder *move_string_builder = create_string_builder();
  string_builder_add_move_description(simmed_play_get_move(play), ld,
                                      move_string_builder);

  assert(strings_equal(string_builder_peek(move_string_builder), "14F ZI.E"));

  destroy_string_builder(move_string_builder);
  destroy_game(game);
  destroy_simmer(simmer);
}

void perf_test_multithread_sim(Config *config) {
  Game *game = create_game(config);
  Generator *gen = game_get_gen(game);
  LetterDistribution *ld = gen_get_ld(gen);
  ThreadControl *thread_control = config_get_thread_control(config);

  int num_threads = get_number_of_threads(thread_control);
  printf("Using %d threads\n", num_threads);
  load_cgp(game, config_get_cgp(config));
  Simmer *simmer = create_simmer(config);
  load_config_or_die(config, "setoptions rack " EMPTY_RACK_STRING
                             " plies 2 threads 1 numplays 15 i 1000 cond none");
  sim_status_t status = simulate(config, game, simmer);
  assert(status == SIM_STATUS_SUCCESS);
  assert(get_halt_status(thread_control) == HALT_STATUS_MAX_ITERATIONS);

  print_sim_stats(game, simmer);
  sort_plays_by_win_rate(simmer);

  SimmedPlay *play = simmer_get_simmed_play(simmer, 0);
  StringBuilder *move_string_builder = create_string_builder();
  string_builder_add_move_description(simmed_play_get_move(play), ld,
                                      move_string_builder);

  assert(strings_equal(string_builder_peek(move_string_builder), "14F ZI.E"));

  destroy_string_builder(move_string_builder);
  destroy_game(game);
  destroy_simmer(simmer);
}

void perf_test_multithread_blocking_sim(Config *config) {
  Game *game = create_game(config);
  ThreadControl *thread_control = config_get_thread_control(config);
  Generator *gen = game_get_gen(game);
  LetterDistribution *ld = gen_get_ld(gen);

  int num_threads = get_number_of_threads(thread_control);
  printf("Using %d threads\n", num_threads);
  load_cgp(game, config_get_cgp(config));

  Simmer *simmer = create_simmer(config);
  load_config_or_die(config,
                     "setoptions rack " EMPTY_RACK_STRING
                     " plies 2 threads 1 numplays 15 i 1000000 cond 99");
  sim_status_t status = simulate(config, game, simmer);
  assert(status == SIM_STATUS_SUCCESS);
  print_sim_stats(game, simmer);
  sort_plays_by_win_rate(simmer);

  SimmedPlay *play = simmer_get_simmed_play(simmer, 0);
  StringBuilder *move_string_builder = create_string_builder();
  string_builder_add_move_description(simmed_play_get_move(play), ld,
                                      move_string_builder);

  assert(strings_equal(string_builder_peek(move_string_builder), "14F ZI.E"));
  destroy_string_builder(move_string_builder);
  destroy_game(game);
  destroy_simmer(simmer);
}

void test_play_similarity(TestConfig *testconfig) {
  Config *config = testconfig->nwl_config;
  Game *game = create_game(config);
  Generator *gen = game_get_gen(game);
  LetterDistribution *ld = gen_get_ld(gen);
  ThreadControl *thread_control = config_get_thread_control(config);

  Bag *bag = gen_get_bag(gen);
  Player *player0 = game_get_player(game, 0);

  Rack *player0_rack = player_get_rack(player0);

  draw_rack_to_string(ld, bag, player0_rack, "ACEIRST", 0);
  Simmer *simmer = create_simmer(config);
  load_config_or_die(config, "setoptions rack " EMPTY_RACK_STRING
                             " plies 2 threads 1 numplays 15 i 0 cond none");
  sim_status_t status = simulate(config, game, simmer);
  assert(status == SIM_STATUS_SUCCESS);
  assert(get_halt_status(thread_control) == HALT_STATUS_MAX_ITERATIONS);
  // The first four plays all score 74. Only
  // 8F ATRESIC and 8F STEARIC should show up as similar, though.
  // These are play indexes 1 and 2.

  StringBuilder *p1_string_builder = create_string_builder();
  StringBuilder *p2_string_builder = create_string_builder();

  for (int i = 0; i < 4; i++) {
    for (int j = i + 1; j < 4; j++) {
      SimmedPlay *play_i = simmer_get_simmed_play(simmer, i);
      SimmedPlay *play_j = simmer_get_simmed_play(simmer, j);
      Move *move_i = simmed_play_get_move(play_i);
      Move *move_j = simmed_play_get_move(play_j);
      string_builder_clear(p1_string_builder);
      string_builder_add_move_description(move_i, ld, p1_string_builder);
      string_builder_clear(p2_string_builder);
      string_builder_add_move_description(move_j, ld, p2_string_builder);

      const char *p1 = string_builder_peek(p1_string_builder);
      const char *p2 = string_builder_peek(p2_string_builder);
      if (strings_equal(p1, "8F ATRESIC") && strings_equal(p2, "8F STEARIC")) {
        assert(plays_are_similar(play_i, play_j, simmer));
      } else if (strings_equal(p2, "8F ATRESIC") &&
                 strings_equal(p1, "8F STEARIC")) {
        assert(plays_are_similar(play_i, play_j, simmer));
      } else {
        assert(!plays_are_similar(play_i, play_j, simmer));
      }
    }
  }
  destroy_string_builder(p1_string_builder);
  destroy_string_builder(p2_string_builder);

  SimmedPlay *play_3 = simmer_get_simmed_play(simmer, 3);
  SimmedPlay *play_4 = simmer_get_simmed_play(simmer, 4);

  assert(!plays_are_similar(play_3, play_4, simmer));
  destroy_game(game);
  destroy_simmer(simmer);
}

void test_sim(TestConfig *testconfig) {
  test_win_pct(testconfig);
  test_sim_single_iteration(testconfig);
  test_more_iterations(testconfig);
  test_play_similarity(testconfig);
  Config *config = create_default_config();
  load_config_or_die(
      config,
      "setoptions lex NWL20 s1 score s2 score r1 all r2 all numplays 1000000 "
      "threads 4"
      "cgp "
      "C14/O2TOY9/mIRADOR8/F4DAB2PUGH1/I5GOOEY3V/T4XI2MALTHA/14N/6GUM3OWN/"
      "7PEW2DOE/9EF1DOR/2KUNA1J1BEVELS/3TURRETs2S2/7A4T2/7N7/7S7 EEEIILZ/ "
      "336/298 0 lex NWL20;");
  perf_test_multithread_sim(config);
  destroy_config(config);
}
