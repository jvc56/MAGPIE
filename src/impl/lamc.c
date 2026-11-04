#include "lamc.h"

#include "../compat/ctime.h"
#include "../def/bai_defs.h"
#include "../def/equity_defs.h"
#include "../def/lamc_defs.h"
#include "../def/move_defs.h"
#include "../def/play_feature_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/bai_result.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/lamc_args.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/sim_args.h"
#include "../ent/sim_results.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include "bai.h"
#include "gp.h"
#include "move_gen.h"
#include "play_feature_registry.h"
#include "random_variable.h"
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  // Each fit chooses its length scale among this many multiples of the
  // configured one, by marginal likelihood.
  LAMC_NUM_LENGTH_SCALE_MULTIPLES = 4,
  // A sample limit above this is no limit (the config default is 10^12).
  LAMC_UNLIMITED_SAMPLES = 1000000000,
};

static const double
    lamc_length_scale_multiples[LAMC_NUM_LENGTH_SCALE_MULTIPLES] = {0.5, 1.0,
                                                                    2.0, 4.0};

// Variance floors: the surrogate's signal, and a rollout's win percentage.
#define LAMC_MIN_SIGNAL_VARIANCE 1e-6
#define LAMC_MIN_ROLLOUT_VARIANCE 1e-4
// A rollout's win percentage variance before any play has two samples.
#define LAMC_DEFAULT_ROLLOUT_VARIANCE 0.1
// Columns whose standard deviation across the plays is below this carry no
// information and are zeroed.
#define LAMC_MIN_COLUMN_STDEV 1e-9

static atomic_uint_fast64_t lamc_total_searches;
static atomic_uint_fast64_t lamc_total_rounds;
static atomic_uint_fast64_t lamc_total_evaluated;
static atomic_uint_fast64_t lamc_total_explore_rollouts;
static atomic_uint_fast64_t lamc_total_explore_microseconds;
static atomic_uint_fast64_t lamc_total_finalists_outside_static;
static atomic_uint_fast64_t lamc_total_chosen_outside_static;

struct LamcCtx {
  int universe_capacity;
  MoveList *universe;
  int num_universe;
  int finalists_capacity;
  MoveList *finalists;
  int num_threads;
  SimResults *explore_results;
  RandomVariables *explore_rvs;
  RandomVariables *pool_rvs;
  BAIResult *bai_result;
  PlayFeatureCtx feature_ctx;
  int num_columns;
  double inverse_length_scales[PLAY_EMBEDDING_MAX_COLUMNS];
  bool column_is_continuous[PLAY_EMBEDDING_MAX_COLUMNS];
  // num_universe rows of num_columns, standardized.
  double *embedding;
  // Per universe play.
  bool *is_evaluated;
  bool *is_proposed;
  // The distance to the nearest pool play while the first pool is chosen,
  // then each play's acquisition score while plays are proposed.
  double *min_distance;
  // The plays sampled this round and next, as universe indices.
  int *pool;
  int pool_count;
  int *next_pool;
  int next_pool_count;
  // Per universe play: its samples in this search (see RVSubsetStats).
  atomic_uint_least64_t *sample_counts;
  atomic_int_least64_t *sample_sums;
  atomic_int_least64_t *sample_squares;
  RVSubsetStats subset_stats;
  uint64_t rollouts_per_sample;
  int evaluated_capacity;
  int gp_num_dims;
  GaussianProcess *gp;
  // The evaluated plays, as universe indices, in the order they were first
  // sampled, and the same plays ranked by the surrogate's mean.
  int *evaluated;
  int num_evaluated;
  int *ranked;
  double *posterior_means;
  double *gp_points;
  double *gp_values;
  double *gp_noise;
  // Per universe play: its static equity, standardized across the universe,
  // its predicted mean once a search ends, and the candidates by that mean.
  double *static_equity;
  double *universe_means;
  int *candidates;
  // The prior mean of a play with standardized static equity e is
  // mean_intercept + mean_slope * e; the process fits what is left.
  double mean_intercept;
  double mean_slope;
  // Whether the last fit succeeded; if not, predictions are the prior mean.
  bool fitted;
  double gradient[PLAY_EMBEDDING_MAX_COLUMNS];
  double target[PLAY_EMBEDDING_MAX_COLUMNS];
  double fit_inverse_length_scales[PLAY_EMBEDDING_MAX_COLUMNS];
};

static void lamc_ctx_free_buffers(LamcCtx *ctx) {
  move_list_destroy(ctx->universe);
  free(ctx->embedding);
  free(ctx->is_evaluated);
  free(ctx->is_proposed);
  free(ctx->min_distance);
  free(ctx->pool);
  free(ctx->next_pool);
  free(ctx->sample_counts);
  free(ctx->sample_sums);
  free(ctx->sample_squares);
  free(ctx->static_equity);
  free(ctx->universe_means);
  free(ctx->candidates);
  ctx->static_equity = NULL;
  ctx->universe_means = NULL;
  ctx->candidates = NULL;
  ctx->sample_counts = NULL;
  ctx->sample_sums = NULL;
  ctx->sample_squares = NULL;
  ctx->universe = NULL;
  ctx->embedding = NULL;
  ctx->is_evaluated = NULL;
  ctx->is_proposed = NULL;
  ctx->min_distance = NULL;
  ctx->pool = NULL;
  ctx->next_pool = NULL;
}

