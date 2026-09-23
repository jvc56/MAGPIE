#include "pat_cost_ratio_test.h"

#include "../src/def/game_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/pat.h"
#include "../src/ent/player.h"
#include "../src/ent/sim_results.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "pat_move_choice_test.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

// Companion to pat_rollout_value_test.c's phase 1: that file times only its
// nested-sim oracle (phase 2), on the assumption that phase 1's own
// round-robin comparison -- PAT-on vs PAT-off during rollout forward-play,
// both conditions round-robin simming the SAME frozen 12-candidate list at
// the SAME 400 iterations/candidate -- already gives each condition an
// equal footing because it gives them an equal iteration count. This file
// checks that assumption: if a rollout iteration with PAT active at every
// simulated ply (get_top_equity_move, called through move_gen.c's
// `pat && !args->disable_pat` branch) costs a different amount of
// wall-clock than one with PAT forced off, an equal ITERATION count is not
// an equal COMPUTE budget, and whichever condition is cheaper per
// iteration was, in effect, handed a bigger budget at the same nominal
// sample size.
//
// Part 1 (test_pat_cost_ratio_measure) times condition A (PAT on) and
// condition B (PAT off) against each other, position by position, using
// the identical outer-config setup pat_rollout_value_test.c's
// test_pat_rollout_value uses (same lexicon, candidate count, ply depth,
// thread count, utility blend, and position-generation recipe), so the
// measured ratio describes the exact configuration the real experiment
// runs, not a synthetic proxy for it. It alternates which condition runs
// first, position by position, to cancel any systematic drift (cache
// warmth, thermal throttling) across the run, and re-issues the same
// "set -seed" before each condition so both time an identical simulated
// workload regardless of order.
//
// Part 2 (pat_cost_ratio_crossover_pilot) is the equal-wall-clock
// crossover this ratio exists to enable: PAT-on at
// PAT_COST_RATIO_ITERATIONS_PER_CANDIDATE iterations/candidate (unchanged)
// against PAT-off at round(that * r) iterations/candidate, where r is
// Part 1's own measured ratio (passed in, not re-measured, so a stale
// ratio is never silently reused -- see run_test's
// "patcostratiopilot:<r>" hook). For context it ALSO runs PAT-off at the
// unscaled iteration count on the same positions, so a single pilot run
// reports both the naive equal-count comparison pat_rollout_value_test.c
// already makes and the equal-time comparison side by side.
#define PAT_COST_RATIO_LEXICON "CSW21"
#define PAT_COST_RATIO_NUM_CANDIDATES 12
#define PAT_COST_RATIO_PLIES 4
#define PAT_COST_RATIO_ITERATIONS_PER_CANDIDATE 400
#define PAT_COST_RATIO_THREADS 10
#define PAT_COST_RATIO_MEASURE_NUM_POSITIONS 200
#define PAT_COST_RATIO_PILOT_NUM_POSITIONS 150
// Distinct from pat_rollout_value_test.c's PAT_ROLLOUT_VALUE_*_SEED_BASE
// (5100000000ULL) and leave_rollout_value_test.c's (5200000000ULL) so a
// run of this file never replays the same self-play positions as either.
#define PAT_COST_RATIO_MEASURE_SEED_BASE 5300000000ULL
#define PAT_COST_RATIO_PILOT_SEED_BASE 5400000000ULL
#define PAT_COST_RATIO_SIM_SEED_OFFSET 111000000ULL

// Same considered default blend as pat_rollout_value_test.c and
// pat_move_choice_test.c (see the former's file comment for the
// config.c-verified rationale); redefined here rather than shared because
// each of those files independently verifies its own config took the
// value, and this file's outer config is its own Config instance.
#define UTILITY_W_WINPCT_BLEND 1.0
#define UTILITY_W_SPREAD_BLEND 0.5
#define UTILITY_SPREAD_SCALE_BLEND 100.0

#define PAT_COST_RATIO_CHECKPOINT_EVERY 20

