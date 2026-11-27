// Tests for Locally Adaptive Monte Carlo (src/impl/lamc.h): its linear
// algebra, Gaussian process, play features and sim, and an on-demand replay
// that compares it with the standard sim on recorded rollouts.

#include "lamc_test.h"

#include "../src/compat/cpthread.h"
#include "../src/compat/cpthread_defs.h"
#include "../src/compat/ctime.h"
#include "../src/compat/memory_info.h"
#include "../src/def/bai_defs.h"
#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/lamc_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/play_feature_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/static_eval_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/autoplay_results.h"
#include "../src/ent/bag.h"
#include "../src/ent/bai_result.h"
#include "../src/ent/board.h"
#include "../src/ent/bonus_square.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/lamc_args.h"
#include "../src/ent/move.h"
#include "../src/ent/play_feature.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/gp.h"
#include "../src/impl/lamc.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/play_feature_registry.h"
#include "../src/impl/random_variable.h"
#include "../src/str/move_string.h"
#include "../src/util/dense_linalg.h"
#include "../src/util/fnv.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  LAMC_TEST_MOVE_LIST_CAPACITY = 100000,
  LAMC_TEST_GUARD_COLUMNS = 2,
  LAMC_TEST_GP_GRID = 6,
  LAMC_TEST_LIKELIHOOD_POINTS = 40,
  LAMC_TEST_TIME_CUT_SAMPLES = 64,
  LAMC_TEST_PLANTED_SAMPLES = 256,
  LAMC_TEST_BOUND_POINTS = 30,
  LAMC_TEST_BOUND_DIMS = 3,
  LAMC_TEST_BOUND_PROBES = 2000,
  LAMC_TEST_PLANTED_FINALISTS = 20,
  LAMC_REPLAY_MAX_VARIANTS = 32,
  LAMC_REPLAY_MAX_BUDGETS = 8,
  LAMC_REPLAY_MAX_THREADS = 64,
};

#define LAMC_TEST_GUARD_VALUE (-12345.0)

static void test_dense_linalg(void) {
  double matrix[9] = {4, 12, -16, 12, 37, -43, -16, -43, 98};
  assert(dense_cholesky(matrix, 3));
  const double expected[9] = {2, 0, 0, 6, 1, 0, -8, 5, 3};
  for (int entry_idx = 0; entry_idx < 9; entry_idx++) {
    assert(fabs(matrix[entry_idx] - expected[entry_idx]) < 1e-12);
  }
  const double original[9] = {4, 12, -16, 12, 37, -43, -16, -43, 98};
  const double rhs[3] = {1, 2, 3};
  double solution[3];
  dense_forward_solve(matrix, 3, rhs, solution);
  dense_backward_solve(matrix, 3, solution, solution);
  for (int row = 0; row < 3; row++) {
    double product = 0;
    for (int col = 0; col < 3; col++) {
      product += original[row * 3 + col] * solution[col];
    }
    assert(fabs(product - rhs[row]) < 1e-9);
  }
  double indefinite[4] = {1, 2, 2, 1};
  assert(!dense_cholesky(indefinite, 2));
}

static void test_table_and_subset(void) {
  // Three arms of four samples each.
  const double samples[12] = {0.1, 0.2, 0.3, 0.4, 1.0, 2.0,
                              3.0, 4.0, -1,  -2,  -3,  -4};
  const RandomVariablesArgs table_args = {
      .type = RANDOM_VARIABLES_TABLE,
      .num_rvs = 3,
      .num_samples = 4,
      .samples = samples,
  };
  RandomVariables *table = rvs_create(&table_args);
  assert(rvs_sample(table, 1, 0, NULL) == 1.0);
  assert(rvs_sample(table, 1, 0, NULL) == 2.0);
  assert(rvs_sample(table, 0, 0, NULL) == 0.1);

  // A view of arms 2 and 0, in that order, that accumulates their samples.
  atomic_uint_least64_t counts[3];
  atomic_int_least64_t sums[3];
  atomic_int_least64_t squares[3];
  for (int arm = 0; arm < 3; arm++) {
    atomic_init(&counts[arm], 0);
    atomic_init(&sums[arm], 0);
    atomic_init(&squares[arm], 0);
  }
  RVSubsetStats stats = {.counts = counts, .sums = sums, .squares = squares};
  const int arms[2] = {2, 0};
  const RandomVariablesArgs subset_args = {
      .type = RANDOM_VARIABLES_SUBSET,
      .num_rvs = 2,
      .base_rvs = table,
      .arm_indices = arms,
      .subset_stats = &stats,
  };
  RandomVariables *subset = rvs_create(&subset_args);
  assert(rvs_get_num_rvs(subset) == 2);
  assert(rvs_sample(subset, 0, 0, NULL) == -1.0);
  assert(rvs_sample(subset, 0, 0, NULL) == -2.0);
  // Arm 0 of the base continues where it left off.
  assert(rvs_sample(subset, 1, 0, NULL) == 0.2);
  assert(atomic_load(&counts[2]) == 2);
  assert(atomic_load(&counts[0]) == 1);
  assert(atomic_load(&counts[1]) == 0);
  assert(fabs((double)atomic_load(&sums[2]) / BAI_FIXED_POINT_SCALE + 3.0) <
         1e-9);
  assert(fabs((double)atomic_load(&squares[2]) / BAI_FIXED_POINT_SCALE - 5.0) <
         1e-9);
  rvs_destroy(subset);

  // A reset table starts every arm over.
  rvs_reset(table, &table_args);
  assert(rvs_sample(table, 2, 0, NULL) == -1.0);
  rvs_destroy(table);
}

static double lamc_test_bowl(const double first_coordinate,
                             const double second_coordinate) {
  return -((first_coordinate - 0.5) * (first_coordinate - 0.5)) -
         ((second_coordinate + 0.3) * (second_coordinate + 0.3));
}

// A smooth signal whose variance (0.005) is below each value's noise
// variance (0.02): the excess of the values' variance over the noise is
// negative, as with a few rollouts a play that each score 0 or 1, yet the
// values vary smoothly, so the likelihood chooses a signal variance that
// lets the fit follow them part way instead of staying on the prior mean.
static void test_gp_fit_by_likelihood(void) {
  GaussianProcess *gp = gp_create(LAMC_TEST_LIKELIHOOD_POINTS, 1);
  double points[LAMC_TEST_LIKELIHOOD_POINTS];
  double values[LAMC_TEST_LIKELIHOOD_POINTS];
  double noise[LAMC_TEST_LIKELIHOOD_POINTS];
  double value_squares = 0.0;
  for (int point_idx = 0; point_idx < LAMC_TEST_LIKELIHOOD_POINTS;
       point_idx++) {
    points[point_idx] = point_idx;
    values[point_idx] = 0.1 * sin(point_idx / 6.0);
    noise[point_idx] = 0.02;
    value_squares += values[point_idx] * values[point_idx];
  }
  assert(value_squares / LAMC_TEST_LIKELIHOOD_POINTS < 0.02);
  const double unit_scale[1] = {1.0};
  const double multiples[3] = {1.0, 2.0, 4.0};
  assert(gp_fit_by_likelihood(gp, GP_KERNEL_MATERN52, 0.0, unit_scale,
                              multiples, 3, points, values, noise,
                              LAMC_TEST_LIKELIHOOD_POINTS));
  // The signal's peak, at 3 pi, and its trough, at 9 pi.
  const double peak = 9.42477796;
  const double trough = 28.27433388;
  double peak_mean;
  double trough_mean;
  gp_predict(gp, &peak, &peak_mean, NULL);
  gp_predict(gp, &trough, &trough_mean, NULL);
  // About 0.03 from above and below; a fit left on the prior mean, as the
  // floored estimate gave, predicts about 0.0001.
  assert(peak_mean > 0.01);
  assert(trough_mean < -0.01);
  gp_destroy(gp);
}

// A uniform value in [0, 1) from a linear congruential generator.
static double lamc_test_uniform(uint64_t *state) {
  *state = *state * 6364136223846793005ULL + 1442695040888963407ULL;
  return (double)(*state >> 11) / 9007199254740992.0;
}

// gp_get_bounds bounds the posterior mean and variance at every point; the
// depth 1 search's pruned scans rely on it. Probed at the data and at random
// points, for both kernels, after fits by likelihood to noisy random values.
static void test_gp_bounds(void) {
  uint64_t state = 99;
  double points[LAMC_TEST_BOUND_POINTS * LAMC_TEST_BOUND_DIMS];
  double values[LAMC_TEST_BOUND_POINTS];
  double noise[LAMC_TEST_BOUND_POINTS];
  for (int point_idx = 0; point_idx < LAMC_TEST_BOUND_POINTS; point_idx++) {
    for (int dim_idx = 0; dim_idx < LAMC_TEST_BOUND_DIMS; dim_idx++) {
      points[point_idx * LAMC_TEST_BOUND_DIMS + dim_idx] =
          4.0 * lamc_test_uniform(&state) - 2.0;
    }
    values[point_idx] = lamc_test_uniform(&state) - 0.5;
    noise[point_idx] = 0.001 + 0.01 * lamc_test_uniform(&state);
  }
  const double base_scales[LAMC_TEST_BOUND_DIMS] = {1.0, 1.0, 1.0};
  const double multiples[3] = {0.5, 1.0, 2.0};
  const gp_kernel_t kernels[2] = {GP_KERNEL_MATERN52,
                                  GP_KERNEL_SQUARED_EXPONENTIAL};
  for (int kernel_idx = 0; kernel_idx < 2; kernel_idx++) {
    GaussianProcess *gp =
        gp_create(LAMC_TEST_BOUND_POINTS, LAMC_TEST_BOUND_DIMS);
    assert(gp_fit_by_likelihood(gp, kernels[kernel_idx], 0.0, base_scales,
                                multiples, 3, points, values, noise,
                                LAMC_TEST_BOUND_POINTS));
    double mean_bound = 0.0;
    double variance_bound = 0.0;
    gp_get_bounds(gp, &mean_bound, &variance_bound);
    for (int probe_idx = 0;
         probe_idx < LAMC_TEST_BOUND_PROBES + LAMC_TEST_BOUND_POINTS;
         probe_idx++) {
      double probe[LAMC_TEST_BOUND_DIMS];
      for (int dim_idx = 0; dim_idx < LAMC_TEST_BOUND_DIMS; dim_idx++) {
        probe[dim_idx] =
            probe_idx < LAMC_TEST_BOUND_POINTS
                ? points[probe_idx * LAMC_TEST_BOUND_DIMS + dim_idx]
                : 6.0 * lamc_test_uniform(&state) - 3.0;
      }
      double mean = 0.0;
      double variance = 0.0;
      gp_predict(gp, probe, &mean, &variance);
      assert(mean <= mean_bound + 1e-12);
      assert(variance <= variance_bound + 1e-12);
    }
    gp_destroy(gp);
  }
}

