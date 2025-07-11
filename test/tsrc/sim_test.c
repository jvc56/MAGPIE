#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/def/config_defs.h"
#include "../../src/def/thread_control_defs.h"

#include "../../src/ent/bag.h"
#include "../../src/ent/board.h"
#include "../../src/ent/game.h"
#include "../../src/ent/letter_distribution.h"
#include "../../src/ent/move.h"
#include "../../src/ent/player.h"
#include "../../src/ent/rack.h"
#include "../../src/ent/sim_results.h"
#include "../../src/ent/stats.h"
#include "../../src/ent/thread_control.h"
#include "../../src/ent/win_pct.h"
#include "../../src/impl/config.h"

#include "../../src/impl/cgp.h"
#include "../../src/impl/gameplay.h"
#include "../../src/impl/move_gen.h"
#include "../../src/impl/simmer.h"

#include "../../src/str/game_string.h"
#include "../../src/str/move_string.h"
#include "../../src/str/sim_string.h"

#include "../../src/util/math_util.h"
#include "../../src/util/string_util.h"

#include "test_constants.h"
#include "test_util.h"

void print_sim_stats(Game *game, SimResults *sim_results) {
  sim_results_sort_plays_by_win_rate(sim_results);
  const LetterDistribution *ld = game_get_ld(game);
  printf("%-20s%-9s%-16s%-16s\n", "Play", "Score", "Win%", "Equity");
  StringBuilder *move_description = string_builder_create();
  for (int i = 0; i < sim_results_get_number_of_plays(sim_results); i++) {
    const SimmedPlay *play = sim_results_get_sorted_simmed_play(sim_results, i);
    Stat *win_pct_stat = simmed_play_get_win_pct_stat(play);
    double wp_mean = stat_get_mean(win_pct_stat) * 100.0;

    Stat *equity_stat = simmed_play_get_equity_stat(play);
    double eq_mean = stat_get_mean(equity_stat);

    char *wp_str = NULL;
    char *eq_str = NULL;

    double wp_stdev = stat_get_stdev(win_pct_stat) * 100.0;
    double eq_stdev = stat_get_stdev(equity_stat);
    wp_str = get_formatted_string("%.3f %.3f", wp_mean, wp_stdev);
    eq_str = get_formatted_string("%.3f %.3f", eq_mean, eq_stdev);

    const char *is_epigon = simmed_play_get_is_epigon(play) ? "❌" : "";
    Move *move = simmed_play_get_move(play);
    string_builder_add_move_description(move_description, move, ld);
    printf("%-20s%-9d%-16s%-16s%s\n", string_builder_peek(move_description),
           move_get_score(move), wp_str, eq_str, is_epigon);
    string_builder_clear(move_description);
    free(wp_str);
    free(eq_str);
  }
  printf("Iterations: %d\n", sim_results_get_iteration_count(sim_results));
  string_builder_destroy(move_description);
}

void test_sim_error_cases(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 15 -plies "
      "2 -threads 1 -iter 1 -scond none");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack 1 AAADERW");
  error_code_t status = config_simulate_and_return_status(
      config, NULL, config_get_sim_results(config));
  assert(status == ERROR_STATUS_SIM_NO_MOVES);
  config_destroy(config);
}

void test_sim_single_iteration(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 15 -plies "
      "2 -threads 1 -iter 1 -scond none");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack 1 AAADERW");
  load_and_exec_config_or_die(config, "gen");
  error_code_t status = config_simulate_and_return_status(
      config, NULL, config_get_sim_results(config));
  assert(status == ERROR_STATUS_SUCCESS);
  assert(thread_control_get_exit_status(config_get_thread_control(config)) ==
         EXIT_STATUS_SAMPLE_LIMIT);
  config_destroy(config);
}

