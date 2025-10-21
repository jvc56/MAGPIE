#include "../src/compat/cpthread.h"
#include "../src/compat/ctime.h"
#include "../src/def/cpthread_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/bai_result.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/str/game_string.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int get_best_simmed_play_index(const SimResults *sim_results) {
  const int num_simmed_plays = sim_results_get_number_of_plays(sim_results);
  if (num_simmed_plays == 0) {
    return -1;
  }
  int best_play_index = -1;
  const SimmedPlay *best_simmed_play = NULL;
  for (int i = 0; i < num_simmed_plays; i++) {
    const SimmedPlay *simmed_play = sim_results_get_simmed_play(sim_results, i);
    if (simmed_play_get_is_epigon(simmed_play)) {
      continue;
    }
    if (!best_simmed_play ||
        stat_get_mean(simmed_play_get_win_pct_stat(simmed_play)) >
            stat_get_mean(simmed_play_get_win_pct_stat(best_simmed_play))) {
      best_simmed_play = simmed_play;
      best_play_index = i;
    }
  }
  return best_play_index;
}

const SimmedPlay *get_best_simmed_play(const SimResults *sim_results) {
  const int best_play_index = get_best_simmed_play_index(sim_results);
  if (best_play_index < 0) {
    return NULL;
  }
  return sim_results_get_simmed_play(sim_results, best_play_index);
}

void test_sim_error_cases(void) {
  Config *config =
      config_create_or_die("set -lex NWL20 -wmp true -s1 score -s2 score -r1 "
                           "all -r2 all -numplays 15 -plies "
                           "2 -threads 1 -iter 1 -scond none");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack AAADERW");
  error_code_t status = config_simulate_and_return_status(
      config, NULL, config_get_sim_results(config));
  assert(status == ERROR_STATUS_SIM_NO_MOVES);
  config_destroy(config);
}

void test_sim_single_iteration(void) {
  Config *config =
      config_create_or_die("set -lex NWL20 -wmp true -s1 score -s2 score -r1 "
                           "all -r2 all -numplays 15 -plies "
                           "2 -threads 1 -iter 1 -scond none");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack AAADERW");
  load_and_exec_config_or_die(config, "gen");
  error_code_t status = config_simulate_and_return_status(
      config, NULL, config_get_sim_results(config));
  assert(status == ERROR_STATUS_SUCCESS);
  assert(thread_control_get_status(config_get_thread_control(config)) ==
         THREAD_CONTROL_STATUS_SAMPLE_LIMIT);
  config_destroy(config);
}

void test_more_iterations(void) {
  Config *config =
      config_create_or_die("set -lex NWL20 -wmp true -s1 score -s2 score -r1 "
                           "all -r2 all -numplays 15 -plies "
                           "2 -threads 1 -iter 500 -scond none -seed 10");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack AEIQRST");
  load_and_exec_config_or_die(config, "gen");
  SimResults *sim_results = config_get_sim_results(config);
  error_code_t status =
      config_simulate_and_return_status(config, NULL, sim_results);
  assert(status == ERROR_STATUS_SUCCESS);
  assert(thread_control_get_status(config_get_thread_control(config)) ==
         THREAD_CONTROL_STATUS_SAMPLE_LIMIT);

  const SimmedPlay *play = get_best_simmed_play(sim_results);
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
  cpthread_mutex_t *mutex;
  cpthread_cond_t *cond;
  int *done;
} SimTestArgs;

void *sim_thread_func(void *arg) {
  SimTestArgs *args = (SimTestArgs *)arg;
  *(args->status) =
      config_simulate_and_return_status(args->config, NULL, args->sim_results);

  cpthread_mutex_lock(args->mutex);
  *(args->done) = 1;
  cpthread_cond_signal(args->cond);
  cpthread_mutex_unlock(args->mutex);

  return NULL;
}