static void test_gp_kernel(const gp_kernel_t kernel) {
  // Interpolation without noise, and the prior far from the data.
  GaussianProcess *gp = gp_create(8, 1);
  const double points[4] = {0, 1, 2, 3};
  double values[4];
  double noise[4];
  for (int point_idx = 0; point_idx < 4; point_idx++) {
    values[point_idx] = sin(points[point_idx]);
    noise[point_idx] = 1e-12;
  }
  const double unit_scale[1] = {1.0};
  const GaussianProcessParams params = {
      .kernel = kernel,
      .inverse_length_scales = unit_scale,
      .signal_variance = 1.0,
      .prior_mean = 0.0,
  };
  assert(gp_fit(gp, &params, points, values, noise, 4));
  for (int point_idx = 0; point_idx < 4; point_idx++) {
    double mean;
    double variance;
    gp_predict(gp, &points[point_idx], &mean, &variance);
    assert(fabs(mean - values[point_idx]) < 1e-4);
    assert(variance < 1e-4);
  }
  double far_mean;
  double far_variance;
  const double far_point = 30.0;
  gp_predict(gp, &far_point, &far_mean, &far_variance);
  assert(fabs(far_mean) < 1e-3);
  assert(far_variance > 0.99);
  gp_destroy(gp);

  // The bowl: gradients against finite differences, and the maximizer.
  const int num_points = LAMC_TEST_GP_GRID * LAMC_TEST_GP_GRID;
  GaussianProcess *bowl = gp_create(num_points, 2);
  double bowl_points[LAMC_TEST_GP_GRID * LAMC_TEST_GP_GRID * 2];
  double bowl_values[LAMC_TEST_GP_GRID * LAMC_TEST_GP_GRID];
  double bowl_noise[LAMC_TEST_GP_GRID * LAMC_TEST_GP_GRID];
  for (int row = 0; row < LAMC_TEST_GP_GRID; row++) {
    for (int col = 0; col < LAMC_TEST_GP_GRID; col++) {
      const int cell_idx = row * LAMC_TEST_GP_GRID + col;
      bowl_points[(ptrdiff_t)cell_idx * 2] =
          -2.0 + 4.0 * row / (LAMC_TEST_GP_GRID - 1);
      bowl_points[cell_idx * 2 + 1] =
          -2.0 + 4.0 * col / (LAMC_TEST_GP_GRID - 1);
      bowl_values[cell_idx] =
          lamc_test_bowl(bowl_points[(ptrdiff_t)cell_idx * 2],
                         bowl_points[(ptrdiff_t)cell_idx * 2 + 1]);
      bowl_noise[cell_idx] = 1e-6;
    }
  }
  const double bowl_scales[2] = {0.7, 0.7};
  const GaussianProcessParams bowl_params = {
      .kernel = kernel,
      .inverse_length_scales = bowl_scales,
      .signal_variance = 4.0,
      .prior_mean = -2.0,
  };
  assert(gp_fit(bowl, &bowl_params, bowl_points, bowl_values, bowl_noise,
                num_points));
  const double probe[2] = {0.37, -1.1};
  double gradient[2];
  gp_mean_gradient(bowl, probe, gradient);
  for (int dim = 0; dim < 2; dim++) {
    double plus[2] = {probe[0], probe[1]};
    double minus[2] = {probe[0], probe[1]};
    plus[dim] += 1e-5;
    minus[dim] -= 1e-5;
    double plus_mean;
    double minus_mean;
    gp_predict(bowl, plus, &plus_mean, NULL);
    gp_predict(bowl, minus, &minus_mean, NULL);
    assert(fabs((plus_mean - minus_mean) / 2e-5 - gradient[dim]) < 1e-5);
  }
  double best_mean = -INFINITY;
  double best_point[2] = {0, 0};
  for (int step_x = 0; step_x <= 80; step_x++) {
    for (int step_y = 0; step_y <= 80; step_y++) {
      const double point[2] = {-2.0 + step_x * 0.05, -2.0 + step_y * 0.05};
      double mean;
      gp_predict(bowl, point, &mean, NULL);
      if (mean > best_mean) {
        best_mean = mean;
        best_point[0] = point[0];
        best_point[1] = point[1];
      }
    }
  }
  assert(fabs(best_point[0] - 0.5) < 0.25);
  assert(fabs(best_point[1] + 0.3) < 0.25);
  gp_destroy(bowl);
}

// With the bag empty, move generation values a play that does not go out by
// its leave's face value, so a pass, which keeps the whole rack, is valued
// below every other play; its equity feature must say so.
static void test_play_feature_pass_with_empty_bag(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -wmp true -s1 equity -s2 equity -r1 all -r2 all");
  load_and_exec_config_or_die(
      config, "cgp 9A1PIXY/9S1L3/2ToWNLETS1O3/9U1DA1R/3GERANIAL1U1I/9g2T1C/"
              "8WE2OBI/6EMU4ON/6AID3GO1/5HUN4ET1/4ZA1T4ME1/1Q1FAKEY3JOES/"
              "FIVE1E5IT1C/5SPORRAN2A/6ORE2N2D BGIV/DEHILOR 384/389 0 "
              "-lex NWL20");
  Game *game = config_get_game(config);
  assert(bag_get_letters(game_get_bag(game)) == 0);
  MoveList *moves = move_list_create(LAMC_TEST_MOVE_LIST_CAPACITY);
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = moves,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&gen_args);
  const int num_moves = move_list_get_count(moves);
  double *values = malloc_or_die(sizeof(double) * num_moves);
  PlayFeatureCtx ctx;
  play_feature_ctx_init(&ctx, game);
  play_feature_prepare(PLAY_FEATURE_EQUITY, &ctx);
  play_feature_extract(PLAY_FEATURE_EQUITY, &ctx, moves, 1, values);
  int pass_idx = -1;
  for (int move_idx = 0; move_idx < num_moves; move_idx++) {
    if (move_get_type(move_list_get_move(moves, move_idx)) == GAME_EVENT_PASS) {
      pass_idx = move_idx;
    }
  }
  assert(pass_idx >= 0);
  const Rack *rack = player_get_rack(
      game_get_player(game, game_get_player_on_turn_index(game)));
  assert(within_epsilon(
      values[pass_idx],
      equity_to_double(-2 * rack_get_score(config_get_ld(config), rack) -
                       NON_OUTPLAY_CONSTANT_PENALTY)));
  for (int move_idx = 0; move_idx < num_moves; move_idx++) {
    assert(values[pass_idx] <= values[move_idx]);
  }
  free(values);
  move_list_destroy(moves);
  config_destroy(config);
}

// Generates every play of the game's position and extracts one feature
// for each. The caller frees the values and destroys the move list.
static double *lamc_test_feature_values(Game *game,
                                        const play_feature_t feature,
                                        MoveList **moves_out) {
  MoveList *moves = move_list_create(LAMC_TEST_MOVE_LIST_CAPACITY);
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = moves,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&gen_args);
  const int num_moves = move_list_get_count(moves);
  const int num_columns = play_feature_get_num_columns(feature);
  double *values = malloc_or_die(sizeof(double) * num_moves * num_columns);
  PlayFeatureCtx ctx;
  play_feature_ctx_init(&ctx, game);
  play_feature_prepare(feature, &ctx);
  play_feature_extract(feature, &ctx, moves, num_columns, values);
  *moves_out = moves;
  return values;
}

// In a sim the opponent's rack is unknown and its tiles are in the bag
// object, so the tiles left in the bag after a play must be counted from the
// real bag, as move generation counts them, and a play that empties it has
// none left.
static void test_play_feature_bag_after_unknown_rack(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all");
  load_and_exec_config_or_die(
      config, "cgp 3V3W6L/1BEATY1U5GI/2XU3S4FEN/3TA2H4LOY/2GEN1DUCAT1AD1/"
              "2O1I1I2WRITE1/2V1M1ZOAEA4/3JAGER2DRILL/2BOtONE5O1/1FERER7Q1/"
              "4S8U1/12NaM/12ATE/13ST/14H ACEINOP/ 361/397 0 -lex CSW24");
  Game *game = config_get_game(config);
  const int real_bag = game_get_real_bag_size(game);
  assert(real_bag == 4);
  assert(bag_get_letters(game_get_bag(game)) == real_bag + RACK_SIZE);
  MoveList *moves = NULL;
  const double *values =
      lamc_test_feature_values(game, PLAY_FEATURE_BAG_AFTER, &moves);
  bool emptied = false;
  for (int move_idx = 0; move_idx < move_list_get_count(moves); move_idx++) {
    const Move *move = move_list_get_move(moves, move_idx);
    int expected = real_bag;
    if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      expected -= move_get_tiles_played(move);
    }
    if (expected < 0) {
      expected = 0;
    }
    emptied = emptied || expected == 0;
    assert(within_epsilon(values[move_idx], (double)expected));
  }
  assert(emptied);
  free((void *)values);
  move_list_destroy(moves);
  config_destroy(config);
}