void test_more_iterations(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 15 -plies "
      "2 -threads 1 -iter 500 -scond none -seed 10");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack 1 AEIQRST");
  load_and_exec_config_or_die(config, "gen");
  SimResults *sim_results = config_get_sim_results(config);
  error_code_t status =
      config_simulate_and_return_status(config, NULL, sim_results);
  assert(status == ERROR_STATUS_SUCCESS);
  assert(thread_control_get_exit_status(config_get_thread_control(config)) ==
         EXIT_STATUS_SAMPLE_LIMIT);
  sim_results_sort_plays_by_win_rate(sim_results);

  SimmedPlay *play = sim_results_get_sorted_simmed_play(sim_results, 0);
  StringBuilder *move_string_builder = string_builder_create();
  string_builder_add_move_description(
      move_string_builder, simmed_play_get_move(play), config_get_ld(config));

  assert(strings_equal(string_builder_peek(move_string_builder), "8G QI"));

  config_destroy(config);
  string_builder_destroy(move_string_builder);
}

typedef struct SimTestArgs {
  Config *config;
  SimResults *sim_results;
  error_code_t *status;
  pthread_mutex_t *mutex;
  pthread_cond_t *cond;
  int *done;
} SimTestArgs;

void *sim_thread_func(void *arg) {
  SimTestArgs *args = (SimTestArgs *)arg;
  *(args->status) =
      config_simulate_and_return_status(args->config, NULL, args->sim_results);

  pthread_mutex_lock(args->mutex);
  *(args->done) = 1;
  pthread_cond_signal(args->cond);
  pthread_mutex_unlock(args->mutex);

  return NULL;
}

void test_sim_threshold(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -plies 2 -threads 8 -iter 100000000 -scond 95");
  load_and_exec_config_or_die(config, "cgp " ZILLION_OPENING_CGP);
  load_and_exec_config_or_die(config, "addmoves 8F.LIN,8D.ZILLION,8F.ZILLION");

  SimResults *sim_results = config_get_sim_results(config);
  error_code_t status;
  int done = 0;

  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

  SimTestArgs args = {.config = config,
                      .sim_results = sim_results,
                      .status = &status,
                      .mutex = &mutex,
                      .cond = &cond,
                      .done = &done};

  pthread_t thread;
  pthread_create(&thread, NULL, sim_thread_func, &args);

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  const int timeout_seconds = 10;
  ts.tv_sec += timeout_seconds;

  pthread_mutex_lock(&mutex);
  while (!done) {
    int ret = pthread_cond_timedwait(&cond, &mutex, &ts);
    if (ret == ETIMEDOUT) {
      printf("sim did not complete within %d seconds.\n", timeout_seconds);
      pthread_cancel(thread);
      pthread_mutex_unlock(&mutex);
      assert(0);
    }
  }
  pthread_mutex_unlock(&mutex);

  pthread_join(thread, NULL);

  assert(status == ERROR_STATUS_SUCCESS);
  assert(thread_control_get_exit_status(config_get_thread_control(config)) ==
         EXIT_STATUS_THRESHOLD);

  sim_results_sort_plays_by_win_rate(sim_results);
  SimmedPlay *play = sim_results_get_sorted_simmed_play(sim_results, 0);
  StringBuilder *move_string_builder = string_builder_create();
  string_builder_add_move_description(
      move_string_builder, simmed_play_get_move(play), config_get_ld(config));

  assert(strings_equal(string_builder_peek(move_string_builder), "8D ZILLION"));

  config_destroy(config);
  string_builder_destroy(move_string_builder);
}

void test_sim_time_limit(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all "
      "-plies 2 -threads 1 -it 1000000000 -scond none -tlim 2");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack 1 ACEIRST");
  load_and_exec_config_or_die(config, "gen");

  SimResults *sim_results = config_get_sim_results(config);
  error_code_t status;
  int done = 0;

  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

  SimTestArgs args = {.config = config,
                      .sim_results = sim_results,
                      .status = &status,
                      .mutex = &mutex,
                      .cond = &cond,
                      .done = &done};

  pthread_t thread;
  pthread_create(&thread, NULL, sim_thread_func, &args);

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  const int timeout_seconds = 10;
  ts.tv_sec += timeout_seconds;

  pthread_mutex_lock(&mutex);
  while (!done) {
    int ret = pthread_cond_timedwait(&cond, &mutex, &ts);
    if (ret == ETIMEDOUT) {
      printf("sim did not complete within %d seconds.\n", timeout_seconds);
      pthread_cancel(thread);
      pthread_mutex_unlock(&mutex);
      assert(0);
    }
  }
  pthread_mutex_unlock(&mutex);

  pthread_join(thread, NULL);

  assert(status == ERROR_STATUS_SUCCESS);
  assert(thread_control_get_exit_status(config_get_thread_control(config)) ==
         EXIT_STATUS_TIMEOUT);
  config_destroy(config);
}