void test_sim_threshold(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -wmp true -plies 2 -threads 8 -iter 100000000 -scond 95");
  load_and_exec_config_or_die(config, "cgp " ZILLION_OPENING_CGP);
  load_and_exec_config_or_die(config, "addmoves 8F.LIN,8D.ZILLION,8F.ZILLION");

  SimResults *sim_results = config_get_sim_results(config);
  error_code_t status;
  int done = 0;

  cpthread_mutex_t mutex;
  cpthread_mutex_init(&mutex);
  cpthread_cond_t cond;
  cpthread_cond_init(&cond);

  SimTestArgs args = {.config = config,
                      .sim_results = sim_results,
                      .status = &status,
                      .mutex = &mutex,
                      .cond = &cond,
                      .done = &done};

  cpthread_t thread;
  cpthread_create(&thread, sim_thread_func, &args);
  cpthread_cond_timedwait_loop(&cond, &mutex, 10, &done);
  cpthread_join(thread);

  assert(status == ERROR_STATUS_SUCCESS);
  assert(thread_control_get_status(config_get_thread_control(config)) ==
         THREAD_CONTROL_STATUS_THRESHOLD);

  const SimmedPlay *play = get_best_simmed_play(sim_results);
  StringBuilder *move_string_builder = string_builder_create();
  string_builder_add_move_description(
      move_string_builder, simmed_play_get_move(play), config_get_ld(config));

  assert(strings_equal(string_builder_peek(move_string_builder), "8D ZILLION"));

  config_destroy(config);
  string_builder_destroy(move_string_builder);
}

void test_sim_time_limit(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -wmp true -s1 score -s2 score -r1 all -r2 all "
      "-plies 2 -threads 1 -it 1000000000 -scond none -tlim 2");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack ACEIRST");
  load_and_exec_config_or_die(config, "gen");

  SimResults *sim_results = config_get_sim_results(config);
  error_code_t status;
  int done = 0;

  cpthread_mutex_t mutex;
  cpthread_mutex_init(&mutex);
  cpthread_cond_t cond;
  cpthread_cond_init(&cond);

  SimTestArgs args = {.config = config,
                      .sim_results = sim_results,
                      .status = &status,
                      .mutex = &mutex,
                      .cond = &cond,
                      .done = &done};

  cpthread_t thread;
  cpthread_create(&thread, sim_thread_func, &args);
  cpthread_cond_timedwait_loop(&cond, &mutex, 10, &done);
  cpthread_join(thread);

  assert(status == ERROR_STATUS_SUCCESS);
  assert(thread_control_get_status(config_get_thread_control(config)) ==
         THREAD_CONTROL_STATUS_TIMEOUT);
  config_destroy(config);
}

void test_sim_one_arm_remaining(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -wmp true -s1 score -s2 score -r1 all -r2 all "
      "-plies 2 -numplays 4 -threads 1 -it 1100 -scond none");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack ACEIRST");
  load_and_exec_config_or_die(
      config, "addmoves 8D.CRISTAE,8D.ATRESIC,8D.STEARIC,8D.RACIEST");

  SimResults *sim_results = config_get_sim_results(config);
  error_code_t status;
  int done = 0;

  cpthread_mutex_t mutex;
  cpthread_mutex_init(&mutex);
  cpthread_cond_t cond;
  cpthread_cond_init(&cond);

  SimTestArgs args = {.config = config,
                      .sim_results = sim_results,
                      .status = &status,
                      .mutex = &mutex,
                      .cond = &cond,
                      .done = &done};

  cpthread_t thread;
  cpthread_create(&thread, sim_thread_func, &args);
  cpthread_cond_timedwait_loop(&cond, &mutex, 10, &done);
  cpthread_join(thread);

  assert(status == ERROR_STATUS_SUCCESS);
  assert(thread_control_get_status(config_get_thread_control(config)) ==
         THREAD_CONTROL_STATUS_ONE_ARM_REMAINING);
  config_destroy(config);
}