// -lamcmaxbag counts the real bag, as autoplay sees it, and not the bag
// object, which at the command line also holds the opponent's unknown rack.
static void lamc_test_max_bag_search(const int max_bag,
                                     const int expected_searches) {
  char *settings = get_formatted_string(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 4 -plies 2 -threads 1 -iterations 300 "
      "-minplayiterations 10 -scond none -tlim 0 -seed 42 -sinfer false "
      "-lamc true -rlamc off -lamcmaxbag %d -savesettings false -hr false",
      max_bag);
  Config *config = config_create_or_die(settings);
  free(settings);
  load_and_exec_config_or_die(
      config, "cgp 3V3W6L/1BEATY1U5GI/2XU3S4FEN/3TA2H4LOY/2GEN1DUCAT1AD1/"
              "2O1I1I2WRITE1/2V1M1ZOAEA4/3JAGER2DRILL/2BOtONE5O1/1FERER7Q1/"
              "4S8U1/12NaM/12ATE/13ST/14H ACEINOP/ 361/397 0 -lex CSW24");
  assert(game_get_real_bag_size(config_get_game(config)) == 4);
  load_and_exec_config_or_die(config, "gen");
  lamc_totals_reset();
  load_and_exec_config_or_die(config, "sim");
  assert(lamc_get_totals().searches == (uint64_t)expected_searches);
  config_destroy(config);
}

static void test_lamc_max_bag_counts_real_bag(void) {
  lamc_test_max_bag_search(4, 1);
  lamc_test_max_bag_search(3, 0);
}

// The premium squares a play leaves the next play: the empty word premium
// squares in reach of a tile it places, leaving out the ones it covers.
static void test_play_feature_premium_leaves_covered_squares(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all");
  load_and_exec_config_or_die(config, "cgp " JOSH2_CGP);
  Game *game = config_get_game(config);
  const Board *board = game_get_board(game);
  MoveList *moves = NULL;
  const double *values =
      lamc_test_feature_values(game, PLAY_FEATURE_PREMIUM_ACCESS, &moves);
  int covering = 0;
  for (int move_idx = 0; move_idx < move_list_get_count(moves); move_idx++) {
    const Move *move = move_list_get_move(moves, move_idx);
    bool placed[BOARD_DIM][BOARD_DIM];
    memset(placed, 0, sizeof(placed));
    if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      const bool vertical = board_is_dir_vertical(move_get_dir(move));
      for (int tile_idx = 0; tile_idx < move_get_tiles_length(move);
           tile_idx++) {
        if (move_get_tile(move, tile_idx) == PLAYED_THROUGH_MARKER) {
          continue;
        }
        placed[move_get_row_start(move) + (vertical ? tile_idx : 0)]
              [move_get_col_start(move) + (vertical ? 0 : tile_idx)] = true;
      }
    }
    int expected[2] = {0, 0};
    bool covers = false;
    for (int row = 0; row < BOARD_DIM; row++) {
      for (int col = 0; col < BOARD_DIM; col++) {
        const BonusSquare bonus_square =
            board_get_bonus_square(board, row, col);
        if (!board_is_empty(board, row, col) ||
            bonus_square_is_brick(bonus_square) ||
            bonus_square_get_word_multiplier(bonus_square) < 2) {
          continue;
        }
        if (placed[row][col]) {
          covers = true;
          continue;
        }
        bool in_reach = false;
        for (int reach = -RACK_SIZE; reach <= RACK_SIZE && !in_reach; reach++) {
          const int along_row = col + reach;
          const int along_col = row + reach;
          in_reach = (along_row >= 0 && along_row < BOARD_DIM &&
                      placed[row][along_row]) ||
                     (along_col >= 0 && along_col < BOARD_DIM &&
                      placed[along_col][col]);
        }
        if (in_reach) {
          expected[bonus_square_get_word_multiplier(bonus_square) >= 3 ? 0
                                                                       : 1]++;
        }
      }
    }
    if (covers) {
      covering++;
    }
    assert(
        within_epsilon(values[(ptrdiff_t)2 * move_idx], (double)expected[0]));
    assert(within_epsilon(values[(ptrdiff_t)2 * move_idx + 1],
                          (double)expected[1]));
  }
  assert(covering > 0);
  free((void *)values);
  move_list_destroy(moves);
  config_destroy(config);
}

static void test_play_features(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all");
  load_and_exec_config_or_die(config, "cgp " JOSH2_CGP);
  Game *game = config_get_game(config);
  Game *game_before = game_duplicate(game);
  MoveList *moves = move_list_create(LAMC_TEST_MOVE_LIST_CAPACITY);
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = moves,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&gen_args);
  const int num_moves = move_list_get_count(moves);
  bool has_exchange = false;
  for (int move_idx = 0; move_idx < num_moves; move_idx++) {
    has_exchange |= move_get_type(move_list_get_move(moves, move_idx)) ==
                    GAME_EVENT_EXCHANGE;
  }
  assert(num_moves > 100);
  assert(has_exchange);

  const int stride = PLAY_FEATURE_MAX_COLUMNS + LAMC_TEST_GUARD_COLUMNS;
  double *first = malloc_or_die(sizeof(double) * num_moves * stride);
  double *second = malloc_or_die(sizeof(double) * num_moves * stride);
  for (int feature_idx = 0; feature_idx < NUMBER_OF_PLAY_FEATURES;
       feature_idx++) {
    const play_feature_t feature = (play_feature_t)feature_idx;
    const int num_columns = play_feature_get_num_columns(feature);
    assert(num_columns >= 1 && num_columns <= PLAY_FEATURE_MAX_COLUMNS);
    assert(play_feature_from_name(play_feature_get_name(feature)) == feature);
    for (int value_idx = 0; value_idx < num_moves * stride; value_idx++) {
      first[value_idx] = LAMC_TEST_GUARD_VALUE;
      second[value_idx] = LAMC_TEST_GUARD_VALUE;
    }
    PlayFeatureCtx ctx;
    play_feature_ctx_init(&ctx, game);
    play_feature_prepare(feature, &ctx);
    play_feature_extract(feature, &ctx, moves, stride, first);
    play_feature_extract(feature, &ctx, moves, stride, second);
    for (int move_idx = 0; move_idx < num_moves; move_idx++) {
      for (int col = 0; col < stride; col++) {
        const double value = first[move_idx * stride + col];
        assert(value == second[move_idx * stride + col]);
        if (col < num_columns) {
          assert(isfinite(value));
          assert(value != LAMC_TEST_GUARD_VALUE);
        } else {
          assert(value == LAMC_TEST_GUARD_VALUE);
        }
      }
      if (play_feature_get_kind(feature) == PLAY_FEATURE_KIND_CATEGORICAL) {
        for (int col = 0; col < num_columns; col++) {
          const double value = first[move_idx * stride + col];
          assert(value == 0.0 || value == 1.0);
        }
      }
    }
    assert_games_are_equal(game, game_before, true);
  }

  // A few meanings, checked directly.
  PlayFeatureCtx ctx;
  play_feature_ctx_init(&ctx, game);
  play_feature_extract(PLAY_FEATURE_SCORE, &ctx, moves, stride, first);
  play_feature_extract(PLAY_FEATURE_MOVE_TYPE, &ctx, moves, stride, second);
  for (int move_idx = 0; move_idx < num_moves; move_idx++) {
    const Move *move = move_list_get_move(moves, move_idx);
    assert(first[(ptrdiff_t)move_idx * stride] ==
           equity_to_double(move_get_score(move)));
    const double *type = &second[(ptrdiff_t)move_idx * stride];
    assert(type[0] + type[1] + type[2] == 1.0);
    assert((type[1] == 1.0) == (move_get_type(move) == GAME_EVENT_EXCHANGE));
  }
  // Premium access: the per-square masks count as the direct loop does.
  play_feature_prepare(PLAY_FEATURE_PREMIUM_ACCESS, &ctx);
  assert(ctx.premium_masks_ready);
  play_feature_extract(PLAY_FEATURE_PREMIUM_ACCESS, &ctx, moves, stride, first);
  ctx.premium_masks_ready = false;
  play_feature_extract(PLAY_FEATURE_PREMIUM_ACCESS, &ctx, moves, stride,
                       second);
  for (int move_idx = 0; move_idx < num_moves; move_idx++) {
    assert(first[(ptrdiff_t)move_idx * stride] ==
           second[(ptrdiff_t)move_idx * stride]);
    assert(first[move_idx * stride + 1] == second[move_idx * stride + 1]);
  }
  free(first);
  free(second);

  // Feature sets.
  bool active[NUMBER_OF_PLAY_FEATURES];
  ErrorStack *error_stack = error_stack_create();
  play_feature_parse_set(PLAY_FEATURE_DEFAULT_SET, active, error_stack);
  assert(error_stack_is_empty(error_stack));
  play_feature_parse_set("score,LEAVE", active, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(active[PLAY_FEATURE_SCORE] && active[PLAY_FEATURE_LEAVE_VALUE]);
  assert(!active[PLAY_FEATURE_EQUITY]);
  const char *const bad_sets[] = {"score,bogus", "score,score", ""};
  for (int set_idx = 0; set_idx < 3; set_idx++) {
    play_feature_parse_set(bad_sets[set_idx], active, error_stack);
    assert(error_stack_top(error_stack) == ERROR_STATUS_LAMC_BAD_FEATURE_SET);
    error_stack_reset(error_stack);
  }
  error_stack_destroy(error_stack);
  assert(get_config_exec_status(config, "set -lamcfeatures nope") ==
         ERROR_STATUS_LAMC_BAD_FEATURE_SET);
  assert(get_config_exec_status(config, "set -lamcproposal nope") ==
         ERROR_STATUS_LAMC_BAD_PROPOSAL);
  assert(get_config_exec_status(config, "set -lamckernel nope") ==
         ERROR_STATUS_LAMC_BAD_KERNEL);

  move_list_destroy(moves);
  game_destroy(game_before);
  config_destroy(config);
}