void test_sim_one_arm_remaining(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all "
      "-plies 2 -numplays 4 -threads 1 -it 1100 -scond none");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack 1 ACEIRST");
  load_and_exec_config_or_die(
      config, "addmoves 8D.CRISTAE,8D.ATRESIC,8D.STEARIC,8D.RACIEST");

  SimResults *sim_results = config_get_sim_results(config);
  error_code_t status;
  int done = 0;

  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

  SimTestArgs args = {.config = config,
                      .sim_results = sim_results,
                      .status = &status,
                      .mutex = &mutex,
                      .cond = &cond,
                      .done = &done};

  pthread_t thread;
  pthread_create(&thread, NULL, sim_thread_func, &args);

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  const int timeout_seconds = 10;
  ts.tv_sec += timeout_seconds;

  pthread_mutex_lock(&mutex);
  while (!done) {
    int ret = pthread_cond_timedwait(&cond, &mutex, &ts);
    if (ret == ETIMEDOUT) {
      printf("sim did not complete within %d seconds.\n", timeout_seconds);
      pthread_cancel(thread);
      pthread_mutex_unlock(&mutex);
      assert(0);
    }
  }
  pthread_mutex_unlock(&mutex);

  pthread_join(thread, NULL);

  assert(status == ERROR_STATUS_SUCCESS);
  assert(thread_control_get_exit_status(config_get_thread_control(config)) ==
         EXIT_STATUS_ONE_ARM_REMAINING);
  config_destroy(config);
}

void test_sim_round_robin_consistency(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 3 -plies "
      "2 -threads 1 -iter 52 -scond none -sr rr");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack 1 AEIQRST");
  load_and_exec_config_or_die(config, "gen");

  uint64_t seed = time(NULL);
  SimResults *sim_results_single_threaded = config_get_sim_results(config);
  SimResults *sim_results_multithreaded = sim_results_create();
  for (int i = 0; i < 11; i++) {
    char *set_threads_cmd =
        get_formatted_string("set -threads %d -seed %lu", i + 1, seed);
    load_and_exec_config_or_die(config, set_threads_cmd);
    free(set_threads_cmd);

    SimResults *sim_results;

    if (i == 0) {
      sim_results = sim_results_single_threaded;
    } else {
      sim_results = sim_results_multithreaded;
    }

    error_code_t status =
        config_simulate_and_return_status(config, NULL, sim_results);
    assert(status == ERROR_STATUS_SUCCESS);
    assert(thread_control_get_exit_status(config_get_thread_control(config)) ==
           EXIT_STATUS_SAMPLE_LIMIT);

    if (i != 0) {
      assert_sim_results_equal(sim_results_single_threaded, sim_results);
    }
  }

  sim_results_destroy(sim_results_multithreaded);
  config_destroy(config);
}

void test_sim_top_two_consistency(void) {
  Config *config =
      config_create_or_die("set -lex CSW21 -numplays 15 -plies 5 -threads 10 "
                           "-iter 30 -scond 99 -seed 33 -sr tt");
  load_and_exec_config_or_die(config, "cgp " PARRODQ_CGP);
  load_and_exec_config_or_die(config, "gen");

  // Get the initial reference results.
  SimResults *expected_sim_results = config_get_sim_results(config);
  assert(config_simulate_and_return_status(
             config, NULL, expected_sim_results) == ERROR_STATUS_SUCCESS);
  exit_status_t expected_exit_status = bai_result_get_exit_status(
      sim_results_get_bai_result(expected_sim_results));

  SimResults *actual_sim_results = sim_results_create();
  for (int i = 0; i < 2; i++) {
    assert(config_simulate_and_return_status(
               config, NULL, actual_sim_results) == ERROR_STATUS_SUCCESS);
    exit_status_t actual_exit_status = bai_result_get_exit_status(
        sim_results_get_bai_result(actual_sim_results));
    assert(actual_exit_status == expected_exit_status);
    assert_sim_results_equal(expected_sim_results, actual_sim_results);
  }

  sim_results_destroy(actual_sim_results);
  config_destroy(config);
}