static Config *pat_cost_ratio_config_create(void) {
  Config *config = pat_move_choice_config_create();
  char *wmp_path =
      get_formatted_string("./data/lexica/%s.wmp", PAT_COST_RATIO_LEXICON);
  char *rit_path =
      get_formatted_string("./data/lexica/%s.rit", PAT_COST_RATIO_LEXICON);
  char *wit_path =
      get_formatted_string("./data/lexica/%s.wit", PAT_COST_RATIO_LEXICON);
  const bool have_wmp = access(wmp_path, R_OK) == 0;
  const bool have_rit = access(rit_path, R_OK) == 0;
  const bool have_wit = access(wit_path, R_OK) == 0;
  free(wmp_path);
  free(rit_path);
  free(wit_path);
  char *set_cmd = get_formatted_string(
      "set -wmp %s %s %s -numplays %d -plies %d -threads %d -iter %d -sr rr "
      "-scond none -threshold none -uwin %f -uspread %f -uspreadscale %f",
      have_wmp ? "true" : "false", have_rit ? "-rit true -ritmmap true" : "",
      have_wit ? "-wit true" : "", PAT_COST_RATIO_NUM_CANDIDATES,
      PAT_COST_RATIO_PLIES, PAT_COST_RATIO_THREADS,
      PAT_COST_RATIO_NUM_CANDIDATES * PAT_COST_RATIO_ITERATIONS_PER_CANDIDATE,
      UTILITY_W_WINPCT_BLEND, UTILITY_W_SPREAD_BLEND,
      UTILITY_SPREAD_SCALE_BLEND);
  load_and_exec_config_or_die(config, set_cmd);
  free(set_cmd);
  const double actual_uwin = config_get_utility_w_winpct(config);
  const double actual_uspread = config_get_utility_w_spread(config);
  const double actual_uscale = config_get_utility_spread_scale(config);
  fprintf(stderr,
          "[pat-cost-ratio] outer config: uwin=%.4f uspread=%.4f "
          "uspreadscale=%.4f\n",
          actual_uwin, actual_uspread, actual_uscale);
  if (actual_uwin != UTILITY_W_WINPCT_BLEND ||
      actual_uspread != UTILITY_W_SPREAD_BLEND ||
      actual_uscale != UTILITY_SPREAD_SCALE_BLEND) {
    fprintf(stderr, "[pat-cost-ratio] FATAL: outer config utility weights "
                    "did not take effect as requested\n");
    abort();
  }
  return config;
}

// Plays a fresh seeded self-play position down to a bag size drawn
// uniformly from PAT_MOVE_CHOICE_DEFAULT_BAG_LO/HI (the champion's own
// play, PAT on -- same recipe pat_rollout_value_test.c's phase 1 uses),
// then freezes the top-K static-equity candidate list via "gen". Returns
// false (position not usable) on game-end during setup, an empty bag, a
// transposed/invalid board, or fewer than 2 candidates -- the same
// disqualifiers phase 1 applies, in the same order.
static bool pat_cost_ratio_setup_position(Config *config, Player *player0,
                                          Player *player1,
                                          MoveList *setup_move_list,
                                          uint64_t position_seed) {
  Game *game = config_get_game(config);
  game_reset(game);
  game_seed(game, position_seed);
  draw_starting_racks(game);
  player_set_rollout_disable_pat(player0, false);
  player_set_rollout_disable_pat(player1, false);
  const int target_bag =
      PAT_MOVE_CHOICE_DEFAULT_BAG_LO +
      (int)(position_seed % (uint64_t)(PAT_MOVE_CHOICE_DEFAULT_BAG_HI -
                                       PAT_MOVE_CHOICE_DEFAULT_BAG_LO + 1));
  while (bag_get_letters(game_get_bag(game)) > target_bag) {
    const Move *setup_move = get_top_equity_move(game, setup_move_list);
    play_move(setup_move, game, NULL);
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      return false;
    }
  }
  if (bag_get_letters(game_get_bag(game)) == 0) {
    return false;
  }
  const Board *board = game_get_board(game);
  if (board_get_transposed(board) || !board_get_cross_sets_valid(board)) {
    return false;
  }
  load_and_exec_config_or_die(config, "gen");
  if (move_list_get_count(config_get_move_list(config)) < 2) {
    return false;
  }
  return true;
}

static void pat_cost_ratio_set_seed(Config *config, uint64_t position_seed) {
  char *seed_cmd = get_formatted_string(
      "set -seed %llu",
      (unsigned long long)(position_seed + PAT_COST_RATIO_SIM_SEED_OFFSET));
  load_and_exec_config_or_die(config, seed_cmd);
  free(seed_cmd);
}