static uint64_t lamc_test_bits(const double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

// A fingerprint of a LAMC sim's results.
static uint64_t lamc_test_sim(const int num_threads, const char *extra,
                              LamcTotals *totals_out) {
  char *settings = get_formatted_string(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 6 -plies 2 -threads %d -iterations 1200 "
      "-minplayiterations 10 -scond none -tlim 0 -seed 42 -lamc true "
      "-lamcpool 8 -lamckeep 2 -lamcshallow 3 -lamcexplore 0.4 -lamcmaxbag "
      "1000 "
      "-savesettings false -hr false %s",
      num_threads, extra);
  Config *config = config_create_or_die(settings);
  free(settings);
  load_and_exec_config_or_die(config, "cgp " JOSH2_CGP);
  load_and_exec_config_or_die(config, "gen");
  lamc_totals_reset();
  load_and_exec_config_or_die(config, "sim");
  const LamcTotals totals = lamc_get_totals();
  assert(totals.searches == 1);
  assert(totals.rounds >= 1);
  assert(totals.evaluated >= 8);
  // The search spends its share, give or take half a round.
  assert(totals.explore_rollouts > 0);
  assert(totals.explore_rollouts <= 480 + 8 * 3 * 7);
  const SimResults *sim_results = config_get_sim_results(config);
  uint64_t hash = FNV_64_OFFSET_BASIS;
  const int num_plays = sim_results_get_number_of_plays(sim_results);
  assert(num_plays == 6);
  // BAI gets what the search leaves of -iterations, give or take a pass
  // over the plays.
  const uint64_t spent =
      sim_results_get_iteration_count(sim_results) + totals.explore_rollouts;
  assert(spent >= 1200 && spent <= 1200 + 2 * (uint64_t)num_plays);
  if (totals_out) {
    *totals_out = totals;
  }
  for (int play_idx = 0; play_idx < num_plays; play_idx++) {
    const SimmedPlay *simmed_play =
        sim_results_get_simmed_play(sim_results, play_idx);
    hash = fnv64a_step(
        hash, stat_get_num_samples(simmed_play_get_equity_stat(simmed_play)));
    hash = fnv64a_step(
        hash, lamc_test_bits(simmed_play_get_exact_win_pct_mean(simmed_play)));
  }
  hash =
      fnv64a_step(hash, (uint64_t)sim_results_get_best_move_index(sim_results));
  hash = fnv64a_step(hash, sim_results_get_iteration_count(sim_results));
  hash = fnv64a_step(hash, totals.explore_rollouts);
  config_destroy(config);
  return hash;
}

static void test_lamc_sim(void) {
  const char *const variants[] = {
      "", "-lamcproposal acquire", "-lamccycle true -sbagcycle false",
      ("-lamcreuse true -lamcequitymean true -lamcfinalall true "
       "-lamcfinalstatic 3")};
  for (int variant_idx = 0; variant_idx < 4; variant_idx++) {
    const uint64_t one = lamc_test_sim(1, variants[variant_idx], NULL);
    const uint64_t several = lamc_test_sim(4, variants[variant_idx], NULL);
    assert(one == several);
  }
  // With places to fill by score, refinement spends the search's share
  // (480 samples, give or take half a round of 8 x 3); without it the
  // search stops after its rounds.
  LamcTotals refined;
  (void)lamc_test_sim(1, "-lamcfinalstatic 3", &refined);
  assert(refined.explore_rollouts >= 480 - 12);
  LamcTotals unrefined;
  (void)lamc_test_sim(1, "-lamcfinalstatic 3 -lamcrefine false", &unrefined);
  assert(unrefined.rounds <= LAMC_DEFAULT_MAX_ROUNDS);
  assert(unrefined.explore_rollouts <= unrefined.rounds * 8 * 3);
  assert(unrefined.explore_rollouts < refined.explore_rollouts);
}

// A planted answer: in recorded rollouts where every exchange beats every
// tile placement, though exchanges rank low by static equity, the search must
// hand on an exchange beyond the top static plays. Values fall with static
// equity rank, with noise of a few win%.
static void test_lamc_finds_planted_exchanges(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 10 -plies 2 -threads 1 -iterations 1000 -scond none "
      "-tlim 0 -seed 42 -sinfer false -lamc true -savesettings false "
      "-hr false");
  load_and_exec_config_or_die(config, "cgp " JOSH2_CGP);
  load_and_exec_config_or_die(config, "gen");
  SimArgs sim_args;
  config_fill_sim_args(config, NULL, NULL, NULL, NULL, &sim_args);
  LamcCtx *ctx = lamc_ctx_create();
  const int num_plays = lamc_build(ctx, &sim_args);
  const MoveList *universe = lamc_get_universe(ctx);
  double *samples =
      malloc_or_die(sizeof(double) * num_plays * LAMC_TEST_PLANTED_SAMPLES);
  uint64_t noise_state = 12345;
  int best_exchange_rank = num_plays;
  for (int play_idx = 0; play_idx < num_plays; play_idx++) {
    const bool exchange =
        move_get_type(move_list_get_move(universe, play_idx)) ==
        GAME_EVENT_EXCHANGE;
    if (exchange && play_idx < best_exchange_rank) {
      best_exchange_rank = play_idx;
    }
    const double mean =
        exchange ? 0.8 : 0.5 - 0.3 * (double)play_idx / (double)num_plays;
    for (int sample_idx = 0; sample_idx < LAMC_TEST_PLANTED_SAMPLES;
         sample_idx++) {
      const double noise = (lamc_test_uniform(&noise_state) - 0.5) * 0.1;
      samples[play_idx * LAMC_TEST_PLANTED_SAMPLES + sample_idx] = mean + noise;
    }
  }
  // Beyond the top static plays, which the candidates hold anyway.
  assert(best_exchange_rank >= sim_args.lamc_args.static_finalists);
  const RandomVariablesArgs table_args = {
      .type = RANDOM_VARIABLES_TABLE,
      .num_rvs = num_plays,
      .num_samples = LAMC_TEST_PLANTED_SAMPLES,
      .samples = samples,
  };
  RandomVariables *table = rvs_create(&table_args);
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  const LamcSearchLimits limits = {
      .rollout_budget = 2000,
      .time_budget_seconds = 0,
      .rollouts_per_sample = 1,
      .num_threads = 1,
      .parent_worker_thread_index = 0,
      .thread_control = config_get_thread_control(config),
      .num_finalists = LAMC_TEST_PLANTED_FINALISTS,
  };
  lamc_search(ctx, &sim_args.lamc_args, table, &limits);
  const int count = lamc_select_candidates(ctx, &sim_args.lamc_args,
                                           LAMC_TEST_PLANTED_FINALISTS);
  int exchanges = 0;
  for (int rank = 0; rank < count; rank++) {
    exchanges +=
        move_get_type(move_list_get_move(
            universe, lamc_get_candidate(ctx, rank))) == GAME_EVENT_EXCHANGE;
  }
  assert(exchanges > 0);
  rvs_destroy(table);
  free(samples);
  lamc_ctx_destroy(ctx);
  config_destroy(config);
}

// A planted search over JOSH2's plays: each play's samples are its mean
// (from mean_of, by the play, its static rank and the universe's size) plus
// noise of a few win%. Returns the context after a search of 2000 samples;
// the caller destroys it and the config.
typedef double (*lamc_test_planted_mean_t)(const Move *move, int rank,
                                           int num_plays, int static_finalists);

static LamcCtx *lamc_test_planted_search(const lamc_test_planted_mean_t mean_of,
                                         Config **config_out,
                                         SimArgs *sim_args) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 10 -plies 2 -threads 1 -iterations 1000 -scond none "
      "-tlim 0 -seed 42 -sinfer false -lamc true -savesettings false "
      "-hr false");
  load_and_exec_config_or_die(config, "cgp " JOSH2_CGP);
  load_and_exec_config_or_die(config, "gen");
  config_fill_sim_args(config, NULL, NULL, NULL, NULL, sim_args);
  LamcCtx *ctx = lamc_ctx_create();
  const int num_plays = lamc_build(ctx, sim_args);
  const MoveList *universe = lamc_get_universe(ctx);
  double *samples =
      malloc_or_die(sizeof(double) * num_plays * LAMC_TEST_PLANTED_SAMPLES);
  uint64_t noise_state = 12345;
  for (int play_idx = 0; play_idx < num_plays; play_idx++) {
    const double mean =
        mean_of(move_list_get_move(universe, play_idx), play_idx, num_plays,
                sim_args->lamc_args.static_finalists);
    for (int sample_idx = 0; sample_idx < LAMC_TEST_PLANTED_SAMPLES;
         sample_idx++) {
      const double noise = (lamc_test_uniform(&noise_state) - 0.5) * 0.1;
      samples[play_idx * LAMC_TEST_PLANTED_SAMPLES + sample_idx] = mean + noise;
    }
  }
  const RandomVariablesArgs table_args = {
      .type = RANDOM_VARIABLES_TABLE,
      .num_rvs = num_plays,
      .num_samples = LAMC_TEST_PLANTED_SAMPLES,
      .samples = samples,
  };
  RandomVariables *table = rvs_create(&table_args);
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  const LamcSearchLimits limits = {
      .rollout_budget = 2000,
      .time_budget_seconds = 0,
      .rollouts_per_sample = 1,
      .num_threads = 1,
      .parent_worker_thread_index = 0,
      .thread_control = config_get_thread_control(config),
      .num_finalists = LAMC_TEST_PLANTED_FINALISTS,
  };
  lamc_search(ctx, &sim_args->lamc_args, table, &limits);
  rvs_destroy(table);
  free(samples);
  *config_out = config;
  return ctx;
}