static void lamc_ctx_free_surrogate(LamcCtx *ctx) {
  gp_destroy(ctx->gp);
  free(ctx->evaluated);
  free(ctx->ranked);
  free(ctx->posterior_means);
  free(ctx->gp_points);
  free(ctx->gp_values);
  free(ctx->gp_noise);
  ctx->gp = NULL;
  ctx->evaluated = NULL;
  ctx->ranked = NULL;
  ctx->posterior_means = NULL;
  ctx->gp_points = NULL;
  ctx->gp_values = NULL;
  ctx->gp_noise = NULL;
}

void lamc_ctx_destroy(LamcCtx *ctx) {
  if (!ctx) {
    return;
  }
  lamc_ctx_free_buffers(ctx);
  lamc_ctx_free_surrogate(ctx);
  move_list_destroy(ctx->finalists);
  rvs_destroy(ctx->pool_rvs);
  rvs_destroy(ctx->explore_rvs);
  sim_results_destroy(ctx->explore_results);
  bai_result_destroy(ctx->bai_result);
  free(ctx);
}

LamcCtx *lamc_ctx_create(void) {
  LamcCtx *ctx = calloc_or_die(1, sizeof(LamcCtx));
  ctx->bai_result = bai_result_create();
  ctx->explore_results = sim_results_create(0.0);
  return ctx;
}

// Sizes the buffers for the args, keeping those already large enough.
static void lamc_ctx_ensure_capacity(LamcCtx *ctx, const LamcArgs *args) {
  if (ctx->universe_capacity < args->max_universe) {
    lamc_ctx_free_buffers(ctx);
    const int capacity = args->max_universe;
    ctx->universe_capacity = capacity;
    ctx->universe = move_list_create(capacity);
    ctx->embedding = malloc_or_die(sizeof(double) * (size_t)capacity *
                                   PLAY_EMBEDDING_MAX_COLUMNS);
    ctx->is_evaluated = malloc_or_die(sizeof(bool) * capacity);
    ctx->is_proposed = malloc_or_die(sizeof(bool) * capacity);
    ctx->min_distance = malloc_or_die(sizeof(double) * capacity);
    ctx->pool = malloc_or_die(sizeof(int) * capacity);
    ctx->next_pool = malloc_or_die(sizeof(int) * capacity);
    ctx->sample_counts =
        malloc_or_die(sizeof(atomic_uint_least64_t) * capacity);
    ctx->sample_sums = malloc_or_die(sizeof(atomic_int_least64_t) * capacity);
    ctx->sample_squares =
        malloc_or_die(sizeof(atomic_int_least64_t) * capacity);
    ctx->static_equity = malloc_or_die(sizeof(double) * capacity);
    ctx->universe_means = malloc_or_die(sizeof(double) * capacity);
    ctx->candidates = malloc_or_die(sizeof(int) * capacity);
    ctx->subset_stats.counts = ctx->sample_counts;
    ctx->subset_stats.sums = ctx->sample_sums;
    ctx->subset_stats.squares = ctx->sample_squares;
  }
  if (ctx->evaluated_capacity < args->max_evaluated) {
    lamc_ctx_free_surrogate(ctx);
    const int capacity = args->max_evaluated;
    ctx->evaluated_capacity = capacity;
    ctx->gp_num_dims = 0;
    ctx->evaluated = malloc_or_die(sizeof(int) * capacity);
    ctx->ranked = malloc_or_die(sizeof(int) * capacity);
    ctx->posterior_means = malloc_or_die(sizeof(double) * capacity);
    ctx->gp_points = malloc_or_die(sizeof(double) * (size_t)capacity *
                                   PLAY_EMBEDDING_MAX_COLUMNS);
    ctx->gp_values = malloc_or_die(sizeof(double) * capacity);
    ctx->gp_noise = malloc_or_die(sizeof(double) * capacity);
  }
}

// Generates every play into the universe, best by static equity first, and
// keeps those within the equity floor of the best.
static void lamc_build_universe(LamcCtx *ctx, const SimArgs *sim_args) {
  const Game *game = sim_args->game;
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = ctx->universe,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = player_get_move_sort_type(
          game_get_player(game, game_get_player_on_turn_index(game))),
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&gen_args);
  move_list_sort_moves(ctx->universe);
  const int count = move_list_get_count(ctx->universe);
  if (count == 0) {
    ctx->num_universe = 0;
    return;
  }
  const Equity floor = move_get_equity(move_list_get_move(ctx->universe, 0)) -
                       double_to_equity(sim_args->lamc_args.equity_floor);
  int num_universe = 1;
  while (num_universe < count && move_get_equity(move_list_get_move(
                                     ctx->universe, num_universe)) >= floor) {
    num_universe++;
  }
  ctx->universe->count = num_universe;
  ctx->num_universe = num_universe;
}

static double *lamc_row(const LamcCtx *ctx, const int universe_idx) {
  return ctx->embedding + (ptrdiff_t)universe_idx * ctx->num_columns;
}

