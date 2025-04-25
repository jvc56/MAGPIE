#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/def/config_defs.h"
#include "../../src/def/simmer_defs.h"
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

#include "../../src/str/move_string.h"

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

void test_win_pct(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all");
  assert(within_epsilon(win_pct_get(config_get_win_pcts(config), 118, 90),
                        0.844430));
  config_destroy(config);
}

void test_sim_error_cases(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 15 -plies "
      "2 -threads 1 -iter 1 -scond none");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack 1 AAADERW");
  sim_status_t status =
      config_simulate(config, NULL, config_get_sim_results(config));
  assert(status == SIM_STATUS_NO_MOVES);
  config_destroy(config);
}

void test_sim_single_iteration(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 15 -plies "
      "2 -threads 1 -iter 1 -scond none");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack 1 AAADERW");
  load_and_exec_config_or_die(config, "gen");
  sim_status_t status =
      config_simulate(config, NULL, config_get_sim_results(config));
  assert(status == SIM_STATUS_SUCCESS);
  assert(thread_control_get_exit_status(config_get_thread_control(config)) ==
         EXIT_STATUS_SAMPLE_LIMIT);
  config_destroy(config);
}

void test_more_iterations(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 15 -plies "
      "2 -threads 1 -iter 500 -scond none");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack 1 AEIQRST");
  load_and_exec_config_or_die(config, "gen");
  SimResults *sim_results = config_get_sim_results(config);
  sim_status_t status = config_simulate(config, NULL, sim_results);
  assert(status == SIM_STATUS_SUCCESS);
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
  sim_status_t *status;
  pthread_mutex_t *mutex;
  pthread_cond_t *cond;
  int *done;
} SimTestArgs;

void *sim_thread_func(void *arg) {
  SimTestArgs *args = (SimTestArgs *)arg;
  *(args->status) = config_simulate(args->config, NULL, args->sim_results);

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
  sim_status_t status;
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

  assert(status == SIM_STATUS_SUCCESS);
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
      "-plies 2 -threads 1 -it 1000000000 -scond none -tlim 5");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack 1 ACEIRST");
  load_and_exec_config_or_die(config, "gen");

  SimResults *sim_results = config_get_sim_results(config);
  sim_status_t status;
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

  assert(status == SIM_STATUS_SUCCESS);
  assert(thread_control_get_exit_status(config_get_thread_control(config)) ==
         EXIT_STATUS_TIME_LIMIT);
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
  sim_status_t status;
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

  assert(status == SIM_STATUS_SUCCESS);
  assert(thread_control_get_exit_status(config_get_thread_control(config)) ==
         EXIT_STATUS_ONE_ARM_REMAINING);
  config_destroy(config);
}

void test_sim_consistency(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 15 -plies "
      "2 -threads 1 -iter 30 -scond none -sr rr");
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

    sim_status_t status = config_simulate(config, NULL, sim_results);
    assert(status == SIM_STATUS_SUCCESS);
    assert(thread_control_get_exit_status(config_get_thread_control(config)) ==
           EXIT_STATUS_SAMPLE_LIMIT);

    if (i != 0) {
      assert_sim_results_equal(sim_results_single_threaded, sim_results);
    }
  }

  sim_results_destroy(sim_results_multithreaded);
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
  sim_status_t status = config_simulate(config, NULL, sim_results);
  assert(status == SIM_STATUS_SUCCESS);
  assert(thread_control_get_exit_status(config_get_thread_control(config)) ==
         EXIT_STATUS_SAMPLE_LIMIT);
  printf("iter count: %d\n", sim_results_get_iteration_count(sim_results));
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
  sim_status_t status = config_simulate(config, NULL, sim_results);
  assert(status == SIM_STATUS_SUCCESS);
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

void test_seed_consistency(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all "
      "-plies 2 -threads 10 -it 1000 -epigon 10000 -scond none -numplays 2 -sr "
      "rr");
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
  sim_status_t status = config_simulate(config, NULL, sim_results);
  assert(status == SIM_STATUS_SUCCESS);
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