static double lamc_test_rising_mean(const Move __attribute__((unused)) * move,
                                    const int rank, const int num_plays,
                                    const int
                                    __attribute__((unused)) static_finalists) {
  return 0.2 + 0.3 * (double)rank / (double)num_plays;
}

// Where the plays worse by static equity sample better, the prior line in
// static equity is flat, not falling: a falling line, which noise also
// gives a flat or decided surface, sends acquisition to the bottom of the
// static order.
static void test_lamc_prior_slope_not_negative(void) {
  Config *config = NULL;
  SimArgs sim_args;
  LamcCtx *ctx =
      lamc_test_planted_search(lamc_test_rising_mean, &config, &sim_args);
  assert(lamc_get_num_evaluated(ctx) > 0);
  assert(lamc_get_prior_slope(ctx) >= 0.0);
  lamc_ctx_destroy(ctx);
  config_destroy(config);
}

static double lamc_test_low_static_mean(const Move __attribute__((unused)) *
                                            move,
                                        const int rank, const int num_plays,
                                        const int static_finalists) {
  if (rank < static_finalists) {
    return 0.05;
  }
  return 0.5 - 0.3 * (double)rank / (double)num_plays;
}

// Refinement aims at the cut the candidates are chosen at: the score of the
// last candidate chosen by score, after the places reserved for the top
// static plays, which here sample worst and so would not be chosen by score.
static void test_lamc_refine_cut(void) {
  Config *config = NULL;
  SimArgs sim_args;
  LamcCtx *ctx =
      lamc_test_planted_search(lamc_test_low_static_mean, &config, &sim_args);
  const int count = LAMC_TEST_PLANTED_FINALISTS;
  assert(count > sim_args.lamc_args.static_finalists);
  const int num_static = sim_args.lamc_args.static_finalists;
  assert(sim_args.lamc_args.final_z <= 0.0);
  assert(sim_args.lamc_args.final_exchanges == 0);
  double cut = 0.0;
  assert(lamc_get_refine_cut(ctx, &sim_args.lamc_args, count, &cut));
  // The (count - num_static)-th best predicted mean past the static plays,
  // which here are none of the plays chosen by score.
  const int num_plays = move_list_get_count(lamc_get_universe(ctx));
  int above = 0;
  int at_or_above = 0;
  for (int play_idx = num_static; play_idx < num_plays; play_idx++) {
    const double mean = lamc_get_predicted_mean(ctx, play_idx);
    above += mean > cut + 1e-12;
    at_or_above += mean >= cut - 1e-12;
  }
  assert(above < count - num_static);
  assert(at_or_above >= count - num_static);
  for (int play_idx = 0; play_idx < num_static; play_idx++) {
    assert(lamc_get_predicted_mean(ctx, play_idx) < cut);
  }
  // The top static plays are candidates though they sample worst, first,
  // in static order.
  assert(lamc_select_candidates(ctx, &sim_args.lamc_args, count) == count);
  for (int rank = 0; rank < num_static; rank++) {
    assert(lamc_get_candidate(ctx, rank) == rank);
  }
  lamc_ctx_destroy(ctx);
  config_destroy(config);
}

// The pass's equity is a sentinel below every floor, yet it is a play the
// search must be able to hand on, also when -lamcuniverse caps the plays
// generated and the pass, generated last, falls off the full list.
static void lamc_test_universe_pass(const char *extra, const int max_plays) {
  char *settings = get_formatted_string(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 10 -plies 2 -threads 1 -iterations 1000 -scond none "
      "-tlim 0 -seed 42 -sinfer false -lamc true -savesettings false "
      "-hr false %s",
      extra);
  Config *config = config_create_or_die(settings);
  free(settings);
  load_and_exec_config_or_die(config, "cgp " JOSH2_CGP);
  load_and_exec_config_or_die(config, "gen");
  SimArgs sim_args;
  config_fill_sim_args(config, NULL, NULL, NULL, NULL, &sim_args);
  LamcCtx *ctx = lamc_ctx_create();
  const int num_plays = lamc_build(ctx, &sim_args);
  assert(num_plays <= max_plays);
  const MoveList *universe = lamc_get_universe(ctx);
  int passes = 0;
  for (int play_idx = 0; play_idx < num_plays; play_idx++) {
    passes += move_get_type(move_list_get_move(universe, play_idx)) ==
              GAME_EVENT_PASS;
  }
  assert(passes == 1);
  lamc_ctx_destroy(ctx);
  config_destroy(config);
}

static void test_lamc_universe_keeps_pass(void) {
  lamc_test_universe_pass("", 100000);
  // Every play within the floor, and fewer places than plays.
  lamc_test_universe_pass("-lamcuniverse 50 -lamcfloor 1000", 50);
  // The floor below the cap.
  lamc_test_universe_pass("-lamcuniverse 400 -lamcfloor 5", 400);
}

// A search stopped during its first round, as a stop request or a time
// limit that runs out stops it: a play cut off before its first sample has
// nothing to fit, so it must not count as evaluated. A stop already pending
// cuts off every play.
static void test_lamc_time_cut_round(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 10 -plies 2 -threads 1 -iterations 1000 -scond none "
      "-tlim 0 -seed 42 -sinfer false -lamc true -savesettings false "
      "-hr false");
  load_and_exec_config_or_die(config, "cgp " JOSH2_CGP);
  load_and_exec_config_or_die(config, "gen");
  SimArgs sim_args;
  config_fill_sim_args(config, NULL, NULL, NULL, NULL, &sim_args);
  LamcCtx *ctx = lamc_ctx_create();
  const int num_plays = lamc_build(ctx, &sim_args);
  assert(num_plays > sim_args.lamc_args.pool_size);
  double *samples =
      malloc_or_die(sizeof(double) * num_plays * LAMC_TEST_TIME_CUT_SAMPLES);
  for (int sample_idx = 0; sample_idx < num_plays * LAMC_TEST_TIME_CUT_SAMPLES;
       sample_idx++) {
    samples[sample_idx] = 0.5;
  }
  const RandomVariablesArgs table_args = {
      .type = RANDOM_VARIABLES_TABLE,
      .num_rvs = num_plays,
      .num_samples = LAMC_TEST_TIME_CUT_SAMPLES,
      .samples = samples,
  };
  RandomVariables *table = rvs_create(&table_args);
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_USER_INTERRUPT);
  const LamcSearchLimits limits = {
      .rollout_budget = UINT64_MAX,
      .time_budget_seconds = 0,
      .rollouts_per_sample = 1,
      .num_threads = 1,
      .parent_worker_thread_index = 0,
      .thread_control = config_get_thread_control(config),
  };
  lamc_search(ctx, &sim_args.lamc_args, table, &limits);
  const int num_evaluated = lamc_get_num_evaluated(ctx);
  assert(num_evaluated < sim_args.lamc_args.pool_size);
  for (int rank = 0; rank < num_evaluated; rank++) {
    assert(lamc_get_play_samples(ctx, lamc_get_ranked_play(ctx, rank)) > 0);
  }
  rvs_destroy(table);
  free(samples);
  lamc_ctx_destroy(ctx);
  config_destroy(config);
}

// The plays the sim simulates after a LAMC search: its 6 candidates and the
// play snoprune protects, the gen list's last, which the search does not
// pick. Each play once, and the protected one never pruned: BAI tops it up
// to the winner's rollouts. With reuse, the sim resumes from the search's
// samples.
static void test_lamc_sim_keeps_protected_play(const bool reuse) {
  char *settings = get_formatted_string(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 30 -plies 2 -threads 1 -iterations 1200 "
      "-minplayiterations 10 -scond none -tlim 0 -seed 42 -lamc true "
      "-lamcpool 8 -lamckeep 2 -lamcshallow 3 -lamcexplore 0.4 -lamcmaxbag "
      "1000 -lamcfinal 6 -lamcfinalstatic 2 -lamcreuse %s -savesettings "
      "false -hr false",
      reuse ? "true" : "false");
  Config *config = config_create_or_die(settings);
  free(settings);
  load_and_exec_config_or_die(config, "cgp " JOSH2_CGP);
  load_and_exec_config_or_die(config, "gen");
  const MoveList *gen_list = config_get_move_list(config);
  const Move *last = move_list_get_move(gen_list, 29);
  StringBuilder *protected_string = string_builder_create();
  string_builder_add_move_description(protected_string, last,
                                      config_get_ld(config));
  char *command = get_formatted_string("snoprune -, %s",
                                       string_builder_peek(protected_string));
  string_builder_destroy(protected_string);
  Move protected_play;
  move_copy(&protected_play, last);
  load_and_exec_config_or_die(config, command);
  free(command);

  const SimResults *sim_results = config_get_sim_results(config);
  const int num_plays = sim_results_get_number_of_plays(sim_results);
  assert(num_plays == 7);
  const int winner_arm =
      bai_result_get_best_arm(sim_results_get_bai_result(sim_results));
  const uint64_t winner_count =
      stat_get_num_samples(simmed_play_get_equity_stat(
          sim_results_get_simmed_play(sim_results, winner_arm)));
  int protected_found = 0;
  for (int play_idx = 0; play_idx < num_plays; play_idx++) {
    const SimmedPlay *simmed_play =
        sim_results_get_simmed_play(sim_results, play_idx);
    const Move *move = simmed_play_get_move(simmed_play);
    for (int other_idx = play_idx + 1; other_idx < num_plays; other_idx++) {
      assert(compare_moves_without_equity(
                 move,
                 simmed_play_get_move(
                     sim_results_get_simmed_play(sim_results, other_idx)),
                 true) != -1);
    }
    if (compare_moves_without_equity(move, &protected_play, true) == -1) {
      protected_found++;
      const uint64_t protected_count =
          stat_get_num_samples(simmed_play_get_equity_stat(simmed_play));
      // Resumed from the search, the winner's count includes the search's
      // samples of it, and BAI tops up to what the winner took from BAI.
      assert(reuse ? protected_count >= 10 : protected_count == winner_count);
    }
  }
  assert(protected_found == 1);
  config_destroy(config);
}