void perf_test_multithread_sim(void) {
  Config *config = config_create_or_die(
      "set -s1 score -s2 score -r1 all -r2 all "
      "-threads 4 -plies 2 -it 1000 -numplays 15 -scond none -pfreq 100");
  load_and_exec_config_or_die(
      config,
      "cgp "
      "C14/O2TOY9/mIRADOR8/F4DAB2PUGH1/I5GOOEY3V/T4XI2MALTHA/14N/6GUM3OWN/"
      "7PEW2DOE/9EF1DOR/2KUNA1J1BEVELS/3TURRETs2S2/7A4T2/7N7/7S7 EEEIILZ/ "
      "336/298 0 -lex NWL20;");
  load_and_exec_config_or_die(config, "gen");

  SimResults *sim_results = config_get_sim_results(config);
  error_code_t status =
      config_simulate_and_return_status(config, NULL, sim_results);
  assert(status == ERROR_STATUS_SUCCESS);
  assert(thread_control_get_exit_status(config_get_thread_control(config)) ==
         EXIT_STATUS_SAMPLE_LIMIT);
  assert(sim_results_get_iteration_count(sim_results) == 1000);

  print_sim_stats(config_get_game(config), sim_results);
  sim_results_sort_plays_by_win_rate(sim_results);

  SimmedPlay *play = sim_results_get_sorted_simmed_play(sim_results, 0);
  StringBuilder *move_string_builder = string_builder_create();
  string_builder_add_move_description(
      move_string_builder, simmed_play_get_move(play), config_get_ld(config));

  assert(strings_equal(string_builder_peek(move_string_builder), "14F ZI.E"));

  string_builder_destroy(move_string_builder);
  config_destroy(config);
}

void test_play_similarity(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all "
      "-plies 2 -threads 1 -it 1200 -scond none -pfreq 100");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack 1 ACEIRST");
  load_and_exec_config_or_die(config, "gen");
  SimResults *sim_results = config_get_sim_results(config);
  error_code_t status =
      config_simulate_and_return_status(config, NULL, sim_results);
  assert(status == ERROR_STATUS_SUCCESS);
  assert(thread_control_get_exit_status(config_get_thread_control(config)) ==
         EXIT_STATUS_SAMPLE_LIMIT);

  // The BAI should have marked inferior plays in the same position as the best
  // play as epigons.
  const Move *best_play =
      simmed_play_get_move(sim_results_get_sorted_simmed_play(sim_results, 0));
  const int best_play_col = move_get_col_start(best_play);
  const int best_play_row = move_get_row_start(best_play);
  const int num_plays = sim_results_get_number_of_plays(sim_results);
  for (int i = 1; i < num_plays; i++) {
    SimmedPlay *play_i = sim_results_get_sorted_simmed_play(sim_results, i);
    Move *move_i = simmed_play_get_move(play_i);
    if (move_get_col_start(move_i) == best_play_col &&
        move_get_row_start(move_i) == best_play_row) {
      assert(simmed_play_get_is_epigon(play_i));
    }
  }

  config_destroy(config);
}