void test_sim_round_robin_consistency(void) {
  Config *config =
      config_create_or_die("set -lex NWL20 -wmp true -s1 score -s2 score -r1 "
                           "all -r2 all -numplays 3 -plies "
                           "2 -threads 1 -iter 52 -scond none -sr rr");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack AEIQRST");
  load_and_exec_config_or_die(config, "gen");

  uint64_t seed = ctime_get_current_time();
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
    assert(thread_control_get_status(config_get_thread_control(config)) ==
           THREAD_CONTROL_STATUS_SAMPLE_LIMIT);

    if (i != 0) {
      assert_sim_results_equal(sim_results_single_threaded, sim_results);
    }
  }

  sim_results_destroy(sim_results_multithreaded);
  config_destroy(config);
}

void test_sim_top_two_consistency(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -numplays 15 -plies 5 -threads 10 "
      "-iter 30 -scond 99 -seed 33 -sr tt");
  load_and_exec_config_or_die(config, "cgp " PARRODQ_CGP);
  load_and_exec_config_or_die(config, "gen");
  ThreadControl *thread_control = config_get_thread_control(config);

  // Get the initial reference results.
  SimResults *expected_sim_results = config_get_sim_results(config);
  assert(config_simulate_and_return_status(
             config, NULL, expected_sim_results) == ERROR_STATUS_SUCCESS);
  thread_control_status_t expected_exit_status =
      thread_control_get_status(thread_control);

  SimResults *actual_sim_results = sim_results_create();
  for (int i = 0; i < 2; i++) {
    assert(config_simulate_and_return_status(
               config, NULL, actual_sim_results) == ERROR_STATUS_SUCCESS);
    thread_control_status_t actual_exit_status =
        thread_control_get_status(thread_control);
    assert(actual_exit_status == expected_exit_status);
    assert_sim_results_equal(expected_sim_results, actual_sim_results);
  }

  sim_results_destroy(actual_sim_results);
  config_destroy(config);
}

void perf_test_multithread_sim(void) {
  Config *config =
      config_create_or_die("set -s1 score -s2 score -r1 all -r2 all "
                           "-threads 4 -plies 2 -it 2000 -minp 50 -numplays 15 "
                           "-scond none -pfreq 100");
  load_and_exec_config_or_die(
      config,
      "cgp "
      "C14/O2TOY9/mIRADOR8/F4DAB2PUGH1/I5GOOEY3V/T4XI2MALTHA/14N/6GUM3OWN/"
      "7PEW2DOE/9EF1DOR/2KUNA1J1BEVELS/3TURRETs2S2/7A4T2/7N7/7S7 EEEIILZ/ "
      "336/298 0 -lex NWL20 -wmp true;");
  load_and_exec_config_or_die(config, "gen");

  SimResults *sim_results = config_get_sim_results(config);
  error_code_t status =
      config_simulate_and_return_status(config, NULL, sim_results);
  assert(status == ERROR_STATUS_SUCCESS);
  assert(thread_control_get_status(config_get_thread_control(config)) ==
         THREAD_CONTROL_STATUS_SAMPLE_LIMIT);
  assert(sim_results_get_iteration_count(sim_results) == 2000);

  const SimmedPlay *play = get_best_simmed_play(sim_results);
  StringBuilder *move_string_builder = string_builder_create();
  string_builder_add_move_description(
      move_string_builder, simmed_play_get_move(play), config_get_ld(config));

  assert(strings_equal(string_builder_peek(move_string_builder), "14F ZI.E"));

  string_builder_destroy(move_string_builder);
  config_destroy(config);
}