// A two-stage recursive sim with LAMC: the search picks the first stage's
// plays, and the second stage simulates the first's finalists, with no
// second search.
static void test_lamc_two_stage(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 6 -plies 2 -threads 1 -iterations 1200 "
      "-minplayiterations 10 -scond none -tlim 0 -seed 42 -lamc true "
      "-lamcpool 8 -lamckeep 2 -lamcshallow 3 -lamcexplore 0.4 -lamcmaxbag "
      "1000 -sinfer false -rlamc plies=0,lamc=1,m=2,finalists=3 "
      "-savesettings false -hr false");
  load_and_exec_config_or_die(config, "cgp " JOSH2_CGP);
  load_and_exec_config_or_die(config, "gen");
  lamc_totals_reset();
  load_and_exec_config_or_die(config, "sim");
  assert(lamc_get_totals().searches == 1);
  assert(sim_results_get_number_of_plays(config_get_sim_results(config)) == 3);
  config_destroy(config);
}

void test_lamc(void) {
  test_dense_linalg();
  test_table_and_subset();
  test_gp_kernel(GP_KERNEL_MATERN52);
  test_gp_kernel(GP_KERNEL_SQUARED_EXPONENTIAL);
  test_gp_fit_by_likelihood();
  test_gp_bounds();
  test_play_features();
  test_play_feature_pass_with_empty_bag();
  test_play_feature_bag_after_unknown_rack();
  test_play_feature_premium_leaves_covered_squares();
  test_lamc_time_cut_round();
  test_lamc_prior_slope_not_negative();
  test_lamc_refine_cut();
  test_lamc_universe_keeps_pass();
  test_lamc_max_bag_counts_real_bag();
  test_lamc_finds_planted_exchanges();
  test_lamc_sim_keeps_protected_play(false);
  test_lamc_sim_keeps_protected_play(true);
  test_lamc_two_stage();
  test_lamc_sim();
}

// The replay: rollouts of every play in a set of positions are recorded once,
// then the standard sim and LAMC variants choose plays from them at equal
// budgets. Each play's first half of samples is what the methods draw from;
// the second half, never drawn, estimates its true win percentage, so the
// methods' regret can be compared without the noise of their own samples.
//
// Settings come from the environment:
//   LAMC_REPLAY_POSITIONS  positions (default 96)
//   LAMC_REPLAY_BUDGETS    comma-separated rollout budgets (default 1000)
//   LAMC_REPLAY_PLAYS      plays the standard sim takes (default 15)
//   LAMC_REPLAY_UNIVERSE   plays recorded per position (default 100)
//   LAMC_REPLAY_SAMPLES    samples per play for the methods (default 150)
//   LAMC_REPLAY_THREADS    threads (default: the cores)
//   LAMC_REPLAY_STAGE      late for 1 to 20 tiles in the bag (default: mid)
//   LAMC_REPLAY_REUSE      1 for final choices that count the search's samples
//   LAMC_REPLAY_FIRST_GAME games to skip, for a fresh set of positions
//   LAMC_REPLAY_VARIANTS   |-separated LAMC option lists, each one variant

typedef struct ReplayPosition {
  Game *game;
  int num_plays;
  // num_plays x samples_per_play: what the methods draw.
  double *samples;
  double *truth;
} ReplayPosition;

typedef struct ReplayShared {
  ReplayPosition *positions;
  int num_positions;
  int samples_per_play;
  int baseline_plays;
  int num_threads;
  // Whether the variant's final choice counts the search's samples too.
  bool reuse_samples;
  SimArgs base_sim_args;
  ThreadControl *thread_control;
  // The variant being evaluated, and the budget.
  const SimArgs *variant_args;
  uint64_t budget;
  // Per position: regrets and diagnostics of the standard sim and the variant.
  double *standard_regret;
  double *standard_set_regret;
  double *variant_regret;
  double *variant_set_regret;
  int *variant_outside;
  int *variant_rounds;
  int *variant_evaluated;
  double *surrogate_correlation;
  double *static_correlation;
} ReplayShared;

typedef struct ReplayWorker {
  ReplayShared *shared;
  int worker_idx;
} ReplayWorker;

static double replay_true_best(const ReplayPosition *position) {
  double best = -INFINITY;
  for (int play_idx = 0; play_idx < position->num_plays; play_idx++) {
    if (position->truth[play_idx] > best) {
      best = position->truth[play_idx];
    }
  }
  return best;
}

static void *replay_record_worker(void *data) {
  const ReplayWorker *worker = (const ReplayWorker *)data;
  ReplayShared *shared = worker->shared;
  LamcCtx *ctx = lamc_ctx_create();
  SimResults *sim_results = sim_results_create(0.0);
  RandomVariables *rvs = NULL;
  const int recorded = shared->samples_per_play * 2;
  for (int pos_idx = worker->worker_idx; pos_idx < shared->num_positions;
       pos_idx += shared->num_threads) {
    ReplayPosition *position = &shared->positions[pos_idx];
    SimArgs sim_args = shared->base_sim_args;
    sim_args.game = position->game;
    sim_args.num_threads = 1;
    sim_args.bai_options.num_threads = 1;
    sim_args.bai_options.parent_worker_thread_index = worker->worker_idx;
    sim_args.seed = 1000003ULL * (uint64_t)(pos_idx + 1);
    position->num_plays = lamc_build(ctx, &sim_args);
    sim_args.move_list = lamc_get_universe(ctx);
    sim_args.num_plays = position->num_plays;
    const RandomVariablesArgs rvs_args = {
        .type = RANDOM_VARIABLES_SIMMED_PLAYS,
        .sim_args = &sim_args,
        .sim_results = sim_results,
    };
    if (rvs) {
      rvs_reset(rvs, &rvs_args);
    } else {
      rvs = rvs_create(&rvs_args);
    }
    const int num_plays = position->num_plays;
    double *all = malloc_or_die(sizeof(double) * num_plays * recorded);
    for (int sample_idx = 0; sample_idx < recorded; sample_idx++) {
      for (int play_idx = 0; play_idx < num_plays; play_idx++) {
        all[play_idx * recorded + sample_idx] =
            rvs_sample(rvs, (uint64_t)play_idx, worker->worker_idx, NULL);
      }
    }
    position->samples =
        malloc_or_die(sizeof(double) * num_plays * shared->samples_per_play);
    position->truth = malloc_or_die(sizeof(double) * num_plays);
    for (int play_idx = 0; play_idx < num_plays; play_idx++) {
      double truth_sum = 0;
      for (int sample_idx = 0; sample_idx < recorded; sample_idx++) {
        const double value = all[play_idx * recorded + sample_idx];
        if (sample_idx < shared->samples_per_play) {
          position->samples[play_idx * shared->samples_per_play + sample_idx] =
              value;
        } else {
          truth_sum += value;
        }
      }
      position->truth[play_idx] = truth_sum / shared->samples_per_play;
    }
    free(all);
  }
  rvs_destroy(rvs);
  sim_results_destroy(sim_results);
  lamc_ctx_destroy(ctx);
  return NULL;
}

// Draws count samples of each listed play and returns the index (into
// plays) of the best mean.
static int replay_choose(RandomVariables *table, const int *plays,
                         const int num_plays, const uint64_t count) {
  int best = 0;
  double best_mean = -INFINITY;
  for (int play_idx = 0; play_idx < num_plays; play_idx++) {
    double sum = 0;
    for (uint64_t sample = 0; sample < count; sample++) {
      sum += rvs_sample(table, (uint64_t)plays[play_idx], 0, NULL);
    }
    const double mean = count > 0 ? sum / (double)count : 0.0;
    if (mean > best_mean) {
      best_mean = mean;
      best = play_idx;
    }
  }
  return best;
}