// Embeds the universe, then standardizes each continuous column across it.
static void lamc_build_embedding(LamcCtx *ctx, const SimArgs *sim_args) {
  const LamcArgs *args = &sim_args->lamc_args;
  PlayFeatureCtx *feature_ctx = &ctx->feature_ctx;
  play_feature_ctx_init(feature_ctx, sim_args->game);
  int num_columns = 0;
  for (int feature_idx = 0; feature_idx < NUMBER_OF_PLAY_FEATURES;
       feature_idx++) {
    if (args->features[feature_idx]) {
      num_columns += play_feature_get_num_columns((play_feature_t)feature_idx);
    }
  }
  ctx->num_columns = num_columns;
  int column = 0;
  for (int feature_idx = 0; feature_idx < NUMBER_OF_PLAY_FEATURES;
       feature_idx++) {
    if (!args->features[feature_idx]) {
      continue;
    }
    const play_feature_t feature = (play_feature_t)feature_idx;
    play_feature_prepare(feature, feature_ctx);
    play_feature_extract(feature, feature_ctx, ctx->universe, num_columns,
                         ctx->embedding + column);
    const bool continuous =
        play_feature_get_kind(feature) == PLAY_FEATURE_KIND_CONTINUOUS;
    const int feature_columns = play_feature_get_num_columns(feature);
    for (int offset = 0; offset < feature_columns; offset++) {
      ctx->column_is_continuous[column + offset] = continuous;
    }
    column += feature_columns;
  }
  const int num_universe = ctx->num_universe;
  for (column = 0; column < num_columns; column++) {
    ctx->inverse_length_scales[column] = 1.0 / args->length_scale;
    if (!ctx->column_is_continuous[column]) {
      continue;
    }
    double sum = 0.0;
    for (int play_idx = 0; play_idx < num_universe; play_idx++) {
      sum += lamc_row(ctx, play_idx)[column];
    }
    const double mean = sum / num_universe;
    double squares = 0.0;
    for (int play_idx = 0; play_idx < num_universe; play_idx++) {
      const double diff = lamc_row(ctx, play_idx)[column] - mean;
      squares += diff * diff;
    }
    const double stdev = sqrt(squares / num_universe);
    for (int play_idx = 0; play_idx < num_universe; play_idx++) {
      double *value = &lamc_row(ctx, play_idx)[column];
      *value = stdev < LAMC_MIN_COLUMN_STDEV ? 0.0 : (*value - mean) / stdev;
    }
  }
}

// Each play's static equity, standardized across the universe, for the
// surrogate's prior mean.
static void lamc_build_static_equity(LamcCtx *ctx) {
  const int num_universe = ctx->num_universe;
  play_feature_extract(PLAY_FEATURE_EQUITY, &ctx->feature_ctx, ctx->universe, 1,
                       ctx->static_equity);
  double sum = 0.0;
  for (int play_idx = 0; play_idx < num_universe; play_idx++) {
    sum += ctx->static_equity[play_idx];
  }
  const double mean = sum / num_universe;
  double squares = 0.0;
  for (int play_idx = 0; play_idx < num_universe; play_idx++) {
    const double diff = ctx->static_equity[play_idx] - mean;
    squares += diff * diff;
  }
  const double stdev = sqrt(squares / num_universe);
  for (int play_idx = 0; play_idx < num_universe; play_idx++) {
    ctx->static_equity[play_idx] =
        stdev < LAMC_MIN_COLUMN_STDEV
            ? 0.0
            : (ctx->static_equity[play_idx] - mean) / stdev;
  }
}

static double lamc_distance_squared(const LamcCtx *ctx, const double *a,
                                    const double *b) {
  double sum = 0.0;
  for (int column = 0; column < ctx->num_columns; column++) {
    const double diff =
        (a[column] - b[column]) * ctx->inverse_length_scales[column];
    sum += diff * diff;
  }
  return sum;
}

// The first pool: the best plays by static equity, then plays chosen one at a
// time as far as possible from those already chosen.
static void lamc_init_pool(LamcCtx *ctx, const LamcArgs *args) {
  const int num_universe = ctx->num_universe;
  int pool_size =
      args->pool_size < num_universe ? args->pool_size : num_universe;
  if (pool_size > ctx->evaluated_capacity) {
    pool_size = ctx->evaluated_capacity;
  }
  int num_seeded = args->seed_plays < pool_size ? args->seed_plays : pool_size;
  if (num_seeded < 1) {
    num_seeded = 1;
  }
  for (int play_idx = 0; play_idx < num_universe; play_idx++) {
    ctx->min_distance[play_idx] = INFINITY;
    ctx->is_proposed[play_idx] = false;
  }
  ctx->pool_count = 0;
  for (int pool_idx = 0; pool_idx < pool_size; pool_idx++) {
    int chosen = pool_idx;
    if (pool_idx >= num_seeded) {
      double farthest = -1.0;
      for (int play_idx = 0; play_idx < num_universe; play_idx++) {
        if (!ctx->is_proposed[play_idx] &&
            ctx->min_distance[play_idx] > farthest) {
          farthest = ctx->min_distance[play_idx];
          chosen = play_idx;
        }
      }
    }
    ctx->is_proposed[chosen] = true;
    ctx->pool[ctx->pool_count++] = chosen;
    const double *chosen_row = lamc_row(ctx, chosen);
    for (int play_idx = 0; play_idx < num_universe; play_idx++) {
      const double distance =
          lamc_distance_squared(ctx, chosen_row, lamc_row(ctx, play_idx));
      if (distance < ctx->min_distance[play_idx]) {
        ctx->min_distance[play_idx] = distance;
      }
    }
  }
}