void test_sim_with_and_without_inference_helper(
    const char *gcg_file, const char *known_opp_rack_str,
    const char **moves_to_add, const char *winner_without_inference,
    const char *winner_with_inference) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-threads 10 -plies 2 -it 2000 -minp 50 -numplays 2 "
      "-scond none -seed 10");
  // Load an empty CGP to create a new game.
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  assert(test_parse_gcg(gcg_file, config, config_get_game_history(config)) ==
         ERROR_STATUS_SUCCESS);

  Game *game = config_get_game(config);

  game_play_to_end_or_die(config_get_game_history(config), game);

  StringBuilder *sb = string_builder_create();

  int moves_to_add_index = 0;
  while (true) {
    const char *move_to_add = moves_to_add[moves_to_add_index];
    if (move_to_add == NULL) {
      break;
    }
    string_builder_clear(sb);
    string_builder_add_formatted_string(sb, "addmoves %s", move_to_add);
    load_and_exec_config_or_die(config, string_builder_peek(sb));
    moves_to_add_index++;
  }
  string_builder_clear(sb);

  Rack known_opp_rack;
  rack_set_dist_size_and_reset(&known_opp_rack,
                               ld_get_size(config_get_ld(config)));
  rack_set_to_string(config_get_ld(config), &known_opp_rack,
                     known_opp_rack_str);
  SimResults *sim_results = config_get_sim_results(config);

  // Without inference
  error_code_t status =
      config_simulate_and_return_status(config, &known_opp_rack, sim_results);
  assert(status == ERROR_STATUS_SUCCESS);
  assert(thread_control_get_status(config_get_thread_control(config)) ==
         THREAD_CONTROL_STATUS_SAMPLE_LIMIT);
  string_builder_add_ucgi_move(
      sb, simmed_play_get_move(get_best_simmed_play(sim_results)),
      game_get_board(game), config_get_ld(config));
  printf("Best move without inference: >%s<\n", string_builder_peek(sb));
  assert(strings_equal(string_builder_peek(sb), winner_without_inference));
  string_builder_clear(sb);

  // With inference
  load_and_exec_config_or_die(config, "set -sinfer true");
  status =
      config_simulate_and_return_status(config, &known_opp_rack, sim_results);
  assert(status == ERROR_STATUS_SUCCESS);
  assert(thread_control_get_status(config_get_thread_control(config)) ==
         THREAD_CONTROL_STATUS_SAMPLE_LIMIT);
  string_builder_add_ucgi_move(
      sb, simmed_play_get_move(get_best_simmed_play(sim_results)),
      game_get_board(game), config_get_ld(config));
  printf("Actual best move:   >%s<\n", string_builder_peek(sb));
  printf("Expected best move: >%s<\n", winner_with_inference);
  assert(strings_equal(string_builder_peek(sb), winner_with_inference));

  string_builder_destroy(sb);
  config_destroy(config);
}

void test_sim_with_inference(void) {
  // 8H MUZAKS infers a leave of S, so playing EMPYREAN one short of the triple
  // word will sim worse with inferenc
  const char *empyrean_move_str = "h7.EMPYREAN";
  const char *napery_move_string = "9g.NAPERY";
  test_sim_with_and_without_inference_helper(
      "muzaks_empyrean", "",
      (const char *[]){empyrean_move_str, napery_move_string, NULL},
      empyrean_move_str, napery_move_string);

  // N6 ERE infers a leave of RE, so playing SYNCHRONIZE/D will sim worse with
  // inference because of the RESYNCHRONIZE/D extension.
  const char *synced_move_str = "1c.SYNCHRONIZED";
  const char *sync_move_string = "1c.SYNCHRONIZE";
  const char *ze_move_string = "b6.ZE";
  test_sim_with_and_without_inference_helper(
      "resynchronized", "",
      (const char *[]){synced_move_str, sync_move_string, ze_move_string, NULL},
      sync_move_string, ze_move_string);

  test_sim_with_and_without_inference_helper(
      "muzaks_empyrean", "IIIIIII",
      (const char *[]){empyrean_move_str, napery_move_string, NULL},
      empyrean_move_str, empyrean_move_str);
}