// Like replay_choose, but each play's mean also counts the samples the search
// already took of it, which come first in its row of the table.
static int replay_choose_reusing(const ReplayPosition *position,
                                 const int samples_per_play,
                                 RandomVariables *table, const LamcCtx *ctx,
                                 const int *plays, const int num_plays,
                                 const uint64_t count) {
  int best = 0;
  double best_mean = -INFINITY;
  for (int play_idx = 0; play_idx < num_plays; play_idx++) {
    const uint64_t taken = lamc_get_play_samples(ctx, plays[play_idx]);
    for (uint64_t sample = 0; sample < count; sample++) {
      rvs_sample(table, (uint64_t)plays[play_idx], 0, NULL);
    }
    const uint64_t total = taken + count;
    double sum = 0;
    for (uint64_t sample = 0; sample < total; sample++) {
      sum +=
          position
              ->samples[(uint64_t)plays[play_idx] * samples_per_play + sample];
    }
    const double mean = total > 0 ? sum / (double)total : 0.0;
    if (mean > best_mean) {
      best_mean = mean;
      best = play_idx;
    }
  }
  return best;
}

// The rank of each value among values, 0 for the smallest, ties broken by
// index.
static void replay_ranks(const double *values, const int count, double *ranks) {
  for (int value_idx = 0; value_idx < count; value_idx++) {
    int rank = 0;
    for (int other = 0; other < count; other++) {
      if (values[other] < values[value_idx] ||
          (other < value_idx && values[other] == values[value_idx])) {
        rank++;
      }
    }
    ranks[value_idx] = rank;
  }
}

static double replay_correlation(const double *first_values,
                                 const double *second_values, const int count) {
  double mean_a = 0;
  double mean_b = 0;
  for (int value_idx = 0; value_idx < count; value_idx++) {
    mean_a += first_values[value_idx];
    mean_b += second_values[value_idx];
  }
  mean_a /= count;
  mean_b /= count;
  double covariance = 0;
  double variance_a = 0;
  double variance_b = 0;
  for (int value_idx = 0; value_idx < count; value_idx++) {
    covariance += (first_values[value_idx] - mean_a) *
                  (second_values[value_idx] - mean_b);
    variance_a +=
        (first_values[value_idx] - mean_a) * (first_values[value_idx] - mean_a);
    variance_b += (second_values[value_idx] - mean_b) *
                  (second_values[value_idx] - mean_b);
  }
  return variance_a > 0 && variance_b > 0
             ? covariance / sqrt(variance_a * variance_b)
             : 0.0;
}

// The Spearman correlations with the plays' true win percentages of the
// surrogate's means and of static equity (the universe's order).
static void replay_rank_correlations(const ReplayPosition *position,
                                     LamcCtx *ctx, double *surrogate,
                                     double *static_equity) {
  const int count = position->num_plays;
  double *values = malloc_or_die(sizeof(double) * count);
  double *truth_ranks = malloc_or_die(sizeof(double) * count);
  double *other_ranks = malloc_or_die(sizeof(double) * count);
  replay_ranks(position->truth, count, truth_ranks);
  for (int play_idx = 0; play_idx < count; play_idx++) {
    values[play_idx] = lamc_get_predicted_mean(ctx, play_idx);
  }
  replay_ranks(values, count, other_ranks);
  *surrogate = replay_correlation(truth_ranks, other_ranks, count);
  for (int play_idx = 0; play_idx < count; play_idx++) {
    values[play_idx] = -play_idx;
  }
  replay_ranks(values, count, other_ranks);
  *static_equity = replay_correlation(truth_ranks, other_ranks, count);
  free(values);
  free(truth_ranks);
  free(other_ranks);
}

static double replay_set_regret(const ReplayPosition *position,
                                const int *plays, const int num_plays) {
  double best = -INFINITY;
  for (int play_idx = 0; play_idx < num_plays; play_idx++) {
    if (position->truth[plays[play_idx]] > best) {
      best = position->truth[plays[play_idx]];
    }
  }
  return replay_true_best(position) - best;
}

static void *replay_evaluate_worker(void *data) {
  const ReplayWorker *worker = (const ReplayWorker *)data;
  ReplayShared *shared = worker->shared;
  LamcCtx *ctx = lamc_ctx_create();
  int plays[LAMC_MAX_POOL_SIZE] = {0};
  for (int pos_idx = worker->worker_idx; pos_idx < shared->num_positions;
       pos_idx += shared->num_threads) {
    const ReplayPosition *position = &shared->positions[pos_idx];
    const RandomVariablesArgs table_args = {
        .type = RANDOM_VARIABLES_TABLE,
        .num_rvs = (uint64_t)position->num_plays,
        .num_samples = (uint64_t)shared->samples_per_play,
        .samples = position->samples,
    };
    RandomVariables *table = rvs_create(&table_args);
    const double true_best = replay_true_best(position);
    int baseline = shared->baseline_plays < position->num_plays
                       ? shared->baseline_plays
                       : position->num_plays;

    // The standard sim: the top plays by static equity, sampled evenly.
    for (int play_idx = 0; play_idx < baseline; play_idx++) {
      plays[play_idx] = play_idx;
    }
    const int standard_choice =
        replay_choose(table, plays, baseline, shared->budget / baseline);
    shared->standard_regret[pos_idx] =
        true_best - position->truth[plays[standard_choice]];
    shared->standard_set_regret[pos_idx] =
        replay_set_regret(position, plays, baseline);

    // The variant: a search, then its candidates sampled evenly with the rest.
    rvs_reset(table, &table_args);
    SimArgs sim_args = *shared->variant_args;
    sim_args.game = position->game;
    const int built = lamc_build(ctx, &sim_args);
    if (built != position->num_plays) {
      log_fatal("replay position %d built %d plays, recorded %d", pos_idx,
                built, position->num_plays);
    }
    const LamcArgs *args = &sim_args.lamc_args;
    const LamcSearchLimits limits = {
        .rollout_budget =
            (uint64_t)(args->explore_share * (double)shared->budget),
        .time_budget_seconds = 0,
        .rollouts_per_sample = 1,
        .num_threads = 1,
        .parent_worker_thread_index = 0,
        .thread_control = shared->thread_control,
        // As lamc_choose_candidates does, so that refinement, on by default,
        // runs.
        .num_finalists =
            args->num_finalists > 0 ? args->num_finalists : baseline,
    };
    shared->variant_rounds[pos_idx] = lamc_search(ctx, args, table, &limits);
    const int num_evaluated = lamc_get_num_evaluated(ctx);
    shared->variant_evaluated[pos_idx] = num_evaluated;
    replay_rank_correlations(position, ctx,
                             &shared->surrogate_correlation[pos_idx],
                             &shared->static_correlation[pos_idx]);
    const int wanted = args->num_finalists > 0 ? args->num_finalists : baseline;
    const int num_finalists = lamc_select_candidates(ctx, args, wanted);
    for (int rank = 0; rank < num_finalists; rank++) {
      plays[rank] = lamc_get_candidate(ctx, rank);
    }
    const uint64_t spent = lamc_get_search_rollouts(ctx);
    const uint64_t left = shared->budget > spent ? shared->budget - spent : 0;
    const int variant_choice =
        shared->reuse_samples
            ? replay_choose_reusing(position, shared->samples_per_play, table,
                                    ctx, plays, num_finalists,
                                    left / num_finalists)
            : replay_choose(table, plays, num_finalists, left / num_finalists);
    shared->variant_regret[pos_idx] =
        true_best - position->truth[plays[variant_choice]];
    shared->variant_set_regret[pos_idx] =
        replay_set_regret(position, plays, num_finalists);
    shared->variant_outside[pos_idx] = plays[variant_choice] >= baseline;
    rvs_destroy(table);
  }
  lamc_ctx_destroy(ctx);
  return NULL;
}

static void replay_run_workers(ReplayShared *shared, void *(*func)(void *)) {
  cpthread_t threads[LAMC_REPLAY_MAX_THREADS];
  ReplayWorker workers[LAMC_REPLAY_MAX_THREADS];
  for (int worker_idx = 0; worker_idx < shared->num_threads; worker_idx++) {
    workers[worker_idx].shared = shared;
    workers[worker_idx].worker_idx = worker_idx;
    cpthread_create(&threads[worker_idx], func, &workers[worker_idx]);
  }
  for (int worker_idx = 0; worker_idx < shared->num_threads; worker_idx++) {
    cpthread_join(threads[worker_idx]);
  }
}

static int replay_env_int(const char *name, const int fallback) {
  const char *value = getenv(name);
  return value ? (int)strtol(value, NULL, 10) : fallback;
}

static void replay_mean_and_se(const double *values, const int count,
                               double *mean, double *se) {
  double sum = 0;
  for (int value_idx = 0; value_idx < count; value_idx++) {
    sum += values[value_idx];
  }
  *mean = sum / count;
  double squares = 0;
  for (int value_idx = 0; value_idx < count; value_idx++) {
    squares += (values[value_idx] - *mean) * (values[value_idx] - *mean);
  }
  *se = count > 1 ? sqrt(squares / (count - 1) / count) : 0.0;
}

// Positions from games of top static equity plays. Midgame positions stop
// after a number of turns that varies with the position, before the bag has
// fewer than 30 tiles; late positions stop once it has at most 1 to 20, which
// also varies.
static void replay_make_positions(const Game *base, ReplayPosition *positions,
                                  const int num_positions, const bool late,
                                  const uint64_t first_game) {
  int made = 0;
  for (uint64_t game_idx = first_game + 1; made < num_positions; game_idx++) {
    Game *game = game_duplicate(base);
    game_reset(game);
    game_seed(game, 7919ULL * game_idx);
    draw_starting_racks(game);
    const int turns = (int)(game_idx % 18);
    const int bag_target = 1 + (int)(game_idx % 20);
    bool usable = true;
    for (int turn = 0; usable; turn++) {
      const int bag = bag_get_letters(game_get_bag(game));
      if (late ? bag <= bag_target : (turn >= turns || bag < 30)) {
        break;
      }
      play_top_n_equity_move(game, 0);
      usable = !game_over(game);
    }
    if (!usable || bag_get_letters(game_get_bag(game)) == 0) {
      game_destroy(game);
      continue;
    }
    positions[made++].game = game;
  }
}