static void pat_cost_ratio_set_iter(Config *config, int total_iterations) {
  char *iter_cmd = get_formatted_string("set -iter %d", total_iterations);
  load_and_exec_config_or_die(config, iter_cmd);
  free(iter_cmd);
}

void test_pat_cost_ratio_measure(void) {
  Config *config = pat_cost_ratio_config_create();
  Player *player0 = game_get_player(config_get_game(config), 0);
  Player *player1 = game_get_player(config_get_game(config), 1);
  SimResults *sim_results = config_get_sim_results(config);
  MoveList *setup_move_list = move_list_create(1);

  const int total_iters =
      PAT_COST_RATIO_NUM_CANDIDATES * PAT_COST_RATIO_ITERATIONS_PER_CANDIDATE;

  int num_positions_considered = 0;
  int num_positions_measured = 0;
  double sum_seconds_on = 0.0;
  double sum_seconds_off = 0.0;
  double sum_ratio = 0.0;
  double sum_ratio_sq = 0.0;

  for (int attempt = 0; attempt < PAT_COST_RATIO_MEASURE_NUM_POSITIONS;
       attempt++) {
    const uint64_t position_seed =
        PAT_COST_RATIO_MEASURE_SEED_BASE + (uint64_t)attempt;
    if (!pat_cost_ratio_setup_position(config, player0, player1,
                                       setup_move_list, position_seed)) {
      continue;
    }
    num_positions_considered++;

    // Alternate which condition runs first so any systematic drift across
    // the run (cache warmth, thermal throttling) lands on both conditions
    // equally rather than always favoring whichever runs second.
    const bool on_first = (attempt % 2) == 0;
    double seconds_on = 0.0;
    double seconds_off = 0.0;
    bool ok = true;
    for (int step = 0; step < 2 && ok; step++) {
      const bool is_on = (step == 0) ? on_first : !on_first;
      // Reset to the same seed before every timed call (not just the
      // first) so both conditions simulate an identical workload
      // regardless of which one runs first.
      pat_cost_ratio_set_seed(config, position_seed);
      player_set_rollout_disable_pat(player0, !is_on);
      player_set_rollout_disable_pat(player1, !is_on);
      struct timespec t0;
      struct timespec t1;
      clock_gettime(CLOCK_MONOTONIC, &t0);
      const error_code_t status =
          config_simulate_and_return_status(config, NULL, NULL, sim_results);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      if (status != ERROR_STATUS_SUCCESS) {
        ok = false;
        break;
      }
      const double elapsed = (double)(t1.tv_sec - t0.tv_sec) +
                             (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
      if (is_on) {
        seconds_on = elapsed;
      } else {
        seconds_off = elapsed;
      }
    }
    player_set_rollout_disable_pat(player0, false);
    player_set_rollout_disable_pat(player1, false);
    if (!ok || seconds_off <= 0.0) {
      continue;
    }
    num_positions_measured++;
    sum_seconds_on += seconds_on;
    sum_seconds_off += seconds_off;
    const double per_position_ratio = seconds_on / seconds_off;
    sum_ratio += per_position_ratio;
    sum_ratio_sq += per_position_ratio * per_position_ratio;

    if (num_positions_measured % PAT_COST_RATIO_CHECKPOINT_EVERY == 0) {
      const double pooled_ratio = sum_seconds_on / sum_seconds_off;
      printf("[pat-cost-ratio checkpoint] %d positions measured "
             "(%d considered): pooled r=on/off=%.4f, mean per-position "
             "ratio=%.4f\n",
             num_positions_measured, num_positions_considered, pooled_ratio,
             sum_ratio / num_positions_measured);
      fflush(stdout);
    }
  }

  move_list_destroy(setup_move_list);

  if (num_positions_measured == 0) {
    printf("[pat-cost-ratio] no positions successfully measured\n");
    return;
  }

  const double per_iter_on =
      sum_seconds_on / ((double)num_positions_measured * total_iters);
  const double per_iter_off =
      sum_seconds_off / ((double)num_positions_measured * total_iters);
  const double pooled_ratio = sum_seconds_on / sum_seconds_off;
  const double mean_ratio = sum_ratio / num_positions_measured;
  const double var_ratio =
      sum_ratio_sq / num_positions_measured - mean_ratio * mean_ratio;
  const double sd_ratio = var_ratio > 0.0 ? sqrt(var_ratio) : 0.0;

  printf("\n[pat-cost-ratio] FINAL: %d positions measured (%d considered), "
         "%d iterations/call (%d candidates x %d/candidate)\n",
         num_positions_measured, num_positions_considered, total_iters,
         PAT_COST_RATIO_NUM_CANDIDATES,
         PAT_COST_RATIO_ITERATIONS_PER_CANDIDATE);
  printf("[pat-cost-ratio] total: PAT-on=%.3fs PAT-off=%.3fs over %d calls "
         "each\n",
         sum_seconds_on, sum_seconds_off, num_positions_measured);
  printf("[pat-cost-ratio] per-call avg: PAT-on=%.2fms PAT-off=%.2fms\n",
         1000.0 * sum_seconds_on / num_positions_measured,
         1000.0 * sum_seconds_off / num_positions_measured);
  printf("[pat-cost-ratio] per-iteration: PAT-on=%.9fs PAT-off=%.9fs\n",
         per_iter_on, per_iter_off);
  printf("[pat-cost-ratio] RATIO r (pooled, seconds_on/seconds_off) = "
         "%.6f\n",
         pooled_ratio);
  printf("[pat-cost-ratio] RATIO (mean of per-position ratios) = %.6f, "
         "sd=%.6f (n=%d)\n",
         mean_ratio, sd_ratio, num_positions_measured);
  printf("[pat-cost-ratio] For an equal-wall-clock comparison: PAT-off "
         "should run %d x %.6f ~= %d iterations/candidate against PAT-on's "
         "%d.\n",
         PAT_COST_RATIO_ITERATIONS_PER_CANDIDATE, pooled_ratio,
         (int)(PAT_COST_RATIO_ITERATIONS_PER_CANDIDATE * pooled_ratio + 0.5),
         PAT_COST_RATIO_ITERATIONS_PER_CANDIDATE);
}

// The three rollout PAT conditions test_pat_cost_ratio_tws_only_measure
// times against each other.
enum {
  PAT_TWS_COST_CONDITION_FULL,
  PAT_TWS_COST_CONDITION_TWS_ONLY,
  PAT_TWS_COST_CONDITION_OFF,
  PAT_TWS_COST_NUM_CONDITIONS,
};

// Distinct from every other seed base in this file and in
// pat_rollout_value_test.c/leave_rollout_value_test.c (see those files'
// own seed-base comments), so this run never replays the same self-play
// positions as any of them.
#define PAT_TWS_COST_RATIO_MEASURE_SEED_BASE 5500000000ULL

static void pat_tws_cost_apply_condition(Player *player0, Player *player1,
                                         int condition) {
  const bool disable = (condition == PAT_TWS_COST_CONDITION_OFF);
  const uint32_t class_mask =
      (condition == PAT_TWS_COST_CONDITION_TWS_ONLY)
          ? (PAT_CLASS_MASK_ALL & ~(uint32_t)PAT_CLASS_MASK_TWS_ONLY)
          : 0;
  player_set_rollout_disable_pat(player0, disable);
  player_set_rollout_disable_pat(player1, disable);
  player_set_rollout_pat_disabled_classes_mask(player0, class_mask);
  player_set_rollout_pat_disabled_classes_mask(player1, class_mask);
}

static const char *pat_tws_cost_condition_label(int condition) {
  switch (condition) {
  case PAT_TWS_COST_CONDITION_FULL:
    return "full";
  case PAT_TWS_COST_CONDITION_TWS_ONLY:
    return "tws_only";
  case PAT_TWS_COST_CONDITION_OFF:
    return "off";
  default:
    return "?";
  }
}

// Same position-generation recipe and outer config as
// test_pat_cost_ratio_measure, but timing three rollout conditions instead
// of two. Each position's three conditions run in a rotated order (attempt
// % PAT_TWS_COST_NUM_CONDITIONS picks which condition goes first) so no
// single condition systematically benefits from running first or last --
// the same drift-cancellation test_pat_cost_ratio_measure's on/off
// alternation uses, generalized to three conditions. Reports all three
// pairwise ratios: tws_only/off (what a TWS-only rollout costs relative to
// no PAT at all -- the number the rollout question actually needs),
// full/off (a consistency check against test_pat_cost_ratio_measure's own
// separately-run 2.25x), and tws_only/full (how much of full PAT's own
// overhead TWS-only avoids).
void test_pat_cost_ratio_tws_only_measure(void) {
  Config *config = pat_cost_ratio_config_create();
  Player *player0 = game_get_player(config_get_game(config), 0);
  Player *player1 = game_get_player(config_get_game(config), 1);
  SimResults *sim_results = config_get_sim_results(config);
  MoveList *setup_move_list = move_list_create(1);

  const int total_iters =
      PAT_COST_RATIO_NUM_CANDIDATES * PAT_COST_RATIO_ITERATIONS_PER_CANDIDATE;

  int num_positions_considered = 0;
  int num_positions_measured = 0;
  double sum_seconds[PAT_TWS_COST_NUM_CONDITIONS];
  for (int condition = 0; condition < PAT_TWS_COST_NUM_CONDITIONS;
       condition++) {
    sum_seconds[condition] = 0.0;
  }

  for (int attempt = 0; attempt < PAT_COST_RATIO_MEASURE_NUM_POSITIONS;
       attempt++) {
    const uint64_t position_seed =
        PAT_TWS_COST_RATIO_MEASURE_SEED_BASE + (uint64_t)attempt;
    if (!pat_cost_ratio_setup_position(config, player0, player1,
                                       setup_move_list, position_seed)) {
      continue;
    }
    num_positions_considered++;

    const int rotation = attempt % PAT_TWS_COST_NUM_CONDITIONS;
    double seconds[PAT_TWS_COST_NUM_CONDITIONS];
    bool ok = true;
    for (int step = 0; step < PAT_TWS_COST_NUM_CONDITIONS && ok; step++) {
      const int condition = (step + rotation) % PAT_TWS_COST_NUM_CONDITIONS;
      // Reset to the same seed before every timed call, same reason as
      // test_pat_cost_ratio_measure: every condition simulates an
      // identical workload regardless of order.
      pat_cost_ratio_set_seed(config, position_seed);
      pat_tws_cost_apply_condition(player0, player1, condition);
      struct timespec t0;
      struct timespec t1;
      clock_gettime(CLOCK_MONOTONIC, &t0);
      const error_code_t status =
          config_simulate_and_return_status(config, NULL, NULL, sim_results);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      if (status != ERROR_STATUS_SUCCESS) {
        ok = false;
        break;
      }
      seconds[condition] = (double)(t1.tv_sec - t0.tv_sec) +
                           (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    }
    pat_tws_cost_apply_condition(player0, player1, PAT_TWS_COST_CONDITION_FULL);
    if (!ok) {
      continue;
    }
    num_positions_measured++;
    for (int condition = 0; condition < PAT_TWS_COST_NUM_CONDITIONS;
         condition++) {
      sum_seconds[condition] += seconds[condition];
    }

    if (num_positions_measured % PAT_COST_RATIO_CHECKPOINT_EVERY == 0) {
      printf("[pat-tws-cost-ratio checkpoint] %d positions measured (%d "
             "considered): tws_only/off=%.4f full/off=%.4f "
             "tws_only/full=%.4f\n",
             num_positions_measured, num_positions_considered,
             sum_seconds[PAT_TWS_COST_CONDITION_TWS_ONLY] /
                 sum_seconds[PAT_TWS_COST_CONDITION_OFF],
             sum_seconds[PAT_TWS_COST_CONDITION_FULL] /
                 sum_seconds[PAT_TWS_COST_CONDITION_OFF],
             sum_seconds[PAT_TWS_COST_CONDITION_TWS_ONLY] /
                 sum_seconds[PAT_TWS_COST_CONDITION_FULL]);
      fflush(stdout);
    }
  }

  move_list_destroy(setup_move_list);

  if (num_positions_measured == 0) {
    printf("[pat-tws-cost-ratio] no positions successfully measured\n");
    return;
  }

  printf("\n[pat-tws-cost-ratio] FINAL: %d positions measured (%d "
         "considered), %d iterations/call (%d candidates x %d/candidate)\n",
         num_positions_measured, num_positions_considered, total_iters,
         PAT_COST_RATIO_NUM_CANDIDATES,
         PAT_COST_RATIO_ITERATIONS_PER_CANDIDATE);
  for (int condition = 0; condition < PAT_TWS_COST_NUM_CONDITIONS;
       condition++) {
    printf("[pat-tws-cost-ratio] %s: total=%.3fs per-call=%.2fms "
           "per-iteration=%.9fs\n",
           pat_tws_cost_condition_label(condition), sum_seconds[condition],
           1000.0 * sum_seconds[condition] / num_positions_measured,
           sum_seconds[condition] /
               ((double)num_positions_measured * total_iters));
  }
  printf("[pat-tws-cost-ratio] RATIO tws_only/off = %.6f\n",
         sum_seconds[PAT_TWS_COST_CONDITION_TWS_ONLY] /
             sum_seconds[PAT_TWS_COST_CONDITION_OFF]);
  printf("[pat-tws-cost-ratio] RATIO full/off = %.6f (consistency check "
         "against test_pat_cost_ratio_measure's own separate run)\n",
         sum_seconds[PAT_TWS_COST_CONDITION_FULL] /
             sum_seconds[PAT_TWS_COST_CONDITION_OFF]);
  printf("[pat-tws-cost-ratio] RATIO tws_only/full = %.6f\n",
         sum_seconds[PAT_TWS_COST_CONDITION_TWS_ONLY] /
             sum_seconds[PAT_TWS_COST_CONDITION_FULL]);
}

void pat_cost_ratio_crossover_pilot(double cost_ratio_on_over_off) {
  if (!(cost_ratio_on_over_off > 0.0)) {
    printf("[pat-cost-ratio-pilot] FATAL: cost ratio must be positive, "
           "got %f\n",
           cost_ratio_on_over_off);
    abort();
  }
  Config *config = pat_cost_ratio_config_create();
  Player *player0 = game_get_player(config_get_game(config), 0);
  Player *player1 = game_get_player(config_get_game(config), 1);
  SimResults *sim_results = config_get_sim_results(config);
  MoveList *setup_move_list = move_list_create(1);

  const int iters_per_candidate_matched = (int)fmax(
      1.0,
      PAT_COST_RATIO_ITERATIONS_PER_CANDIDATE * cost_ratio_on_over_off + 0.5);
  const int iter_total_on =
      PAT_COST_RATIO_NUM_CANDIDATES * PAT_COST_RATIO_ITERATIONS_PER_CANDIDATE;
  const int iter_total_off_equal = iter_total_on;
  const int iter_total_off_matched =
      PAT_COST_RATIO_NUM_CANDIDATES * iters_per_candidate_matched;

  printf("[pat-cost-ratio-pilot] r=%.6f -> PAT-off matched at %d "
         "iterations/candidate (vs PAT-on's %d, PAT-off-equal's %d)\n",
         cost_ratio_on_over_off, iters_per_candidate_matched,
         PAT_COST_RATIO_ITERATIONS_PER_CANDIDATE,
         PAT_COST_RATIO_ITERATIONS_PER_CANDIDATE);

  int num_positions_considered = 0;
  int num_positions_compared = 0;
  int num_disagree_equal_count = 0;
  int num_disagree_equal_time = 0;
  int num_off_pair_disagree = 0;

  for (int attempt = 0; attempt < PAT_COST_RATIO_PILOT_NUM_POSITIONS;
       attempt++) {
    const uint64_t position_seed =
        PAT_COST_RATIO_PILOT_SEED_BASE + (uint64_t)attempt;
    if (!pat_cost_ratio_setup_position(config, player0, player1,
                                       setup_move_list, position_seed)) {
      continue;
    }
    num_positions_considered++;

    // Condition A: PAT on, the unchanged baseline sample count.
    pat_cost_ratio_set_seed(config, position_seed);
    pat_cost_ratio_set_iter(config, iter_total_on);
    player_set_rollout_disable_pat(player0, false);
    player_set_rollout_disable_pat(player1, false);
    if (config_simulate_and_return_status(config, NULL, NULL, sim_results) !=
        ERROR_STATUS_SUCCESS) {
      continue;
    }
    Move move_on;
    move_copy(&move_on, sim_results_get_best_move(sim_results));

    // Condition B-equal: PAT off, the SAME sample count as condition A --
    // the naive comparison pat_rollout_value_test.c's phase 1 already
    // makes, reported here for side-by-side reference.
    pat_cost_ratio_set_seed(config, position_seed);
    pat_cost_ratio_set_iter(config, iter_total_off_equal);
    player_set_rollout_disable_pat(player0, true);
    player_set_rollout_disable_pat(player1, true);
    if (config_simulate_and_return_status(config, NULL, NULL, sim_results) !=
        ERROR_STATUS_SUCCESS) {
      player_set_rollout_disable_pat(player0, false);
      player_set_rollout_disable_pat(player1, false);
      continue;
    }
    Move move_off_equal;
    move_copy(&move_off_equal, sim_results_get_best_move(sim_results));

    // Condition B-matched: PAT off, cost-ratio-scaled sample count -- the
    // equal-wall-clock comparison this pilot exists to run.
    pat_cost_ratio_set_seed(config, position_seed);
    pat_cost_ratio_set_iter(config, iter_total_off_matched);
    if (config_simulate_and_return_status(config, NULL, NULL, sim_results) !=
        ERROR_STATUS_SUCCESS) {
      player_set_rollout_disable_pat(player0, false);
      player_set_rollout_disable_pat(player1, false);
      continue;
    }
    Move move_off_matched;
    move_copy(&move_off_matched, sim_results_get_best_move(sim_results));

    player_set_rollout_disable_pat(player0, false);
    player_set_rollout_disable_pat(player1, false);

    if (move_get_type(&move_on) != GAME_EVENT_TILE_PLACEMENT_MOVE ||
        move_get_type(&move_off_equal) != GAME_EVENT_TILE_PLACEMENT_MOVE ||
        move_get_type(&move_off_matched) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    num_positions_compared++;
    if (compare_moves_without_equity(&move_on, &move_off_equal, true) != -1) {
      num_disagree_equal_count++;
    }
    if (compare_moves_without_equity(&move_on, &move_off_matched, true) != -1) {
      num_disagree_equal_time++;
    }
    if (compare_moves_without_equity(&move_off_equal, &move_off_matched,
                                     true) != -1) {
      num_off_pair_disagree++;
    }

    if (num_positions_compared % PAT_COST_RATIO_CHECKPOINT_EVERY == 0) {
      printf("[pat-cost-ratio-pilot checkpoint] %d positions compared "
             "(%d considered): equal-count disagree %.1f%%, equal-time "
             "disagree %.1f%%, off-pair disagree %.1f%%\n",
             num_positions_compared, num_positions_considered,
             100.0 * num_disagree_equal_count / num_positions_compared,
             100.0 * num_disagree_equal_time / num_positions_compared,
             100.0 * num_off_pair_disagree / num_positions_compared);
      fflush(stdout);
    }
  }

  move_list_destroy(setup_move_list);

  printf("\n[pat-cost-ratio-pilot] FINAL: %d positions considered, %d "
         "compared (all three conditions produced a tile-placement move)\n",
         num_positions_considered, num_positions_compared);
  if (num_positions_compared == 0) {
    printf("[pat-cost-ratio-pilot] no positions successfully compared\n");
    return;
  }
  printf("[pat-cost-ratio-pilot] PAT-on(%d/candidate) vs "
         "PAT-off-equal-count(%d/candidate): %d disagreements (%.2f%%)\n",
         PAT_COST_RATIO_ITERATIONS_PER_CANDIDATE,
         PAT_COST_RATIO_ITERATIONS_PER_CANDIDATE, num_disagree_equal_count,
         100.0 * num_disagree_equal_count / num_positions_compared);
  printf("[pat-cost-ratio-pilot] PAT-on(%d/candidate) vs "
         "PAT-off-equal-time(%d/candidate, r=%.4f): %d disagreements "
         "(%.2f%%)\n",
         PAT_COST_RATIO_ITERATIONS_PER_CANDIDATE, iters_per_candidate_matched,
         cost_ratio_on_over_off, num_disagree_equal_time,
         100.0 * num_disagree_equal_time / num_positions_compared);
  printf("[pat-cost-ratio-pilot] PAT-off-equal-count vs "
         "PAT-off-equal-time (does scaling PAT-off's own budget change ITS "
         "OWN answer): %d disagreements (%.2f%%)\n",
         num_off_pair_disagree,
         100.0 * num_off_pair_disagree / num_positions_compared);
}

void pat_cost_ratio_run_pilot_spec(const char *spec) {
  char *end = NULL;
  const double ratio = strtod(spec, &end);
  if (end == spec || !(ratio > 0.0)) {
    printf("[pat-cost-ratio-pilot] FATAL: could not parse a positive ratio "
           "from spec \"%s\" (expected \"patcostratiopilot:<r>\")\n",
           spec);
    abort();
  }
  pat_cost_ratio_crossover_pilot(ratio);
}