void test_play_similarity(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -wmp true -s1 score -s2 score -r1 all -r2 all "
      "-plies 2 -threads 10 -it 1200 -minp 50 -scond none -pfreq 100");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack ACEIRST");
  load_and_exec_config_or_die(config, "gen");
  SimResults *sim_results = config_get_sim_results(config);
  error_code_t status =
      config_simulate_and_return_status(config, NULL, sim_results);
  assert(status == ERROR_STATUS_SUCCESS);
  assert(thread_control_get_status(config_get_thread_control(config)) ==
         THREAD_CONTROL_STATUS_SAMPLE_LIMIT);

  // The BAI should have marked inferior plays in the same position as the best
  // play as epigons.
  const int best_play_index = get_best_simmed_play_index(sim_results);
  const Move *best_play = simmed_play_get_move(
      sim_results_get_simmed_play(sim_results, best_play_index));
  const int best_play_col = move_get_col_start(best_play);
  const int best_play_row = move_get_row_start(best_play);
  const int best_play_length = move_get_tiles_length(best_play);
  const int num_plays = sim_results_get_number_of_plays(sim_results);
  for (int i = 0; i < num_plays; i++) {
    if (i == best_play_index) {
      continue;
    }
    const SimmedPlay *play_i = sim_results_get_simmed_play(sim_results, i);
    const Move *move_i = simmed_play_get_move(play_i);
    if (move_get_col_start(move_i) == best_play_col &&
        move_get_row_start(move_i) == best_play_row &&
        move_get_tiles_length(move_i) == best_play_length) {
      assert(simmed_play_get_is_epigon(play_i));
    }
  }

  config_destroy(config);
}

void test_similar_play_consistency(const int num_threads) {
  // The number of iterations needs to be less than 2 *
  // BAI_ARM_SAMPLE_MINIMUM so that neither play is marked as an epigon
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
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
  assert(thread_control_get_status(config_get_thread_control(config)) ==
         THREAD_CONTROL_STATUS_SAMPLE_LIMIT);
  const SimmedPlay *p1 = sim_results_get_simmed_play(sim_results, 0);
  const Move *m1 = simmed_play_get_move(p1);
  const SimmedPlay *p2 = sim_results_get_simmed_play(sim_results, 1);
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
  SimStrategyStats *stats = malloc_or_die(sizeof(SimStrategyStats));
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
  FILE *output_file = fopen_or_die(filename, "w");
  // Write header row
  fprintf_or_die(output_file, "%-20s | %-11s | %-11s | %-11s\n", "Strategy",
                 "Samples", "Samples/Sec", "Total Time");

  // Write stats for each strategy
  for (int j = 0; j < num_strategies; j++) {
    const SimStrategyStats *stats_j = stats[j];
    fprintf_or_die(output_file, "%-20s | %-11.2f | %-11.2f | %-11.2f\n",
                   strategies[j], stat_get_mean(stats_j->num_samples),
                   stat_get_mean(stats_j->num_samples) /
                       stat_get_mean(stats_j->total_time),
                   stat_get_mean(stats_j->total_time));
  }

  fclose_or_die(output_file);
}

void append_game_with_moves_to_file(const char *filename, const Game *game,
                                    const MoveList *move_list) {
  FILE *output_file = fopen_or_die(filename, "a");
  StringBuilder *game_string = string_builder_create();
  GameStringOptions *gso = game_string_options_create_default();
  string_builder_add_game(game, move_list, gso, game_string);
  game_string_options_destroy(gso);
  fprintf_or_die(output_file, "%s\n", string_builder_peek(game_string));
  string_builder_destroy(game_string);
  fclose_or_die(output_file);
}

void append_content_to_file(const char *filename, const char *sim_stats_str) {
  FILE *output_file = fopen_or_die(filename, "a");
  fprintf_or_die(output_file, "%s\n", sim_stats_str);
  fclose_or_die(output_file);
}