void test_lamc_replay(void) {
  const int num_positions = replay_env_int("LAMC_REPLAY_POSITIONS", 96);
  const int baseline_plays = replay_env_int("LAMC_REPLAY_PLAYS", 15);
  const int universe = replay_env_int("LAMC_REPLAY_UNIVERSE", 100);
  const int samples_per_play = replay_env_int("LAMC_REPLAY_SAMPLES", 150);
  int num_threads = replay_env_int("LAMC_REPLAY_THREADS", get_num_cores());
  if (num_threads > LAMC_REPLAY_MAX_THREADS) {
    num_threads = LAMC_REPLAY_MAX_THREADS;
  }
  const char *budgets_env = getenv("LAMC_REPLAY_BUDGETS");
  StringSplitter *budget_items =
      split_string(budgets_env ? budgets_env : "1000", ',', true);
  const char *variants_env = getenv("LAMC_REPLAY_VARIANTS");
  StringSplitter *variant_items = split_string(
      variants_env ? variants_env
                   : "|-lamcproposal acquire|-lamcfeatures score,leave,equity",
      '|', false);

  char *settings = get_formatted_string(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-plies 2 -threads 1 -sbagcycle false -sinfer false -lamc true "
      "-lamcuniverse %d -lamcfloor 1000 -savesettings false -hr false",
      universe);
  Config *config = config_create_or_die(settings);
  free(settings);
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  ErrorStack *error_stack = error_stack_create();
  config_load_win_pcts(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);

  ReplayShared shared;
  memset(&shared, 0, sizeof(shared));
  shared.num_positions = num_positions;
  shared.samples_per_play = samples_per_play;
  shared.baseline_plays = baseline_plays;
  shared.num_threads = num_threads;
  shared.reuse_samples = replay_env_int("LAMC_REPLAY_REUSE", 0) != 0;
  shared.thread_control = config_get_thread_control(config);
  shared.positions = calloc_or_die(num_positions, sizeof(ReplayPosition));
  config_fill_sim_args(config, NULL, NULL, NULL, NULL, &shared.base_sim_args);
  const char *stage = getenv("LAMC_REPLAY_STAGE");
  replay_make_positions(config_get_game(config), shared.positions,
                        num_positions, stage && strings_equal(stage, "late"),
                        (uint64_t)replay_env_int("LAMC_REPLAY_FIRST_GAME", 0));
  printf("LAMC replay: %d positions, %d plays each at most, %d samples per "
         "play, %d threads\n",
         num_positions, universe, samples_per_play, num_threads);
  (void)fflush(stdout);
  replay_run_workers(&shared, replay_record_worker);
  printf("LAMC replay: recorded\n");
  (void)fflush(stdout);

  shared.standard_regret = malloc_or_die(sizeof(double) * num_positions);
  shared.standard_set_regret = malloc_or_die(sizeof(double) * num_positions);
  shared.variant_regret = malloc_or_die(sizeof(double) * num_positions);
  shared.variant_set_regret = malloc_or_die(sizeof(double) * num_positions);
  shared.variant_outside = malloc_or_die(sizeof(int) * num_positions);
  shared.variant_rounds = malloc_or_die(sizeof(int) * num_positions);
  shared.variant_evaluated = malloc_or_die(sizeof(int) * num_positions);
  shared.surrogate_correlation = malloc_or_die(sizeof(double) * num_positions);
  shared.static_correlation = malloc_or_die(sizeof(double) * num_positions);
  double *differences = malloc_or_die(sizeof(double) * num_positions);

  const int num_budgets = string_splitter_get_number_of_items(budget_items);
  const int num_variants = string_splitter_get_number_of_items(variant_items);
  for (int budget_idx = 0; budget_idx < num_budgets; budget_idx++) {
    shared.budget = (uint64_t)strtoull(
        string_splitter_get_item(budget_items, budget_idx), NULL, 10);
    for (int variant_idx = 0; variant_idx < num_variants; variant_idx++) {
      const char *variant =
          string_splitter_get_item(variant_items, variant_idx);
      // A fresh config, so that no variant inherits another's options.
      char *variant_settings = get_formatted_string(
          "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
          "-plies 2 -threads 1 -sbagcycle false -sinfer false -lamc true "
          "-lamcuniverse %d -lamcfloor 1000 -savesettings false -hr false %s",
          universe, variant);
      Config *variant_config = config_create_or_die(variant_settings);
      free(variant_settings);
      SimArgs variant_args = shared.base_sim_args;
      SimArgs filled_args;
      config_fill_sim_args(variant_config, NULL, NULL, NULL, NULL,
                           &filled_args);
      variant_args.lamc_args = filled_args.lamc_args;
      config_destroy(variant_config);
      shared.variant_args = &variant_args;
      replay_run_workers(&shared, replay_evaluate_worker);

      double standard_mean;
      double standard_se;
      double variant_mean;
      double variant_se;
      double difference_mean;
      double difference_se;
      double standard_set_mean;
      double variant_set_mean;
      double unused_se;
      for (int pos_idx = 0; pos_idx < num_positions; pos_idx++) {
        differences[pos_idx] =
            shared.variant_regret[pos_idx] - shared.standard_regret[pos_idx];
      }
      replay_mean_and_se(shared.standard_regret, num_positions, &standard_mean,
                         &standard_se);
      replay_mean_and_se(shared.variant_regret, num_positions, &variant_mean,
                         &variant_se);
      replay_mean_and_se(differences, num_positions, &difference_mean,
                         &difference_se);
      replay_mean_and_se(shared.standard_set_regret, num_positions,
                         &standard_set_mean, &unused_se);
      replay_mean_and_se(shared.variant_set_regret, num_positions,
                         &variant_set_mean, &unused_se);
      double surrogate_mean;
      double static_mean;
      replay_mean_and_se(shared.surrogate_correlation, num_positions,
                         &surrogate_mean, &unused_se);
      replay_mean_and_se(shared.static_correlation, num_positions, &static_mean,
                         &unused_se);
      int outside = 0;
      double rounds = 0;
      double evaluated = 0;
      for (int pos_idx = 0; pos_idx < num_positions; pos_idx++) {
        outside += shared.variant_outside[pos_idx];
        rounds += shared.variant_rounds[pos_idx];
        evaluated += shared.variant_evaluated[pos_idx];
      }
      printf("LAMCREPLAY budget=%llu variant='%s' standard=%.3f(%.3f) "
             "lamc=%.3f(%.3f) diff=%+.3f(%.3f) setregret std=%.3f lamc=%.3f "
             "outside=%d rounds=%.1f evaluated=%.1f spearman lamc=%.3f "
             "static=%.3f\n",
             (unsigned long long)shared.budget, variant, 100 * standard_mean,
             100 * standard_se, 100 * variant_mean, 100 * variant_se,
             100 * difference_mean, 100 * difference_se,
             100 * standard_set_mean, 100 * variant_set_mean, outside,
             rounds / num_positions, evaluated / num_positions, surrogate_mean,
             static_mean);
      (void)fflush(stdout);
    }
  }

  for (int pos_idx = 0; pos_idx < num_positions; pos_idx++) {
    game_destroy(shared.positions[pos_idx].game);
    free(shared.positions[pos_idx].samples);
    free(shared.positions[pos_idx].truth);
  }
  free(shared.positions);
  free(shared.standard_regret);
  free(shared.standard_set_regret);
  free(shared.variant_regret);
  free(shared.variant_set_regret);
  free(shared.variant_outside);
  free(shared.variant_rounds);
  free(shared.variant_evaluated);
  free(shared.surrogate_correlation);
  free(shared.static_correlation);
  free(differences);
  string_splitter_destroy(budget_items);
  string_splitter_destroy(variant_items);
  config_destroy(config);
}

// Autoplay with LAMC, printing its totals with the results. The settings are
// LAMC_AUTOPLAY_SETTINGS (a set command's options) and LAMC_AUTOPLAY_COMMAND
// (default: autoplay games 100 -gp true -seed 1).
void test_lamc_autoplay(void) {
  const char *settings_env = getenv("LAMC_AUTOPLAY_SETTINGS");
  const char *command_env = getenv("LAMC_AUTOPLAY_COMMAND");
  char *settings = get_formatted_string(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-savesettings false -autosavegcg false %s",
      settings_env ? settings_env : "");
  Config *config = config_create_or_die(settings);
  free(settings);
  lamc_totals_reset();
  Timer timer;
  ctimer_start(&timer);
  load_and_exec_config_or_die(
      config,
      command_env ? command_env : "autoplay games 100 -gp true -seed 1");
  const double seconds = ctimer_elapsed_seconds(&timer);
  char *results = autoplay_results_to_string(
      config_get_autoplay_results(config), true, true);
  const LamcTotals totals = lamc_get_totals();
  const double searches = totals.searches > 0 ? (double)totals.searches : 1.0;
  printf("%s\nLAMCAUTOPLAY seconds=%.1f searches=%llu rounds=%.2f "
         "evaluated=%.1f search_rollouts=%.1f search_ms=%.2f "
         "finalists_outside=%.2f chosen_outside=%llu\n",
         results, seconds, (unsigned long long)totals.searches,
         (double)totals.rounds / searches, (double)totals.evaluated / searches,
         (double)totals.explore_rollouts / searches,
         (double)totals.explore_microseconds / 1000.0 / searches,
         (double)totals.finalists_outside_static / searches,
         (unsigned long long)totals.chosen_outside_static);
  free(results);
  config_destroy(config);
}