// Samples every play in the pool shallow_samples more times.
static void lamc_run_round(LamcCtx *ctx, const LamcArgs *args,
                           RandomVariables *base_rvs,
                           const LamcSearchLimits *limits,
                           const double time_limit_seconds) {
  const RandomVariablesArgs pool_args = {
      .type = RANDOM_VARIABLES_SUBSET,
      .num_rvs = (uint64_t)ctx->pool_count,
      .base_rvs = base_rvs,
      .arm_indices = ctx->pool,
      .subset_stats = &ctx->subset_stats,
  };
  if (ctx->pool_rvs) {
    rvs_reset(ctx->pool_rvs, &pool_args);
  } else {
    ctx->pool_rvs = rvs_create(&pool_args);
  }
  const uint64_t samples = (uint64_t)args->shallow_samples;
  const BAIOptions options = {
      .sampling_rule = BAI_SAMPLING_RULE_ROUND_ROBIN,
      .threshold = BAI_THRESHOLD_NONE,
      .delta = 1.0,
      .sample_limit = samples * (uint64_t)ctx->pool_count,
      .sample_minimum = samples,
      .time_limit_seconds = time_limit_seconds,
      .num_threads = limits->num_threads,
      .parent_worker_thread_index = limits->parent_worker_thread_index,
      .cutoff = 0.0,
      .arm_avoid_prune = NULL,
      .num_arm_avoid_prune = 0,
      .draws_per_sample = limits->rollouts_per_sample,
  };
  bai(&options, ctx->pool_rvs, limits->thread_control, NULL, ctx->bai_result);
}

// Adds the pool's plays that are new to the evaluated plays.
static void lamc_mark_pool_evaluated(LamcCtx *ctx) {
  for (int pool_idx = 0; pool_idx < ctx->pool_count; pool_idx++) {
    const int play_idx = ctx->pool[pool_idx];
    if (!ctx->is_evaluated[play_idx]) {
      ctx->is_evaluated[play_idx] = true;
      ctx->evaluated[ctx->num_evaluated++] = play_idx;
    }
  }
}

// The surrogate's mean, and optionally its variance, at a universe play.
static void lamc_predict(LamcCtx *ctx, const int play_idx, double *mean,
                         double *variance) {
  double residual = 0.0;
  gp_predict(ctx->gp, lamc_row(ctx, play_idx), &residual, variance);
  *mean = ctx->mean_intercept + ctx->mean_slope * ctx->static_equity[play_idx] +
          residual;
}

// Fits the prior mean, constant or linear in static equity, by least squares
// weighted by each value's precision, and leaves the residuals in values.
static void lamc_fit_prior_mean(LamcCtx *ctx, const LamcArgs *args,
                                double *values) {
  const int num_evaluated = ctx->num_evaluated;
  double weight_sum = 0.0;
  double equity_sum = 0.0;
  double value_sum = 0.0;
  for (int eval_idx = 0; eval_idx < num_evaluated; eval_idx++) {
    const double weight = 1.0 / ctx->gp_noise[eval_idx];
    weight_sum += weight;
    equity_sum += weight * ctx->static_equity[ctx->evaluated[eval_idx]];
    value_sum += weight * values[eval_idx];
  }
  const double equity_mean = equity_sum / weight_sum;
  const double value_mean = value_sum / weight_sum;
  double slope = 0.0;
  if (args->equity_prior_mean) {
    double covariance = 0.0;
    double variance = 0.0;
    for (int eval_idx = 0; eval_idx < num_evaluated; eval_idx++) {
      const double weight = 1.0 / ctx->gp_noise[eval_idx];
      const double equity_diff =
          ctx->static_equity[ctx->evaluated[eval_idx]] - equity_mean;
      covariance += weight * equity_diff * (values[eval_idx] - value_mean);
      variance += weight * equity_diff * equity_diff;
    }
    if (variance > 0.0) {
      slope = covariance / variance;
    }
  }
  ctx->mean_slope = slope;
  ctx->mean_intercept = value_mean - slope * equity_mean;
  for (int eval_idx = 0; eval_idx < num_evaluated; eval_idx++) {
    values[eval_idx] -= ctx->mean_intercept +
                        slope * ctx->static_equity[ctx->evaluated[eval_idx]];
  }
}