void test_similar_play_consistency(const int num_threads) {
  // The number of iterations needs to be less than 2 *
  // BAI_ARM_SAMPLE_MINIMUM so that neither play is marked as an epigon
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all "
      "-plies 2 -it 90 -scond none -numplays 2 -sr "
      "rr");
  char *set_threads_cmd = get_formatted_string("set -threads %d", num_threads);
  load_and_exec_config_or_die(config, set_threads_cmd);
  free(set_threads_cmd);
  load_and_exec_config_or_die(config, "cgp " CACHEXIC_CGP);
  load_and_exec_config_or_die(config, "gen");

  // The two top plays are:
  //
  // A1 cACHEXI(C)
  // A1 CAcHEXI(C)
  //
  // which only differ in which C is the blank. There are no words in the
  // lexicon which fit the following patterns:
  //
  // C.OES
  // C.GOO
  //
  // and since all plays start with the same seed and are sampled the exact
  // same number of times by the round robin sampling method, the sim results
  // for these plays should be identical.
  SimResults *sim_results = config_get_sim_results(config);
  error_code_t status =
      config_simulate_and_return_status(config, NULL, sim_results);
  assert(status == ERROR_STATUS_SUCCESS);
  assert(thread_control_get_exit_status(config_get_thread_control(config)) ==
         EXIT_STATUS_SAMPLE_LIMIT);
  SimmedPlay *p1 = sim_results_get_simmed_play(sim_results, 0);
  const Move *m1 = simmed_play_get_move(p1);
  SimmedPlay *p2 = sim_results_get_simmed_play(sim_results, 1);
  const Move *m2 = simmed_play_get_move(p2);

  assert(move_get_score(m1) == move_get_score(m2));
  assert(move_get_col_start(m1) == move_get_col_start(m2));
  assert(move_get_row_start(m1) == move_get_row_start(m2));
  assert(move_get_dir(m1) == move_get_dir(m2));
  assert(move_get_tiles_length(m1) == move_get_tiles_length(m2));
  assert(move_get_tiles_played(m1) == move_get_tiles_played(m2));
  assert_simmed_plays_stats_are_equal(p1, p2, 2);

  config_destroy(config);
}

typedef struct SimStrategyStats {
  // ** Staged values ** //
  int staged_num_samples;
  double staged_total_time;
  // ******************* //
  int total;
  Stat *num_samples;
  Stat *total_time;
} SimStrategyStats;

SimStrategyStats *sim_strategy_stats_create(void) {
  SimStrategyStats *stats = malloc(sizeof(SimStrategyStats));
  stats->total = 0;
  stats->num_samples = stat_create(true);
  stats->total_time = stat_create(true);
  return stats;
}

void sim_strategy_stats_destroy(SimStrategyStats *stats) {
  stat_destroy(stats->num_samples);
  stat_destroy(stats->total_time);
  free(stats);
}

void sim_strategy_stats_stage(SimStrategyStats **stats, int j, int num_samples,
                              double total_time) {
  SimStrategyStats *sss = stats[j];
  sss->staged_num_samples = num_samples;
  sss->staged_total_time = total_time;
}

void sim_strategy_stats_commit(SimStrategyStats **stats, int j) {
  SimStrategyStats *sss = stats[j];
  sss->total++;
  stat_push(sss->num_samples, sss->staged_num_samples, 1);
  stat_push(sss->total_time, sss->staged_total_time, 1);
}

// Overwrites the existing file with the new updated stats
void write_stats_to_file(const char *filename, const char *strategies[],
                         SimStrategyStats **stats, int num_strategies) {

  FILE *output_file = fopen(filename, "w");
  if (!output_file) {
    log_fatal("failed to open output file '%s'\n", filename);
  }

  // Write header row
  fprintf(output_file, "%-20s | %-11s | %-11s | %-11s\n", "Strategy", "Samples",
          "Samples/Sec", "Total Time");

  // Write stats for each strategy
  for (int j = 0; j < num_strategies; j++) {
    const SimStrategyStats *stats_j = stats[j];
    fprintf(output_file, "%-20s | %-11.2f | %-11.2f | %-11.2f\n", strategies[j],
            stat_get_mean(stats_j->num_samples),
            stat_get_mean(stats_j->num_samples) /
                stat_get_mean(stats_j->total_time),
            stat_get_mean(stats_j->total_time));
  }

  fclose_or_die(output_file);
}

void append_game_with_moves_to_file(const char *filename, const Game *game,
                                    const MoveList *move_list) {
  FILE *output_file = fopen(filename, "a");
  if (!output_file) {
    log_fatal("failed to open output file '%s'\n", filename);
  }
  StringBuilder *game_string = string_builder_create();
  string_builder_add_game(game_string, game, move_list);
  fprintf(output_file, "%s\n", string_builder_peek(game_string));
  string_builder_destroy(game_string);
  fclose_or_die(output_file);
}

void append_content_to_file(const char *filename, const char *sim_stats_str) {
  FILE *output_file = fopen(filename, "a");
  if (!output_file) {
    log_fatal("failed to open output file '%s'\n", filename);
  }
  fprintf(output_file, "%s\n", sim_stats_str);
  fclose_or_die(output_file);
}