void test_sim_perf(const char *sim_perf_iters) {
  ErrorStack *error_stack = error_stack_create();
  const int num_iters = string_to_int(sim_perf_iters, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("Invalid number of iterations: %s\n", sim_perf_iters);
  }
  error_stack_destroy(error_stack);
  if (num_iters < 0) {
    log_fatal("Invalid number of iterations: %s\n", sim_perf_iters);
  }
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 "
      "all -numplays 100 -plies 2 -scond 99");
  const uint64_t max_samples = 200000;
  char *set_threads_cmd =
      get_formatted_string("set -threads 1 -iter %lu", max_samples);
  load_and_exec_config_or_die(config, set_threads_cmd);
  free(set_threads_cmd);
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  Game *game = config_get_game(config);
  const Bag *bag = game_get_bag(game);
  const char *strategies[] = {
      "-sr oldtt -threads 12",
      "-sr tt -threads 12",
  };
  const int num_strategies = sizeof(strategies) / sizeof(strategies[0]);
  SimStrategyStats **stats =
      malloc_or_die(num_strategies * sizeof(SimStrategyStats *));
  for (int i = 0; i < num_strategies; i++) {
    stats[i] = sim_strategy_stats_create();
  }
  SimResults *sim_results = config_get_sim_results(config);
  const BAIResult *bai_result = sim_results_get_bai_result(sim_results);
  ThreadControl *thread_control = config_get_thread_control(config);
  const char *sim_perf_filename = "sim_perf_stats.txt";
  const char *sim_perf_game_details_filename = "sim_perf_game_details.txt";
  if (remove(sim_perf_game_details_filename) != 0 && errno != ENOENT) {
    log_fatal("error deleting %s: %s", sim_perf_game_details_filename);
  }
  draw_starting_racks(game);
  const int details_limit = 100;
  for (int i = 0; i < num_iters; i++) {
    if (bag_get_letters(bag) < RACK_SIZE) {
      game_reset(game);
      draw_starting_racks(game);
    }
    load_and_exec_config_or_die(config, "gen -wmp true");
    if (i < details_limit) {
      append_game_with_moves_to_file(sim_perf_game_details_filename, game,
                                     config_get_move_list(config));
    }
    for (int j = 0; j < num_strategies; j++) {
      char *set_strategies_cmd =
          get_formatted_string("set %s -wmp true", strategies[j]);
      load_and_exec_config_or_die(config, set_strategies_cmd);
      free(set_strategies_cmd);
      thread_control_set_seed(thread_control, i);
      if (i < details_limit) {
        append_content_to_file(sim_perf_game_details_filename, strategies[j]);
      }
      const error_code_t status =
          config_simulate_and_return_status(config, NULL, sim_results);
      assert(status == ERROR_STATUS_SUCCESS);

      char *sim_stats_str =
          ucgi_sim_stats(game, sim_results,
                         (double)sim_results_get_node_count(sim_results) /
                             thread_control_get_seconds_elapsed(thread_control),
                         false);
      if (i < details_limit) {
        append_content_to_file(sim_perf_game_details_filename, sim_stats_str);
      }
      free(sim_stats_str);
      sim_strategy_stats_stage(
          stats, j, (int)sim_results_get_iteration_count(sim_results),
          bai_result_get_total_time(bai_result));
    }
    for (int j = 0; j < num_strategies; j++) {
      sim_strategy_stats_commit(stats, j);
    }
    write_stats_to_file(sim_perf_filename, strategies, stats, num_strategies);
    const Move *best_play =
        get_top_equity_move(game, 0, config_get_move_list(config));
    play_move(best_play, game, NULL);
  }
  for (int i = 0; i < num_strategies; i++) {
    sim_strategy_stats_destroy(stats[i]);
  }
  free(stats);
  config_destroy(config);
}

void test_sim_one_ply(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -wmp true -s1 score -s2 score -r1 all -r2 all "
      "-plies 1 -threads 1 -iter 1000 -scond none");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack JIBERRS");
  load_and_exec_config_or_die(config, "gen");

  SimResults *sim_results = config_get_sim_results(config);
  error_code_t status =
      config_simulate_and_return_status(config, NULL, sim_results);
  assert(status == ERROR_STATUS_SUCCESS);
  assert(thread_control_get_status(config_get_thread_control(config)) ==
         THREAD_CONTROL_STATUS_SAMPLE_LIMIT);

  const SimmedPlay *play = get_best_simmed_play(sim_results);
  StringBuilder *move_string_builder = string_builder_create();
  string_builder_add_move_description(
      move_string_builder, simmed_play_get_move(play), config_get_ld(config));

  assert(strings_equal(string_builder_peek(move_string_builder), "8D JIBER"));

  config_destroy(config);
  string_builder_destroy(move_string_builder);
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
    test_sim_with_inference();
    test_sim_round_robin_consistency();
    test_sim_top_two_consistency();
    test_sim_one_ply();
  }
}