// Fits the surrogate to the evaluated plays' win percentages, choosing the
// length scale by marginal likelihood, and ranks them by its mean.
static void lamc_fit(LamcCtx *ctx, const LamcArgs *args) {
  const int num_evaluated = ctx->num_evaluated;
  const int num_columns = ctx->num_columns;
  double pooled_squares = 0.0;
  double pooled_dof = 0.0;
  for (int eval_idx = 0; eval_idx < num_evaluated; eval_idx++) {
    const int play_idx = ctx->evaluated[eval_idx];
    const double num_samples =
        (double)atomic_load(&ctx->sample_counts[play_idx]);
    const double sample_mean =
        num_samples > 0 ? (double)atomic_load(&ctx->sample_sums[play_idx]) /
                              (double)BAI_FIXED_POINT_SCALE / num_samples
                        : 0.0;
    if (num_samples > 1) {
      const double square_mean =
          (double)atomic_load(&ctx->sample_squares[play_idx]) /
          (double)BAI_FIXED_POINT_SCALE / num_samples;
      const double variance = (square_mean - sample_mean * sample_mean) *
                              num_samples / (num_samples - 1);
      pooled_squares += (variance > 0 ? variance : 0.0) * (num_samples - 1);
      pooled_dof += num_samples - 1;
    }
    ctx->gp_values[eval_idx] = sample_mean;
    ctx->gp_noise[eval_idx] = num_samples;
    memcpy(ctx->gp_points + (ptrdiff_t)eval_idx * num_columns,
           lamc_row(ctx, play_idx), sizeof(double) * num_columns);
  }
  double rollout_variance = pooled_dof > 0 ? pooled_squares / pooled_dof
                                           : LAMC_DEFAULT_ROLLOUT_VARIANCE;
  if (rollout_variance < LAMC_MIN_ROLLOUT_VARIANCE) {
    rollout_variance = LAMC_MIN_ROLLOUT_VARIANCE;
  }
  double noise_sum = 0.0;
  for (int eval_idx = 0; eval_idx < num_evaluated; eval_idx++) {
    const double num_samples = ctx->gp_noise[eval_idx];
    ctx->gp_noise[eval_idx] =
        rollout_variance / (num_samples > 0 ? num_samples : 1.0);
    noise_sum += ctx->gp_noise[eval_idx];
  }
  lamc_fit_prior_mean(ctx, args, ctx->gp_values);
  double value_squares = 0.0;
  for (int eval_idx = 0; eval_idx < num_evaluated; eval_idx++) {
    value_squares += ctx->gp_values[eval_idx] * ctx->gp_values[eval_idx];
  }
  double signal_variance =
      value_squares / num_evaluated - noise_sum / num_evaluated;
  if (signal_variance < LAMC_MIN_SIGNAL_VARIANCE) {
    signal_variance = LAMC_MIN_SIGNAL_VARIANCE;
  }

  if (ctx->gp_num_dims != num_columns) {
    gp_destroy(ctx->gp);
    ctx->gp = gp_create(ctx->evaluated_capacity, num_columns);
    ctx->gp_num_dims = num_columns;
  }
  GaussianProcessParams params = {
      .kernel = args->kernel,
      .inverse_length_scales = ctx->fit_inverse_length_scales,
      .signal_variance = signal_variance,
      .prior_mean = 0.0,
  };
  int best_multiple = -1;
  double best_likelihood = -INFINITY;
  for (int multiple_idx = 0; multiple_idx < LAMC_NUM_LENGTH_SCALE_MULTIPLES;
       multiple_idx++) {
    for (int column = 0; column < num_columns; column++) {
      ctx->fit_inverse_length_scales[column] =
          ctx->inverse_length_scales[column] /
          lamc_length_scale_multiples[multiple_idx];
    }
    if (gp_fit(ctx->gp, &params, ctx->gp_points, ctx->gp_values, ctx->gp_noise,
               num_evaluated) &&
        gp_get_log_marginal_likelihood(ctx->gp) > best_likelihood) {
      best_likelihood = gp_get_log_marginal_likelihood(ctx->gp);
      best_multiple = multiple_idx;
    }
  }
  const double multiple =
      best_multiple >= 0 ? lamc_length_scale_multiples[best_multiple] : 1.0;
  for (int column = 0; column < num_columns; column++) {
    ctx->fit_inverse_length_scales[column] =
        ctx->inverse_length_scales[column] / multiple;
  }
  const bool fitted = gp_fit(ctx->gp, &params, ctx->gp_points, ctx->gp_values,
                             ctx->gp_noise, num_evaluated);
  ctx->fitted = fitted;

  // Rank by the surrogate's mean; by the raw mean if no fit succeeded. Ties
  // keep the order of static equity, the universe's order.
  for (int eval_idx = 0; eval_idx < num_evaluated; eval_idx++) {
    const int play_idx = ctx->evaluated[eval_idx];
    if (fitted) {
      lamc_predict(ctx, play_idx, &ctx->posterior_means[eval_idx], NULL);
    } else {
      ctx->posterior_means[eval_idx] =
          ctx->gp_values[eval_idx] + ctx->mean_intercept +
          ctx->mean_slope * ctx->static_equity[play_idx];
    }
    int insert_at = eval_idx;
    while (insert_at > 0) {
      const int other = ctx->ranked[insert_at - 1];
      const double other_mean = ctx->posterior_means[other];
      const double this_mean = ctx->posterior_means[eval_idx];
      if (other_mean > this_mean ||
          (other_mean == this_mean &&
           ctx->evaluated[other] < ctx->evaluated[eval_idx])) {
        break;
      }
      ctx->ranked[insert_at] = other;
      insert_at--;
    }
    ctx->ranked[insert_at] = eval_idx;
  }
}

static void lamc_add_to_next_pool(LamcCtx *ctx, const int play_idx) {
  ctx->is_proposed[play_idx] = true;
  ctx->next_pool[ctx->next_pool_count++] = play_idx;
}