void test_sim_perf(const char *sim_perf_iters) {
  const int num_iters = atoi(sim_perf_iters);
  if (num_iters < 0) {
    log_fatal("Invalid number of iterations: %s\n", sim_perf_iters);
  }
  Config *config =
      config_create_or_die("set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 "
                           "all -numplays 100 -plies 2 -scond 99");
  const uint64_t max_samples = 200000;
  char *set_threads_cmd =
      get_formatted_string("set -threads 1 -iter %lu", max_samples);
  load_and_exec_config_or_die(config, set_threads_cmd);
  free(set_threads_cmd);
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  Game *game = config_get_game(config);
  Bag *bag = game_get_bag(game);
  const char *strategies[] = {
      "-sr tt -threads 10",
      "-sr tf -threads 10",
  };
  const int num_strategies = sizeof(strategies) / sizeof(strategies[0]);
  SimStrategyStats **stats =
      malloc_or_die(num_strategies * sizeof(SimStrategyStats *));
  for (int i = 0; i < num_strategies; i++) {
    stats[i] = sim_strategy_stats_create();
  }
  SimResults *sim_results = config_get_sim_results(config);
  BAIResult *bai_result = sim_results_get_bai_result(sim_results);
  ThreadControl *thread_control = config_get_thread_control(config);
  const char *sim_perf_filename = "sim_perf_stats.txt";
  const char *sim_perf_game_details_filename = "sim_perf_game_details.txt";
  if (remove(sim_perf_game_details_filename) != 0 && errno != ENOENT) {
    log_fatal("error deleting %s: %s", sim_perf_game_details_filename);
  }
  draw_starting_racks(game);
  for (int i = 0; i < num_iters; i++) {
    if (bag_get_tiles(bag) < RACK_SIZE) {
      game_reset(game);
      draw_starting_racks(game);
    }
    load_and_exec_config_or_die(config, "gen -wmp true");
    append_game_with_moves_to_file(sim_perf_game_details_filename, game,
                                   config_get_move_list(config));
    for (int j = 0; j < num_strategies; j++) {
      char *set_strategies_cmd =
          get_formatted_string("set %s -wmp true", strategies[j]);
      load_and_exec_config_or_die(config, set_strategies_cmd);
      free(set_strategies_cmd);
      thread_control_set_seed(thread_control, i);
      append_content_to_file(sim_perf_game_details_filename, strategies[j]);
      const error_code_t status =
          config_simulate_and_return_status(config, NULL, sim_results);
      assert(status == ERROR_STATUS_SUCCESS);

      char *sim_stats_str =
          ucgi_sim_stats(game, sim_results,
                         (double)sim_results_get_node_count(sim_results) /
                             thread_control_get_seconds_elapsed(thread_control),
                         false);
      append_content_to_file(sim_perf_game_details_filename, sim_stats_str);
      free(sim_stats_str);
      sim_strategy_stats_stage(stats, j,
                               sim_results_get_iteration_count(sim_results),
                               bai_result_get_total_time(bai_result));
    }
    for (int j = 0; j < num_strategies; j++) {
      sim_strategy_stats_commit(stats, j);
    }
    write_stats_to_file(sim_perf_filename, strategies, stats, num_strategies);
    const Move *best_play =
        get_top_equity_move(game, 0, config_get_move_list(config));
    play_move(best_play, game, NULL, NULL);
  }
  for (int i = 0; i < num_strategies; i++) {
    sim_strategy_stats_destroy(stats[i]);
  }
  free(stats);
  config_destroy(config);
}

void test_sim(void) {
  const char *sim_perf_iters = getenv("SIM_PERF_ITERS");
  if (sim_perf_iters) {
    test_sim_perf(sim_perf_iters);
  } else {
    test_similar_play_consistency(1);
    test_similar_play_consistency(10);
    test_sim_error_cases();
    test_sim_single_iteration();
    test_sim_threshold();
    test_sim_time_limit();
    test_sim_one_arm_remaining();
    test_more_iterations();
    test_play_similarity();
    perf_test_multithread_sim();
    test_sim_round_robin_consistency();
    test_sim_top_two_consistency();
  }
}
