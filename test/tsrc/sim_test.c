#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/def/config_defs.h"
#include "../../src/def/simmer_defs.h"
#include "../../src/def/stats_defs.h"
#include "../../src/def/thread_control_defs.h"

#include "../../src/ent/bag.h"
#include "../../src/ent/board.h"
#include "../../src/ent/config.h"
#include "../../src/ent/game.h"
#include "../../src/ent/letter_distribution.h"
#include "../../src/ent/move.h"
#include "../../src/ent/player.h"
#include "../../src/ent/rack.h"
#include "../../src/ent/sim_results.h"
#include "../../src/ent/stats.h"
#include "../../src/ent/thread_control.h"
#include "../../src/ent/win_pct.h"

#include "../../src/impl/simmer.h"

#include "../../src/str/move_string.h"

#include "../../src/util/string_util.h"

#include "test_util.h"

void print_sim_stats(Game *game, SimResults *sim_results) {
  sim_results_sort_plays_by_win_rate(sim_results);
  const LetterDistribution *ld = game_get_ld(game);
  printf("%-20s%-9s%-16s%-16s\n", "Play", "Score", "Win%", "Equity");
  StringBuilder *move_description = string_builder_create();
  for (int i = 0; i < sim_results_get_number_of_plays(sim_results); i++) {
    const SimmedPlay *play = sim_results_get_simmed_play(sim_results, i);
    Stat *win_pct_stat = simmed_play_get_win_pct_stat(play);
    double wp_mean = stat_get_mean(win_pct_stat) * 100.0;
    double wp_se = stat_get_stderr(win_pct_stat, STATS_Z99) * 100.0;

    Stat *equity_stat = simmed_play_get_equity_stat(play);
    double eq_mean = stat_get_mean(equity_stat);
    double eq_se = stat_get_stderr(equity_stat, STATS_Z99);

    char *wp = get_formatted_string("%.3f±%.3f", wp_mean, wp_se);
    char *eq = get_formatted_string("%.3f±%.3f", eq_mean, eq_se);

    const char *ignore = simmed_play_get_ignore(play) ? "❌" : "";
    Move *move = simmed_play_get_move(play);
    string_builder_add_move_description(move, ld, move_description);
    printf("%-20s%-9d%-16s%-16s%s\n", string_builder_peek(move_description),
           move_get_score(move), wp, eq, ignore);
    string_builder_clear(move_description);
    free(wp);
    free(eq);
  }
  printf("Iterations: %d\n", sim_results_get_iteration_count(sim_results));
  string_builder_destroy(move_description);
}

void test_win_pct() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  assert(within_epsilon(win_pct_get(config_get_win_pcts(config), 118, 90),
                        0.844430));
  config_destroy(config);
}

void test_sim_single_iteration() {
  Config *config = create_config_or_die(
      "setoptions lex NWL20 s1 score s2 score r1 all r2 all numplays 1");
  Game *game = game_create(config);
  Board *board = game_get_board(game);
  Bag *bag = game_get_bag(game);
  const LetterDistribution *ld = game_get_ld(game);
  Player *player0 = game_get_player(game, 0);
  Rack *player0_rack = player_get_rack(player0);
  ThreadControl *thread_control = config_get_thread_control(config);

  draw_rack_to_string(ld, bag, player0_rack, "AAADERW", 0);
  SimResults *sim_results = sim_results_create();
  load_config_or_die(config, "setoptions rack " EMPTY_RACK_STRING
                             " plies 2 threads 1 numplays 15 i 1 cond none");
  sim_status_t status = simulate(config, game, NULL, sim_results);
  assert(status == SIM_STATUS_SUCCESS);
  assert(thread_control_get_halt_status(thread_control) ==
         HALT_STATUS_MAX_ITERATIONS);

  assert(board_get_tiles_played(board) == 0);

  game_destroy(game);
  config_destroy(config);
  sim_results_destroy(sim_results);
}

void test_more_iterations() {
  Config *config = create_config_or_die(
      "setoptions lex NWL20 s1 score s2 score r1 all r2 all numplays 1");
  Game *game = game_create(config);
  Bag *bag = game_get_bag(game);
  const LetterDistribution *ld = game_get_ld(game);
  Player *player0 = game_get_player(game, 0);
  Rack *player0_rack = player_get_rack(player0);
  ThreadControl *thread_control = config_get_thread_control(config);

  draw_rack_to_string(ld, bag, player0_rack, "AEIQRST", 0);
  SimResults *sim_results = sim_results_create();
  load_config_or_die(config, "setoptions rack " EMPTY_RACK_STRING
                             " plies 2 threads 1 numplays 15 i 400 cond none");
  sim_status_t status = simulate(config, game, NULL, sim_results);
  assert(status == SIM_STATUS_SUCCESS);
  assert(thread_control_get_halt_status(thread_control) ==
         HALT_STATUS_MAX_ITERATIONS);
  sim_results_sort_plays_by_win_rate(sim_results);

  SimmedPlay *play = sim_results_get_simmed_play(sim_results, 0);
  StringBuilder *move_string_builder = string_builder_create();
  string_builder_add_move_description(simmed_play_get_move(play), ld,
                                      move_string_builder);

  assert(strings_equal(string_builder_peek(move_string_builder), "8G QI"));

  game_destroy(game);
  config_destroy(config);
  sim_results_destroy(sim_results);
  string_builder_destroy(move_string_builder);
}