// Adds up to count unevaluated plays with the highest upper confidence bound,
// each computed once.
static void lamc_propose_by_acquisition(LamcCtx *ctx, const LamcArgs *args,
                                        int count) {
  if (count <= 0) {
    return;
  }
  for (int play_idx = 0; play_idx < ctx->num_universe; play_idx++) {
    if (ctx->is_evaluated[play_idx] || ctx->is_proposed[play_idx]) {
      ctx->min_distance[play_idx] = -INFINITY;
      continue;
    }
    double mean;
    double variance;
    lamc_predict(ctx, play_idx, &mean, &variance);
    ctx->min_distance[play_idx] = mean + args->ucb_beta * sqrt(variance);
  }
  while (count > 0) {
    int best_play = -1;
    double best_bound = -INFINITY;
    for (int play_idx = 0; play_idx < ctx->num_universe; play_idx++) {
      if (!ctx->is_proposed[play_idx] &&
          ctx->min_distance[play_idx] > best_bound) {
        best_bound = ctx->min_distance[play_idx];
        best_play = play_idx;
      }
    }
    if (best_play < 0) {
      return;
    }
    lamc_add_to_next_pool(ctx, best_play);
    count--;
  }
}

// From the pool play at source, a trust-radius step up the surrogate's mean
// along the continuous columns, then the nearest unevaluated play to where it
// lands. Returns false if the mean is flat there or no play is left.
static bool lamc_propose_by_ascent(LamcCtx *ctx, const LamcArgs *args,
                                   const int source) {
  const double *source_row = lamc_row(ctx, source);
  gp_mean_gradient(ctx->gp, source_row, ctx->gradient);
  double norm_squared = 0.0;
  for (int column = 0; column < ctx->num_columns; column++) {
    if (!ctx->column_is_continuous[column]) {
      ctx->gradient[column] = 0.0;
    }
    norm_squared += ctx->gradient[column] * ctx->gradient[column];
  }
  if (!(norm_squared > 0.0)) {
    return false;
  }
  const double step = args->trust_radius / sqrt(norm_squared);
  for (int column = 0; column < ctx->num_columns; column++) {
    ctx->target[column] = source_row[column] + step * ctx->gradient[column];
  }
  int nearest = -1;
  double nearest_distance = INFINITY;
  for (int play_idx = 0; play_idx < ctx->num_universe; play_idx++) {
    if (ctx->is_evaluated[play_idx] || ctx->is_proposed[play_idx]) {
      continue;
    }
    const double distance =
        lamc_distance_squared(ctx, ctx->target, lamc_row(ctx, play_idx));
    if (distance < nearest_distance) {
      nearest_distance = distance;
      nearest = play_idx;
    }
  }
  if (nearest < 0) {
    return false;
  }
  lamc_add_to_next_pool(ctx, nearest);
  return true;
}

// The next pool: the top keep plays by the surrogate's mean, then new plays
// by the proposal strategy, up to the pool size and the surrogate's capacity.
// Returns how many new plays it proposed.
static int lamc_propose(LamcCtx *ctx, const LamcArgs *args) {
  for (int play_idx = 0; play_idx < ctx->num_universe; play_idx++) {
    ctx->is_proposed[play_idx] = false;
  }
  ctx->next_pool_count = 0;
  const int keep =
      args->keep < ctx->num_evaluated ? args->keep : ctx->num_evaluated;
  for (int rank = 0; rank < keep; rank++) {
    lamc_add_to_next_pool(ctx, ctx->evaluated[ctx->ranked[rank]]);
  }
  int wanted = args->pool_size - keep;
  const int room = ctx->evaluated_capacity - ctx->num_evaluated;
  if (wanted > room) {
    wanted = room;
  }
  const int first_new = ctx->next_pool_count;
  switch (args->proposal) {
  case LAMC_PROPOSAL_ASCENT:
    for (int pool_idx = 0; pool_idx < ctx->pool_count &&
                           ctx->next_pool_count - first_new < wanted;
         pool_idx++) {
      const int source = ctx->pool[pool_idx];
      if (ctx->is_proposed[source]) {
        continue;
      }
      lamc_propose_by_ascent(ctx, args, source);
    }
    break;
  case LAMC_PROPOSAL_ACQUIRE:
    break;
  }
  lamc_propose_by_acquisition(ctx, args,
                              wanted - (ctx->next_pool_count - first_new));
  return ctx->next_pool_count - first_new;
}