void test_sim_perf(const char *sim_perf_iters, const char *sim_perf_threads) {
  const int num_iters = atoi(sim_perf_iters);
  if (num_iters < 0) {
    log_fatal("Invalid number of iterations: %s\n", sim_perf_iters);
  }
  int num_threads = 8;
  if (sim_perf_threads) {
    num_threads = atoi(sim_perf_threads);
    if (num_threads < 0 || num_threads >= MAX_THREADS) {
      log_fatal("Invalid number of threads: %s\n", sim_perf_iters);
    }
  }
  Config *config =
      config_create_or_die("set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 "
                           "all -numplays 15 -plies 2 -iter 5000 -scond 99");
  char *set_threads_cmd = get_formatted_string("set -threads %d", num_threads);
  load_and_exec_config_or_die(config, set_threads_cmd);
  free(set_threads_cmd);
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  Game *game = config_get_game(config);
  Bag *bag = game_get_bag(game);
  MoveList *move_list = config_get_move_list(config);
  const char *strategies[] = {
      "-sr rr -ev false -threshold none",
      "-sr tas -ev false -threshold ht",
      "-sr tas -ev true -threshold ht",
      "-sr tt -ev false -threshold ht"
      "-sr tt -ev true -threshold ht",
      "-sr tas -ev true -threshold ht",
      "-sr rr -ev false -threshold ht",
      "-sr tas -ev true -threshold gk16",
  };
  const int num_strategies = sizeof(strategies) / sizeof(strategies[0]);
  const int stats_per_strat = 6;
  const int num_stats = num_strategies * stats_per_strat;

  Stat **stats = malloc_or_die(num_stats * sizeof(Stat *));
  for (int i = 0; i < num_stats; i++) {
    stats[i] = stat_create(true);
  }
  SimResults *sim_results = config_get_sim_results(config);
  BAIResult *bai_result = sim_results_get_bai_result(sim_results);
  ThreadControl *thread_control = config_get_thread_control(config);
  const char *output_filename = "sim_perf_stats.txt";
  for (int i = 0; i < num_iters; i++) {
    if (bag_get_tiles(bag) < RACK_SIZE) {
      game_reset(game);
      draw_starting_racks(game);
    }
    load_and_exec_config_or_die(config, "gen");
    for (int j = 0; j < num_strategies; j++) {
      char *set_strategies_cmd = get_formatted_string("set %s", strategies[j]);
      load_and_exec_config_or_die(config, set_strategies_cmd);
      free(set_strategies_cmd);

      const sim_status_t status = config_simulate(config, NULL, sim_results);
      assert(status == SIM_STATUS_SUCCESS);
      const double total_time = bai_result_get_total_time(bai_result);
      const double sample_time = bai_result_get_sample_time(bai_result);
      const double bai_time = total_time - sample_time;
      const exit_status_t exit_status =
          thread_control_get_exit_status(thread_control);
      int exit_status_offset = 0;
      if (exit_status == EXIT_STATUS_SAMPLE_LIMIT ||
          exit_status == EXIT_STATUS_ONE_ARM_REMAINING) {
        exit_status_offset = stats_per_strat / 2;
      } else if (exit_status != EXIT_STATUS_THRESHOLD) {
        log_fatal("unexpected sim performance test exit status: %d\n",
                  exit_status);
      }
      const int base_index = j * stats_per_strat + exit_status_offset;
      stat_push(stats[base_index], total_time, 1);
      stat_push(stats[base_index + 1], sample_time, 1);
      stat_push(stats[base_index + 2], bai_time, 1);
    }

    FILE *output_file = fopen(output_filename, "w");
    if (!output_file) {
      log_fatal("failed to open output file '%s'\n", output_filename);
    }

    // Write header row
    fprintf(output_file, "Strategy | Threshold Exit | Sample Limit Exit\n");
    fprintf(output_file,
            "          | Total Time      | Sample Time    | BAI Time    | "
            "Total Time      | Sample Time    | BAI Time    |\n");

    // Write stats for each strategy
    for (int j = 0; j < num_strategies; j++) {
      const int base_index = j * stats_per_strat;
      fprintf(output_file, "%s | ", strategies[j]);

      // Threshold exit
      fprintf(output_file, "             | ");
      fprintf(output_file, "%f %f | ", stat_get_mean(stats[base_index]),
              stat_get_stdev(stats[base_index]));
      fprintf(output_file, "%f %f | ", stat_get_mean(stats[base_index + 1]),
              stat_get_stdev(stats[base_index + 1]));
      fprintf(output_file, "%f %f | ", stat_get_mean(stats[base_index + 2]),
              stat_get_stdev(stats[base_index + 2]));

      // Sample limit exit
      fprintf(output_file, "             | ");
      fprintf(output_file, "%f %f | ", stat_get_mean(stats[base_index + 3]),
              stat_get_stdev(stats[base_index + 3]));
      fprintf(output_file, "%f %f | ", stat_get_mean(stats[base_index + 4]),
              stat_get_stdev(stats[base_index + 4]));
      fprintf(output_file, "%f %f |\n", stat_get_mean(stats[base_index + 5]),
              stat_get_stdev(stats[base_index + 5]));
    }

    fclose(output_file);

    const Move *best_play = get_top_equity_move(game, 0, move_list);
    play_move(best_play, game, NULL, NULL);
  }
  for (int i = 0; i < num_stats; i++) {
    stat_destroy(stats[i]);
  }
  free(stats);
  config_destroy(config);
}

void test_sim(void) {
  const char *sim_perf_iters = getenv("SIM_PERF_ITERS");
  const char *sim_perf_threads = getenv("SIM_PERF_THREADS");
  if (sim_perf_iters) {
    test_sim_perf(sim_perf_iters, sim_perf_threads);
  } else {
    test_win_pct();
    test_sim_error_cases();
    test_sim_single_iteration();
    test_sim_threshold();
    test_sim_time_limit();
    test_sim_one_arm_remaining();
    test_more_iterations();
    test_play_similarity();
    perf_test_multithread_sim();
    test_sim_consistency();
    test_seed_consistency();
  }
}