void perf_test_multithread_sim() {
  Config *config = create_config_or_die(
      "setoptions s1 score s2 score r1 all r2 all numplays 1000000 "
      "threads 4 "
      "cgp "
      "C14/O2TOY9/mIRADOR8/F4DAB2PUGH1/I5GOOEY3V/T4XI2MALTHA/14N/6GUM3OWN/"
      "7PEW2DOE/9EF1DOR/2KUNA1J1BEVELS/3TURRETs2S2/7A4T2/7N7/7S7 EEEIILZ/ "
      "336/298 0 lex NWL20;");

  Game *game = game_create(config);

  const LetterDistribution *ld = game_get_ld(game);
  ThreadControl *thread_control = config_get_thread_control(config);

  int num_threads = thread_control_get_threads(thread_control);
  printf("Using %d threads\n", num_threads);
  game_load_cgp(game, config_get_cgp(config));
  SimResults *sim_results = sim_results_create();
  load_config_or_die(config, "setoptions rack " EMPTY_RACK_STRING
                             " plies 2 threads 1 numplays 15 i 1000 cond none");
  sim_status_t status = simulate(config, game, NULL, sim_results);
  assert(status == SIM_STATUS_SUCCESS);
  assert(thread_control_get_halt_status(thread_control) ==
         HALT_STATUS_MAX_ITERATIONS);

  print_sim_stats(game, sim_results);
  sim_results_sort_plays_by_win_rate(sim_results);

  SimmedPlay *play = sim_results_get_simmed_play(sim_results, 0);
  StringBuilder *move_string_builder = string_builder_create();
  string_builder_add_move_description(simmed_play_get_move(play), ld,
                                      move_string_builder);

  assert(strings_equal(string_builder_peek(move_string_builder), "14F ZI.E"));

  string_builder_destroy(move_string_builder);
  game_destroy(game);
  config_destroy(config);
  sim_results_destroy(sim_results);
}

void test_play_similarity() {
  Config *config = create_config_or_die(
      "setoptions lex NWL20 s1 score s2 score r1 all r2 all numplays 1");
  Game *game = game_create(config);
  Bag *bag = game_get_bag(game);
  const LetterDistribution *ld = game_get_ld(game);
  ThreadControl *thread_control = config_get_thread_control(config);

  Player *player0 = game_get_player(game, 0);

  Rack *player0_rack = player_get_rack(player0);

  draw_rack_to_string(ld, bag, player0_rack, "ACEIRST", 0);
  SimResults *sim_results = sim_results_create();
  load_config_or_die(config,
                     "setoptions rack " EMPTY_RACK_STRING
                     " plies 2 threads 1 numplays 4 i 1200 cond none check 50");
  sim_status_t status = simulate(config, game, NULL, sim_results);
  assert(status == SIM_STATUS_SUCCESS);
  assert(thread_control_get_halt_status(thread_control) ==
         HALT_STATUS_MAX_ITERATIONS);

  // The first four plays all score 74. Only
  // 8F ATRESIC and 8F STEARIC should show up as similar, though.
  // Only one of these plays should be marked as ignored and all
  // others should not be ignored, since the stopping condition
  // is NONE.

  StringBuilder *p1_string_builder = string_builder_create();
  bool found_ignored_play = false;
  for (int i = 0; i < 4; i++) {
    SimmedPlay *play_i = sim_results_get_simmed_play(sim_results, i);
    Move *move_i = simmed_play_get_move(play_i);
    string_builder_clear(p1_string_builder);
    string_builder_add_move_description(move_i, ld, p1_string_builder);

    const char *p1 = string_builder_peek(p1_string_builder);
    if (simmed_play_get_ignore(play_i)) {
      assert(strings_equal(p1, "8F ATRESIC") ||
             strings_equal(p1, "8F STEARIC"));
      assert(!found_ignored_play);
      found_ignored_play = true;
    }
  }
  string_builder_destroy(p1_string_builder);
  game_destroy(game);
  config_destroy(config);
  sim_results_destroy(sim_results);
}

void test_sim() {
  test_win_pct();
  test_sim_single_iteration();
  test_more_iterations();
  test_play_similarity();
  perf_test_multithread_sim();
  // FIXME: test with validated moves
}