static bool lamc_top_unchanged(const LamcCtx *ctx, const int *previous_top,
                               const int count) {
  for (int rank = 0; rank < count; rank++) {
    bool found = false;
    for (int other = 0; other < count && !found; other++) {
      found = previous_top[other] == ctx->evaluated[ctx->ranked[rank]];
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

int lamc_build(LamcCtx *ctx, const SimArgs *sim_args) {
  const LamcArgs *args = &sim_args->lamc_args;
  lamc_ctx_ensure_capacity(ctx, args);
  lamc_build_universe(ctx, sim_args);
  if (ctx->num_universe > 0) {
    lamc_build_embedding(ctx, sim_args);
    lamc_build_static_equity(ctx);
  }
  ctx->num_evaluated = 0;
  return ctx->num_universe;
}

const MoveList *lamc_get_universe(const LamcCtx *ctx) { return ctx->universe; }

int lamc_get_num_evaluated(const LamcCtx *ctx) { return ctx->num_evaluated; }

int lamc_get_ranked_play(const LamcCtx *ctx, const int rank) {
  return ctx->evaluated[ctx->ranked[rank]];
}

int lamc_select_candidates(LamcCtx *ctx, const LamcArgs *args,
                           const int count) {
  if (!args->finalists_from_all) {
    const int num_candidates =
        count < ctx->num_evaluated ? count : ctx->num_evaluated;
    for (int rank = 0; rank < num_candidates; rank++) {
      ctx->candidates[rank] = lamc_get_ranked_play(ctx, rank);
    }
    return num_candidates;
  }
  for (int play_idx = 0; play_idx < ctx->num_universe; play_idx++) {
    lamc_predict(ctx, play_idx, &ctx->universe_means[play_idx], NULL);
    ctx->is_proposed[play_idx] = false;
  }
  const int num_candidates =
      count < ctx->num_universe ? count : ctx->num_universe;
  for (int rank = 0; rank < num_candidates; rank++) {
    int best = -1;
    for (int play_idx = 0; play_idx < ctx->num_universe; play_idx++) {
      if (!ctx->is_proposed[play_idx] &&
          (best < 0 ||
           ctx->universe_means[play_idx] > ctx->universe_means[best])) {
        best = play_idx;
      }
    }
    ctx->is_proposed[best] = true;
    ctx->candidates[rank] = best;
  }
  return num_candidates;
}

int lamc_get_candidate(const LamcCtx *ctx, const int rank) {
  return ctx->candidates[rank];
}

uint64_t lamc_get_search_rollouts(const LamcCtx *ctx) {
  uint64_t samples = 0;
  for (int eval_idx = 0; eval_idx < ctx->num_evaluated; eval_idx++) {
    samples += atomic_load(&ctx->sample_counts[ctx->evaluated[eval_idx]]);
  }
  return samples * ctx->rollouts_per_sample;
}

int lamc_search(LamcCtx *ctx, const LamcArgs *args, RandomVariables *base_rvs,
                const LamcSearchLimits *limits) {
  Timer timer;
  ctimer_start(&timer);
  for (int play_idx = 0; play_idx < ctx->num_universe; play_idx++) {
    ctx->is_evaluated[play_idx] = false;
    atomic_init(&ctx->sample_counts[play_idx], 0);
    atomic_init(&ctx->sample_sums[play_idx], 0);
    atomic_init(&ctx->sample_squares[play_idx], 0);
  }
  ctx->num_evaluated = 0;
  ctx->rollouts_per_sample =
      limits->rollouts_per_sample > 0 ? limits->rollouts_per_sample : 1;
  lamc_init_pool(ctx, args);
  int previous_top[LAMC_MAX_KEEP];
  const int top_count = args->keep < LAMC_MAX_KEEP ? args->keep : LAMC_MAX_KEEP;
  int num_top = 0;
  int stable = 0;
  int rounds = 0;
  while (true) {
    double round_time_limit = 0.0;
    if (limits->time_budget_seconds > 0) {
      round_time_limit =
          limits->time_budget_seconds - ctimer_elapsed_seconds(&timer);
      if (round_time_limit <= 0) {
        break;
      }
    }
    lamc_run_round(ctx, args, base_rvs, limits, round_time_limit);
    rounds++;
    lamc_mark_pool_evaluated(ctx);
    lamc_fit(ctx, args);
    if (thread_control_get_status(limits->thread_control) !=
        THREAD_CONTROL_STATUS_STARTED) {
      break;
    }
    const int compared =
        top_count < ctx->num_evaluated ? top_count : ctx->num_evaluated;
    if (num_top == compared &&
        lamc_top_unchanged(ctx, previous_top, compared)) {
      stable++;
    } else {
      stable = 0;
    }
    num_top = compared;
    for (int rank = 0; rank < compared; rank++) {
      previous_top[rank] = ctx->evaluated[ctx->ranked[rank]];
    }
    const uint64_t spent = lamc_get_search_rollouts(ctx);
    const uint64_t next_round_cost = (uint64_t)args->pool_size *
                                     (uint64_t)args->shallow_samples *
                                     ctx->rollouts_per_sample;
    if (rounds >= args->max_rounds || stable >= args->stable_rounds ||
        spent >= limits->rollout_budget ||
        limits->rollout_budget - spent < next_round_cost / 2) {
      break;
    }
    if (lamc_propose(ctx, args) == 0) {
      break;
    }
    memcpy(ctx->pool, ctx->next_pool, sizeof(int) * ctx->next_pool_count);
    ctx->pool_count = ctx->next_pool_count;
  }
  return rounds;
}

// The search's sim args: the sim's, over the universe, with no display and
// no census (whose tables would cover every play in the universe).
static void lamc_fill_explore_args(const LamcCtx *ctx, const SimArgs *sim_args,
                                   SimArgs *explore_args) {
  *explore_args = *sim_args;
  explore_args->move_list = ctx->universe;
  explore_args->num_plays = ctx->num_universe;
  explore_args->bag_cycle = sim_args->lamc_args.explore_bag_cycle;
  explore_args->use_census_rollout = false;
  explore_args->use_heat_map = false;
  explore_args->print_interval = 0;
  explore_args->resume_results = false;
}

const MoveList *lamc_choose_candidates(const SimArgs *sim_args,
                                       LamcCtx **ctx_ptr,
                                       const int baseline_plays,
                                       LamcReport *report) {
  const LamcArgs *args = &sim_args->lamc_args;
  memset(report, 0, sizeof(LamcReport));
  if (*ctx_ptr == NULL) {
    *ctx_ptr = lamc_ctx_create();
  }
  LamcCtx *ctx = *ctx_ptr;
  Timer timer;
  ctimer_start(&timer);
  report->num_universe = lamc_build(ctx, sim_args);
  if (ctx->num_universe <= baseline_plays) {
    return NULL;
  }

  SimArgs explore_args;
  lamc_fill_explore_args(ctx, sim_args, &explore_args);
  const RandomVariablesArgs explore_rvs_args = {
      .type = RANDOM_VARIABLES_SIMMED_PLAYS,
      .sim_args = &explore_args,
      .sim_results = ctx->explore_results,
  };
  if (ctx->explore_rvs && ctx->num_threads == explore_args.num_threads) {
    rvs_reset(ctx->explore_rvs, &explore_rvs_args);
  } else {
    rvs_destroy(ctx->pool_rvs);
    ctx->pool_rvs = NULL;
    rvs_destroy(ctx->explore_rvs);
    ctx->explore_rvs = rvs_create(&explore_rvs_args);
    ctx->num_threads = explore_args.num_threads;
  }

  const uint64_t sample_limit = sim_args->bai_options.sample_limit;
  const LamcSearchLimits limits = {
      .rollout_budget =
          sample_limit > LAMC_UNLIMITED_SAMPLES
              ? UINT64_MAX
              : (uint64_t)(args->explore_share * (double)sample_limit),
      .time_budget_seconds =
          sim_args->bai_options.time_limit_seconds > 0
              ? args->explore_share * sim_args->bai_options.time_limit_seconds
              : 0.0,
      .rollouts_per_sample =
          explore_args.bag_cycle
              ? (uint64_t)rv_sim_bag_cycle_rotations(sim_args->game)
              : 1,
      .num_threads = sim_args->bai_options.num_threads,
      .parent_worker_thread_index =
          sim_args->bai_options.parent_worker_thread_index,
      .thread_control = sim_args->thread_control,
  };
  const int rounds = lamc_search(ctx, args, ctx->explore_rvs, &limits);

  // The candidates: the evaluated plays best by the surrogate's mean.
  const int num_finalists =
      args->num_finalists > 0 ? args->num_finalists : baseline_plays;
  if (ctx->finalists_capacity < num_finalists) {
    move_list_destroy(ctx->finalists);
    ctx->finalists_capacity = num_finalists;
    ctx->finalists = move_list_create(num_finalists);
  }
  const int count = lamc_select_candidates(ctx, args, num_finalists);
  move_list_reset(ctx->finalists);
  for (int rank = 0; rank < count; rank++) {
    const int play_idx = lamc_get_candidate(ctx, rank);
    move_list_add_move(ctx->finalists,
                       move_list_get_move(ctx->universe, play_idx));
    if (play_idx >= baseline_plays) {
      report->finalists_outside_static++;
    }
  }
  move_list_set_rack(ctx->finalists, move_list_get_rack(sim_args->move_list));
  report->num_evaluated = ctx->num_evaluated;
  report->num_rounds = rounds;
  report->explore_rollouts = lamc_get_search_rollouts(ctx);
  report->explore_seconds = ctimer_elapsed_seconds(&timer);
  report->num_finalists = count;

  atomic_fetch_add(&lamc_total_searches, 1);
  atomic_fetch_add(&lamc_total_rounds, (uint64_t)rounds);
  atomic_fetch_add(&lamc_total_evaluated, (uint64_t)ctx->num_evaluated);
  atomic_fetch_add(&lamc_total_explore_rollouts, report->explore_rollouts);
  atomic_fetch_add(&lamc_total_explore_microseconds,
                   (uint64_t)(report->explore_seconds * 1e6));
  atomic_fetch_add(&lamc_total_finalists_outside_static,
                   (uint64_t)report->finalists_outside_static);
  return ctx->finalists;
}

void lamc_note_choice(const LamcCtx *ctx, const Move *chosen,
                      const int baseline_plays) {
  if (!ctx || !chosen || ctx->num_universe <= baseline_plays) {
    return;
  }
  const Equity cutoff =
      move_get_equity(move_list_get_move(ctx->universe, baseline_plays - 1));
  if (move_get_equity(chosen) < cutoff) {
    atomic_fetch_add(&lamc_total_chosen_outside_static, 1);
  }
}

void lamc_totals_reset(void) {
  atomic_store(&lamc_total_searches, 0);
  atomic_store(&lamc_total_rounds, 0);
  atomic_store(&lamc_total_evaluated, 0);
  atomic_store(&lamc_total_explore_rollouts, 0);
  atomic_store(&lamc_total_explore_microseconds, 0);
  atomic_store(&lamc_total_finalists_outside_static, 0);
  atomic_store(&lamc_total_chosen_outside_static, 0);
}

LamcTotals lamc_get_totals(void) {
  const LamcTotals totals = {
      .searches = atomic_load(&lamc_total_searches),
      .rounds = atomic_load(&lamc_total_rounds),
      .evaluated = atomic_load(&lamc_total_evaluated),
      .explore_rollouts = atomic_load(&lamc_total_explore_rollouts),
      .explore_microseconds = atomic_load(&lamc_total_explore_microseconds),
      .finalists_outside_static =
          atomic_load(&lamc_total_finalists_outside_static),
      .chosen_outside_static = atomic_load(&lamc_total_chosen_outside_static),
  };
  return totals;
}
