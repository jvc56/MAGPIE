#include "random_variable.h"

#include "../compat/cpthread.h"
#include "../def/cpthread_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/value_net_defs.h"
#include "../ent/alias_method.h"
#include "../ent/bag.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/inference_results.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/sim_args.h"
#include "../ent/sim_results.h"
#include "../ent/thread_control.h"
#include "../ent/value_net_history.h"
#include "../ent/win_pct.h"
#include "../ent/xoshiro.h"
#include "../str/sim_string.h"
#include "../util/io_util.h"
#include "bai_logger.h"
#include "gameplay.h"
#include "move_gen.h"
#include "value_net_features.h"
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SIMILARITY_EPSILON 1e-6

typedef double (*rvs_sample_func_t)(RandomVariables *, const uint64_t,
                                    const int, const uint64_t, BAILogger *);
typedef bool (*rvs_similar_func_t)(RandomVariables *, const int, const int);
typedef void (*rvs_destroy_data_func_t)(RandomVariables *);
typedef int (*rvs_get_best_arm_index_func_t)(const RandomVariables *);

struct RandomVariables {
  uint64_t num_rvs;
  atomic_uint_fast64_t total_samples;
  rvs_sample_func_t sample_func;
  rvs_similar_func_t similar_func;
  rvs_destroy_data_func_t destroy_data_func;
  rvs_get_best_arm_index_func_t get_best_arm_index_func;
  void *data;
};

// Draws a uniform value in [0, 1) from prng. Caller must hold whatever mutex
// guards prng (see uniform_sample for the self-locking variant).
static inline double uniform_sample_while_locked(XoshiroPRNG *prng) {
  return (double)prng_next(prng) / ((double)UINT64_MAX);
}

double uniform_sample(XoshiroPRNG *prng, cpthread_mutex_t *mutex) {
  cpthread_mutex_lock(mutex);
  double result = uniform_sample_while_locked(prng);
  cpthread_mutex_unlock(mutex);
  return result;
}

typedef struct RVUniform {
  XoshiroPRNG *xoshiro_prng;
  cpthread_mutex_t mutex;
} RVUniform;

double rv_uniform_sample(RandomVariables *rvs,
                         const uint64_t __attribute__((unused)) k,
                         const int __attribute__((unused)) thread_index,
                         const uint64_t __attribute__((unused)) sample_count,
                         BAILogger __attribute__((unused)) * bai_logger) {
  RVUniform *rv_uniform = (RVUniform *)rvs->data;
  return uniform_sample(rv_uniform->xoshiro_prng, &rv_uniform->mutex);
}

bool rv_uniform_are_similar(RandomVariables __attribute__((unused)) * rvs,
                            const int __attribute__((unused)) i,
                            const int __attribute__((unused)) j) {
  return false;
}

void rv_uniform_destroy(RandomVariables *rvs) {
  RVUniform *rv_uniform = (RVUniform *)rvs->data;
  prng_destroy(rv_uniform->xoshiro_prng);
  free(rv_uniform);
}

static int rv_unsupported_get_best_arm_index(const RandomVariables *rvs
                                             __attribute__((unused))) {
  log_fatal("rvs_get_best_arm_index called on non-simmed-plays "
            "RandomVariables");
  return -1;
}

void rv_uniform_create(RandomVariables *rvs, const uint64_t seed) {
  rvs->sample_func = rv_uniform_sample;
  rvs->similar_func = rv_uniform_are_similar;
  rvs->destroy_data_func = rv_uniform_destroy;
  rvs->get_best_arm_index_func = rv_unsupported_get_best_arm_index;
  RVUniform *rv_uniform = malloc_or_die(sizeof(RVUniform));
  rv_uniform->xoshiro_prng = prng_create(seed);
  cpthread_mutex_init(&rv_uniform->mutex);
  rvs->data = rv_uniform;
}

void rv_uniform_reset(RandomVariables *rvs, const uint64_t seed) {
  RVUniform *rv_uniform = (RVUniform *)rvs->data;
  prng_seed(rv_uniform->xoshiro_prng, seed);
}

typedef struct RVUniformPredetermined {
  uint64_t num_samples;
  uint64_t index;
  double *samples;
  cpthread_mutex_t mutex;
} RVUniformPredetermined;

double rv_uniform_predetermined_sample(
    RandomVariables *rvs, const uint64_t __attribute__((unused)) k,
    const int __attribute__((unused)) thread_index,
    const uint64_t __attribute__((unused)) sample_count,
    BAILogger __attribute__((unused)) * bai_logger) {
  RVUniformPredetermined *rv_uniform_predetermined =
      (RVUniformPredetermined *)rvs->data;
  cpthread_mutex_lock(&rv_uniform_predetermined->mutex);
  if (rv_uniform_predetermined->index >=
      rv_uniform_predetermined->num_samples) {
    log_fatal("ran out of uniform predetermined samples");
  }
  const uint64_t index = rv_uniform_predetermined->index++;
  cpthread_mutex_unlock(&rv_uniform_predetermined->mutex);
  return rv_uniform_predetermined->samples[index];
}

bool rv_uniform_predetermined_are_similar(RandomVariables
                                              __attribute__((unused)) *
                                              rvs,
                                          const int __attribute__((unused)) i,
                                          const int __attribute__((unused)) j) {
  return false;
}

void rv_uniform_predetermined_destroy(RandomVariables *rvs) {
  RVUniformPredetermined *rv_uniform_predetermined =
      (RVUniformPredetermined *)rvs->data;
  free(rv_uniform_predetermined->samples);
  free(rv_uniform_predetermined);
}

void rv_uniform_predetermined_create(RandomVariables *rvs,
                                     const double *samples,
                                     const uint64_t num_samples) {
  rvs->sample_func = rv_uniform_predetermined_sample;
  rvs->similar_func = rv_uniform_predetermined_are_similar;
  rvs->destroy_data_func = rv_uniform_predetermined_destroy;
  rvs->get_best_arm_index_func = rv_unsupported_get_best_arm_index;
  RVUniformPredetermined *rv_uniform_predetermined =
      malloc_or_die(sizeof(RVUniformPredetermined));
  rv_uniform_predetermined->num_samples = num_samples;
  rv_uniform_predetermined->index = 0;
  rv_uniform_predetermined->samples =
      malloc_or_die(rv_uniform_predetermined->num_samples * sizeof(double));
  memcpy(rv_uniform_predetermined->samples, samples,
         rv_uniform_predetermined->num_samples * sizeof(double));
  cpthread_mutex_init(&rv_uniform_predetermined->mutex);
  rvs->data = rv_uniform_predetermined;
}

void rv_uniform_predetermined_reset(RandomVariables *rvs) {
  RVUniformPredetermined *rv_uniform_predetermined =
      (RVUniformPredetermined *)rvs->data;
  rv_uniform_predetermined->index = 0;
}

typedef struct RVNormal {
  // One PRNG per arm, each seeded with a non-overlapping subsequence, guarded
  // by its own mutex. See rv_normal_seed_arm_prngs / rv_normal_sample for why.
  XoshiroPRNG **xoshiro_prngs;
  cpthread_mutex_t *mutexes;
  uint64_t num_arms;
  double *means_and_vars;
} RVNormal;

// Re-seeds every arm's PRNG so arm k draws from the subsequence reached by
// jumping the base stream k times (2^128 draws apart). Independent per-arm
// streams make an arm's sample sequence -- hence its running mean/variance
// after N draws -- a function of N alone, not of how worker threads interleave
// across arms, which keeps BAI results reproducible across thread counts.
static void rv_normal_seed_arm_prngs(RVNormal *rv_normal, const uint64_t seed) {
  if (rv_normal->num_arms == 0) {
    return;
  }
  // Arm k's stream is the base stream jumped k times. Build them in O(num_arms)
  // jumps by seeding arm 0, then copying the previous arm and advancing it one
  // jump, rather than re-seeding and jumping k times per arm (O(num_arms^2)).
  prng_seed(rv_normal->xoshiro_prngs[0], seed);
  for (uint64_t arm = 1; arm < rv_normal->num_arms; arm++) {
    prng_copy(rv_normal->xoshiro_prngs[arm], rv_normal->xoshiro_prngs[arm - 1]);
    prng_jump(rv_normal->xoshiro_prngs[arm]);
  }
}

double rv_normal_sample(RandomVariables *rvs, const uint64_t k,
                        const int __attribute__((unused)) thread_index,
                        const uint64_t __attribute__((unused)) sample_count,
                        BAILogger __attribute__((unused)) * bai_logger) {
  // Implements the Box-Muller transform. The arm's mutex is held across the
  // entire rejection loop so each logical sample consumes a contiguous block
  // of draws; otherwise concurrent draws on the same arm could re-pair (u, v)
  // and change the nonlinear result, breaking reproducibility.
  RVNormal *rv_normal = (RVNormal *)rvs->data;
  XoshiroPRNG *arm_prng = rv_normal->xoshiro_prngs[k];
  cpthread_mutex_t *arm_mutex = &rv_normal->mutexes[k];
  double u = 0.0;
  double s = 2.0;
  cpthread_mutex_lock(arm_mutex);
  while (s >= 1.0 || s == 0.0) {
    u = 2.0 * uniform_sample_while_locked(arm_prng) - 1.0;
    const double v = 2.0 * uniform_sample_while_locked(arm_prng) - 1.0;
    s = u * u + v * v;
  }
  cpthread_mutex_unlock(arm_mutex);
  s = sqrt(-2.0 * log(s) / s);
  // means_and_vars holds {mean, variance} per arm, so scale the unit normal
  // (u * s) by the standard deviation. This matches the variance semantics used
  // by rv_normal_predetermined_sample (which multiplies its unit sample by
  // sqrt(sigma2)); without the sqrt the arm's variance would be variance^2.
  const double mean = rv_normal->means_and_vars[k * 2];
  const double variance = rv_normal->means_and_vars[k * 2 + 1];
  return mean + sqrt(variance) * u * s;
}

bool rv_normal_are_similar(RandomVariables *rvs, const int i, const int j) {
  if (i == j) {
    return false;
  }
  const RVNormal *rv_normal = (RVNormal *)rvs->data;
  return fabs(rv_normal->means_and_vars[(ptrdiff_t)(i * 2)] -
              rv_normal->means_and_vars[(ptrdiff_t)(j * 2)]) <
             SIMILARITY_EPSILON &&
         fabs(rv_normal->means_and_vars[(ptrdiff_t)(i * 2 + 1)] -
              rv_normal->means_and_vars[(ptrdiff_t)(j * 2 + 1)]) <
             SIMILARITY_EPSILON;
}

void rv_normal_destroy(RandomVariables *rvs) {
  RVNormal *rv_normal = (RVNormal *)rvs->data;
  for (uint64_t arm = 0; arm < rv_normal->num_arms; arm++) {
    prng_destroy(rv_normal->xoshiro_prngs[arm]);
  }
  free(rv_normal->xoshiro_prngs);
  free(rv_normal->mutexes);
  free(rv_normal->means_and_vars);
  free(rv_normal);
}

void rv_normal_create(RandomVariables *rvs, const uint64_t seed,
                      const double *means_and_vars) {
  rvs->sample_func = rv_normal_sample;
  rvs->similar_func = rv_normal_are_similar;
  rvs->destroy_data_func = rv_normal_destroy;
  rvs->get_best_arm_index_func = rv_unsupported_get_best_arm_index;
  RVNormal *rv_normal = malloc_or_die(sizeof(RVNormal));
  rv_normal->num_arms = rvs->num_rvs;
  rv_normal->xoshiro_prngs =
      malloc_or_die(rv_normal->num_arms * sizeof(XoshiroPRNG *));
  rv_normal->mutexes =
      malloc_or_die(rv_normal->num_arms * sizeof(cpthread_mutex_t));
  for (uint64_t arm = 0; arm < rv_normal->num_arms; arm++) {
    rv_normal->xoshiro_prngs[arm] = prng_create(seed);
    cpthread_mutex_init(&rv_normal->mutexes[arm]);
  }
  rv_normal_seed_arm_prngs(rv_normal, seed);
  rv_normal->means_and_vars = malloc_or_die(rvs->num_rvs * 2 * sizeof(double));
  memcpy(rv_normal->means_and_vars, means_and_vars,
         rvs->num_rvs * 2 * sizeof(double));
  rvs->data = rv_normal;
}

void rv_normal_reset(RandomVariables *rvs, const uint64_t seed) {
  RVNormal *rv_normal = (RVNormal *)rvs->data;
  rv_normal_seed_arm_prngs(rv_normal, seed);
}

typedef struct RVNormalPredetermined {
  uint64_t num_samples;
  uint64_t index;
  double *samples;
  double *means_and_vars;
  cpthread_mutex_t mutex;
} RVNormalPredetermined;

double rv_normal_predetermined_sample(RandomVariables *rvs, const uint64_t k,
                                      const int
                                      __attribute__((unused)) thread_index,
                                      const uint64_t
                                      __attribute__((unused)) sample_count,
                                      BAILogger *bai_logger) {
  RVNormalPredetermined *rv_normal_predetermined =
      (RVNormalPredetermined *)rvs->data;
  cpthread_mutex_lock(&rv_normal_predetermined->mutex);
  if (rv_normal_predetermined->index >= rv_normal_predetermined->num_samples) {
    log_fatal("ran out of normal predetermined samples");
  }
  const uint64_t index = rv_normal_predetermined->index++;
  cpthread_mutex_unlock(&rv_normal_predetermined->mutex);
  const double mean = rv_normal_predetermined->means_and_vars[k * 2];
  const double sigma2 = rv_normal_predetermined->means_and_vars[k * 2 + 1];
  const double sample = rv_normal_predetermined->samples[index];
  const double result = mean + sqrt(sigma2) * sample;
  bai_logger_log_title(bai_logger, "DETERMINISTIC_SAMPLE");
  bai_logger_log_int(bai_logger, "index", (int)(index + 1));
  bai_logger_log_int(bai_logger, "arm", (int)k + 1);
  bai_logger_log_double(bai_logger, "s", result);
  bai_logger_log_double(bai_logger, "u", mean);
  bai_logger_log_double(bai_logger, "sigma2", sigma2);
  bai_logger_log_double(bai_logger, "samp", sample);
  bai_logger_flush(bai_logger);
  return result;
}

bool rv_normal_predetermined_are_similar(RandomVariables *rvs, const int i,
                                         const int j) {
  if (i == j) {
    return false;
  }
  const RVNormalPredetermined *rv_normal_predetermined =
      (RVNormalPredetermined *)rvs->data;
  return fabs(rv_normal_predetermined->means_and_vars[(ptrdiff_t)(i * 2)] -
              rv_normal_predetermined->means_and_vars[(ptrdiff_t)(j * 2)]) <
             SIMILARITY_EPSILON &&
         fabs(rv_normal_predetermined->means_and_vars[(ptrdiff_t)(i * 2 + 1)] -
              rv_normal_predetermined->means_and_vars[(ptrdiff_t)(j * 2 + 1)]) <
             SIMILARITY_EPSILON;
}

void rv_normal_predetermined_destroy(RandomVariables *rvs) {
  RVNormalPredetermined *rv_normal_predetermined =
      (RVNormalPredetermined *)rvs->data;
  free(rv_normal_predetermined->samples);
  free(rv_normal_predetermined->means_and_vars);
  free(rv_normal_predetermined);
}

void rv_normal_predetermined_create(RandomVariables *rvs, const double *samples,
                                    const uint64_t num_samples,
                                    const double *means_and_vars) {
  rvs->sample_func = rv_normal_predetermined_sample;
  rvs->similar_func = rv_normal_predetermined_are_similar;
  rvs->destroy_data_func = rv_normal_predetermined_destroy;
  rvs->get_best_arm_index_func = rv_unsupported_get_best_arm_index;
  RVNormalPredetermined *rv_normal_predetermined =
      malloc_or_die(sizeof(RVNormalPredetermined));
  rv_normal_predetermined->num_samples = num_samples;
  rv_normal_predetermined->index = 0;
  rv_normal_predetermined->samples =
      malloc_or_die(rv_normal_predetermined->num_samples * sizeof(double));
  memcpy(rv_normal_predetermined->samples, samples,
         rv_normal_predetermined->num_samples * sizeof(double));
  rv_normal_predetermined->means_and_vars =
      malloc_or_die(rvs->num_rvs * 2 * sizeof(double));
  memcpy(rv_normal_predetermined->means_and_vars, means_and_vars,
         rvs->num_rvs * 2 * sizeof(double));
  cpthread_mutex_init(&rv_normal_predetermined->mutex);
  rvs->data = rv_normal_predetermined;
}

void rv_normal_predetermined_reset(RandomVariables *rvs) {
  RVNormalPredetermined *rv_normal_predetermined =
      (RVNormalPredetermined *)rvs->data;
  rv_normal_predetermined->index = 0;
}

enum {
  SIM_VALUE_NET_DEFAULT_CANDIDATES = 15,
  SIM_VALUE_NET_DEFAULT_BATCH = 8,
};

// Value net rollout plies (SimArgs.rollout_value_net_*). Each thread keeps,
// per play, a queue of iterations it computed batch at a time: each one's
// seed and the moves the net chose for its first value net plies. A refill
// advances its batch iterations together, one evaluation per ply over every
// iteration still going, so each evaluation scores batch positions'
// candidates at once and no lock is held while it runs. A seed reproduces
// an iteration's opening state only from the same bag order, which every
// iteration changes (racks go back to the bag), so every value net
// iteration, queued or played, starts from a copy of start, and
// rv_sim_sample replays its queued moves. When the sim stops, each thread
// can leave up to batch - 1 computed iterations per play unused.
typedef struct SimValueNetShared {
  int plays;
  int batch;
  // Rollout plies the net chooses, 1..num_plies.
  int plies;
  // Bumped by each sim start, so threads empty queues from an earlier sim.
  uint64_t generation;
  Game *start;
} SimValueNetShared;

// One thread's queues and refill buffers.
typedef struct SimValueNetWorker {
  int plays;
  int batch;
  int plies;
  int candidates;
  uint64_t generation;
  // Per play, batch iterations: their seeds, the moves chosen (plies each)
  // and how many (fewer than plies when the game ended first).
  uint64_t *seeds; // [plays * batch]
  Move *moves;     // [plays * batch * plies]
  int *move_count; // [plays * batch]
  int *count;      // [plays]
  int *next;       // [plays]
  // A refill's iterations: their games, each player's value net history
  // (by player index), and whether they still take value net moves.
  Game **games;               // [batch]
  ValueNetHistory *histories; // [batch * 2]
  bool *going;                // [batch]
  // One evaluation's candidates and rows.
  Move *candidate_moves; // [batch * candidates]
  int *candidate_count;  // [batch]
  float *board;          // [batch * candidates] rows
  float *scalars;
  float *values;
  float *spreads;
  // Each row's mover spread just after the move.
  double *spread_after;
  MoveList *list;
  Game *scratch;
  // The moves of the iteration rv_sim_sample is playing.
  Move *iteration_moves; // [plies]
} SimValueNetWorker;

typedef struct SimmerWorker {
  Game *game;
  MoveList *move_list;
  XoshiroPRNG *prng;
  SimValueNetWorker value_net;
} SimmerWorker;

typedef struct Simmer {
  Equity initial_spread;
  int initial_player;
  int dist_size;
  Rack *known_opp_rack;
  SimmerWorker **workers;
  // Owned by the caller
  const WinPct *win_pcts;
  bool use_inference;
  bool use_alias_method;
  const InferenceResults *inference_results;
  int num_threads;
  // In PGP mode, each autoplay worker has its own simmer with num_threads
  // workers indexed 0..num_threads-1. However, the global rvs_thread_index
  // passed to rv_sim_sample is parent_worker_thread_index + bai_thread_index,
  // which can exceed num_threads-1. Storing parent_worker_thread_index lets us
  // compute the local 0-based worker index: thread_index - parent_offset.
  int parent_worker_thread_index;
  int print_interval;
  int max_num_display_plays;
  int max_num_display_plies;
  // Utility blend weights consumed by rv_sim_sample (see sim_utility_blend
  // in sim_args.h). Copied from SimArgs on create/reset.
  double utility_w_winpct;
  double utility_w_spread;
  double utility_spread_scale;
  bool use_margin_forecast;
  // See SimArgs.rollout_value_net_*.
  value_net_rows_fn value_net_evaluate;
  void *value_net_context;
  int value_net_candidates;
  int value_net_batch;
  ValueNetHistory value_net_history;
  ValueNetHistory value_net_own_history;
  SimValueNetShared value_net_shared;
  // The rollout PAT settings (SimArgs.pat_rollout_*), for copies of the
  // start game.
  bool pat_rollout_disabled;
  uint32_t pat_rollout_disabled_classes_mask;
  ThreadControl *thread_control;
  SimResults *sim_results;
} Simmer;

static void sim_value_net_worker_free(SimValueNetWorker *worker) {
  free(worker->seeds);
  free(worker->moves);
  free(worker->move_count);
  free(worker->count);
  free(worker->next);
  if (worker->games != NULL) {
    for (int seed_idx = 0; seed_idx < worker->batch; seed_idx++) {
      game_destroy(worker->games[seed_idx]);
    }
    free(worker->games);
  }
  free(worker->histories);
  free(worker->going);
  free(worker->candidate_moves);
  free(worker->candidate_count);
  free(worker->board);
  free(worker->scalars);
  free(worker->values);
  free(worker->spreads);
  free(worker->spread_after);
  free(worker->iteration_moves);
  move_list_destroy(worker->list);
  game_destroy(worker->scratch);
  memset(worker, 0, sizeof(SimValueNetWorker));
}

// Sizes a thread's queues and buffers for the sim (plays, batch, plies) and
// candidates per position, and empties its queues when a new sim started.
static void sim_value_net_worker_ensure(SimValueNetWorker *worker,
                                        const SimValueNetShared *shared,
                                        int candidates) {
  if (worker->plays != shared->plays || worker->batch != shared->batch ||
      worker->plies != shared->plies || worker->candidates != candidates) {
    sim_value_net_worker_free(worker);
    const int batch = shared->batch;
    worker->plays = shared->plays;
    worker->batch = batch;
    worker->plies = shared->plies;
    worker->candidates = candidates;
    worker->generation = shared->generation;
    const size_t slots = (size_t)shared->plays * (size_t)batch;
    worker->seeds = malloc_or_die(sizeof(uint64_t) * slots);
    worker->moves = malloc_or_die(sizeof(Move) * slots * (size_t)shared->plies);
    worker->move_count = malloc_or_die(sizeof(int) * slots);
    worker->count = calloc_or_die((size_t)shared->plays, sizeof(int));
    worker->next = calloc_or_die((size_t)shared->plays, sizeof(int));
    worker->games = malloc_or_die(sizeof(Game *) * (size_t)batch);
    for (int seed_idx = 0; seed_idx < batch; seed_idx++) {
      worker->games[seed_idx] = game_duplicate(shared->start);
    }
    worker->histories =
        malloc_or_die(sizeof(ValueNetHistory) * (size_t)batch * 2);
    worker->going = malloc_or_die(sizeof(bool) * (size_t)batch);
    const size_t rows = (size_t)batch * (size_t)candidates;
    worker->candidate_moves = malloc_or_die(sizeof(Move) * rows);
    worker->candidate_count = malloc_or_die(sizeof(int) * (size_t)batch);
    worker->board =
        malloc_or_die(sizeof(float) * rows * VALUE_NET_BOARD_FLOATS);
    worker->scalars = malloc_or_die(sizeof(float) * rows * VALUE_NET_SCALARS);
    worker->values = malloc_or_die(sizeof(float) * rows);
    // Read by value_net_utility even when the utility does not weigh spread
    // (and the evaluation leaves them unset).
    worker->spreads = calloc_or_die(rows, sizeof(float));
    worker->spread_after = malloc_or_die(sizeof(double) * rows);
    worker->iteration_moves =
        malloc_or_die(sizeof(Move) * (size_t)shared->plies);
    worker->list = move_list_create(candidates);
    worker->scratch = game_duplicate(shared->start);
  }
  if (worker->generation != shared->generation) {
    for (int play_idx = 0; play_idx < worker->plays; play_idx++) {
      worker->count[play_idx] = 0;
      worker->next[play_idx] = 0;
    }
    worker->generation = shared->generation;
  }
}

static void sim_value_net_shared_free(SimValueNetShared *shared) {
  game_destroy(shared->start);
  memset(shared, 0, sizeof(SimValueNetShared));
}

// Prepares a sim of plays plays, batch iterations per refill and plies
// value net plies, taking game as every iteration's start; threads empty
// their queues from any earlier sim.
static void sim_value_net_shared_prepare(SimValueNetShared *shared, int plays,
                                         int batch, int plies,
                                         const Game *game) {
  shared->plays = plays;
  shared->batch = batch;
  shared->plies = plies;
  shared->generation++;
  if (shared->start == NULL) {
    shared->start = game_duplicate(game);
  } else {
    game_copy(shared->start, game);
  }
}

// A worker's copy of the game plays the sim's rollouts, so both of its
// players take the simming player's rollout PAT settings.
static void simmer_worker_set_rollout_pat(const SimmerWorker *simmer_worker,
                                          const SimArgs *sim_args) {
  for (int player_index = 0; player_index < 2; player_index++) {
    player_set_pat_usage(game_get_player(simmer_worker->game, player_index),
                         sim_args->pat_rollout_disabled,
                         sim_args->pat_rollout_disabled_classes_mask);
  }
}

// The same for a value net iteration's copy of the start game: copying a
// game copies its players' PAT settings, so each copy needs this.
static void sim_set_rollout_pat(const Simmer *simmer, const Game *game) {
  for (int player_index = 0; player_index < 2; player_index++) {
    player_set_pat_usage(game_get_player(game, player_index),
                         simmer->pat_rollout_disabled,
                         simmer->pat_rollout_disabled_classes_mask);
  }
}

SimmerWorker *simmer_create_worker(const SimArgs *sim_args) {
  SimmerWorker *simmer_worker = calloc_or_die(1, sizeof(SimmerWorker));
  simmer_worker->game = game_duplicate(sim_args->game);
  simmer_worker_set_rollout_pat(simmer_worker, sim_args);
  game_set_backup_mode(simmer_worker->game, BACKUP_MODE_SIMULATION);
  simmer_worker->move_list = move_list_create(1);
  simmer_worker->prng = prng_create(0);
  return simmer_worker;
}

void simmer_reset_worker(SimmerWorker *simmer_worker, const SimArgs *sim_args) {
  game_copy(simmer_worker->game, sim_args->game);
  simmer_worker_set_rollout_pat(simmer_worker, sim_args);
}

void simmer_worker_destroy(SimmerWorker *simmer_worker) {
  if (!simmer_worker) {
    return;
  }
  game_destroy(simmer_worker->game);
  move_list_destroy(simmer_worker->move_list);
  prng_destroy(simmer_worker->prng);
  sim_value_net_worker_free(&simmer_worker->value_net);
  free(simmer_worker);
}

// Seeds game for one iteration and deals the opponent's rack, as every
// iteration starts. Returns the opponent's player index.
static int sim_start_iteration(const Simmer *simmer, Game *game,
                               XoshiroPRNG *prng, uint64_t seed) {
  prng_seed(prng, seed);
  game_seed(game, seed);
  const int player_off_turn_index = 1 - game_get_player_on_turn_index(game);
  bool set_player_off_turn_rack_with_known_opp_rack = false;
  if (simmer->use_alias_method) {
    Rack inferred_rack;
    rack_set_dist_size(&inferred_rack, simmer->dist_size);
    if (alias_method_sample(
            inference_results_get_alias_method(simmer->inference_results), prng,
            &inferred_rack)) {
      set_random_rack(game, player_off_turn_index, &inferred_rack);
    } else {
      set_player_off_turn_rack_with_known_opp_rack = true;
    }
  } else {
    set_player_off_turn_rack_with_known_opp_rack = true;
  }
  if (set_player_off_turn_rack_with_known_opp_rack) {
    set_random_rack(game, player_off_turn_index, simmer->known_opp_rack);
  }
  return player_off_turn_index;
}

// Queues move as the next value net move of seed_idx's iteration (slot),
// records it in the other player's history and plays it on the iteration's
// game.
static void sim_value_net_play(SimValueNetWorker *worker, int seed_idx,
                               size_t slot, const Move *move) {
  Game *game = worker->games[seed_idx];
  const int mover = game_get_player_on_turn_index(game);
  Move *queued = &worker->moves[(slot * (size_t)worker->plies) +
                                (size_t)worker->move_count[slot]];
  move_copy(queued, move);
  worker->move_count[slot]++;
  value_net_history_record_opponent_move(
      &worker->histories[((size_t)seed_idx * 2) + (size_t)(1 - mover)], queued);
  play_move(queued, game, NULL);
  if (game_over(game) || worker->move_count[slot] == worker->plies) {
    worker->going[seed_idx] = false;
  }
}

// Fills this thread's queue for play_index with batch iterations advanced
// together from the candidate, ply by ply. At each ply, each iteration still
// going takes the top candidates by static equity for the player on turn;
// one with an empty bag or a single candidate plays the top one, and the
// rest are scored in one evaluation and play the best by the sim's utility
// (value_net_utility; ties: more tiles played).
static void sim_value_net_refill(Simmer *simmer, SimmerWorker *simmer_worker,
                                 SimmedPlay *simmed_play, int play_index) {
  const SimValueNetShared *shared = &simmer->value_net_shared;
  SimValueNetWorker *worker = &simmer_worker->value_net;
  const Move *candidate = simmed_play_get_move(simmed_play);
  const size_t slot_base = (size_t)play_index * (size_t)worker->batch;
  for (int seed_idx = 0; seed_idx < worker->batch; seed_idx++) {
    const size_t slot = slot_base + (size_t)seed_idx;
    const uint64_t seed = simmed_play_get_seed(simmed_play);
    worker->seeds[slot] = seed;
    worker->move_count[slot] = 0;
    Game *game = worker->games[seed_idx];
    game_copy(game, shared->start);
    sim_set_rollout_pat(simmer, game);
    game_set_backup_mode(game, BACKUP_MODE_OFF);
    sim_start_iteration(simmer, game, simmer_worker->prng, seed);
    ValueNetHistory *histories = &worker->histories[(size_t)seed_idx * 2];
    histories[simmer->initial_player] = simmer->value_net_own_history;
    histories[1 - simmer->initial_player] = simmer->value_net_history;
    value_net_history_record_opponent_move(
        &histories[1 - simmer->initial_player], candidate);
    play_move(candidate, game, NULL);
    worker->going[seed_idx] = !game_over(game);
  }
  for (int ply = 0; ply < worker->plies; ply++) {
    int rows = 0;
    for (int seed_idx = 0; seed_idx < worker->batch; seed_idx++) {
      worker->candidate_count[seed_idx] = 0;
      if (!worker->going[seed_idx]) {
        continue;
      }
      Game *game = worker->games[seed_idx];
      MoveList *list = worker->list;
      move_list_reset(list);
      const MoveGenArgs args = {
          .game = game,
          .move_list = list,
          .move_record_type = MOVE_RECORD_ALL,
          .move_sort_type = MOVE_SORT_EQUITY,
          .override_kwg = NULL,
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
          .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      };
      generate_moves(&args);
      move_list_sort_moves(list);
      const int count = move_list_get_count(list);
      if (count <= 1 || bag_get_letters(game_get_bag(game)) == 0) {
        // Nothing to choose, or the net is not used with an empty bag.
        sim_value_net_play(worker, seed_idx, slot_base + (size_t)seed_idx,
                           move_list_get_move(list, 0));
        continue;
      }
      const ValueNetHistory *history =
          &worker->histories[((size_t)seed_idx * 2) +
                             (size_t)game_get_player_on_turn_index(game)];
      Move *moves =
          worker->candidate_moves + ((size_t)seed_idx * worker->candidates);
      for (int move_idx = 0; move_idx < count; move_idx++) {
        move_copy(&moves[move_idx], move_list_get_move(list, move_idx));
        worker->spread_after[rows] =
            value_net_spread_after_move(game, &moves[move_idx]);
        value_net_features_for_move(
            game, &moves[move_idx], history, worker->scratch,
            worker->board + ((size_t)rows * VALUE_NET_BOARD_FLOATS),
            worker->scalars + ((size_t)rows * VALUE_NET_SCALARS));
        rows++;
      }
      worker->candidate_count[seed_idx] = count;
    }
    if (rows == 0) {
      continue;
    }
    simmer->value_net_evaluate(simmer->value_net_context, rows, worker->board,
                               worker->scalars, worker->values,
                               simmer->utility_w_spread > 0.0 ? worker->spreads
                                                              : NULL);
    int row = 0;
    for (int seed_idx = 0; seed_idx < worker->batch; seed_idx++) {
      const int count = worker->candidate_count[seed_idx];
      if (count == 0) {
        continue;
      }
      const Move *moves =
          worker->candidate_moves + ((size_t)seed_idx * worker->candidates);
      int best = 0;
      double best_utility = 0.0;
      for (int move_idx = 0; move_idx < count; move_idx++) {
        const int move_row = row + move_idx;
        const double utility = value_net_utility(
            worker->values[move_row], worker->spreads[move_row],
            worker->spread_after[move_row], simmer->utility_w_winpct,
            simmer->utility_w_spread, simmer->utility_spread_scale);
        if (move_idx == 0 || utility > best_utility ||
            (utility == best_utility &&
             move_get_tiles_played(&moves[move_idx]) >
                 move_get_tiles_played(&moves[best]))) {
          best = move_idx;
          best_utility = utility;
        }
      }
      sim_value_net_play(worker, seed_idx, slot_base + (size_t)seed_idx,
                         &moves[best]);
      row += count;
    }
  }
  worker->count[play_index] = worker->batch;
  worker->next[play_index] = 0;
}

#ifndef NDEBUG
// Whether the player on turn holds every tile move places or exchanges, as
// a queued value net move replayed from its iteration's seed must: a replay
// that diverged from its refill would not.
static bool sim_rack_holds_move(const Game *game, const Move *move) {
  Rack rack;
  rack_copy(&rack, player_get_rack(game_get_player(
                       game, game_get_player_on_turn_index(game))));
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    MachineLetter ml = move_get_tile(move, tile_idx);
    if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
        ml == PLAYED_THROUGH_MARKER) {
      continue;
    }
    if (get_is_blanked(ml)) {
      ml = BLANK_MACHINE_LETTER;
    }
    if (rack_get_letter(&rack, ml) == 0) {
      return false;
    }
    rack_take_letter(&rack, ml);
  }
  return true;
}
#endif

// The next iteration for play_index from this thread's queue, refilled when
// empty: its seed, with its value net moves copied to the worker's
// iteration_moves. Returns how many there are.
static int sim_value_net_next(Simmer *simmer, SimmerWorker *simmer_worker,
                              SimmedPlay *simmed_play, int play_index,
                              uint64_t *seed) {
  SimValueNetWorker *worker = &simmer_worker->value_net;
  const int candidates = simmer->value_net_candidates > 0
                             ? simmer->value_net_candidates
                             : SIM_VALUE_NET_DEFAULT_CANDIDATES;
  sim_value_net_worker_ensure(worker, &simmer->value_net_shared, candidates);
  if (worker->next[play_index] >= worker->count[play_index]) {
    sim_value_net_refill(simmer, simmer_worker, simmed_play, play_index);
  }
  const size_t slot = ((size_t)play_index * (size_t)worker->batch) +
                      (size_t)worker->next[play_index]++;
  *seed = worker->seeds[slot];
  const int moves = worker->move_count[slot];
  for (int ply = 0; ply < moves; ply++) {
    move_copy(&worker->iteration_moves[ply],
              &worker->moves[(slot * (size_t)worker->plies) + (size_t)ply]);
  }
  return moves;
}

double rv_sim_sample(RandomVariables *rvs, const uint64_t play_index,
                     const int thread_index, const uint64_t sample_count,
                     BAILogger __attribute__((unused)) * bai_logger) {
  Simmer *simmer = (Simmer *)rvs->data;
  SimResults *sim_results = simmer->sim_results;
  SimmedPlay *simmed_play =
      sim_results_get_simmed_play(sim_results, (int)play_index);
  // In PGP mode, rvs_thread_index = parent_worker_thread_index +
  // bai_thread_index. The local index into the workers array is always
  // bai_thread_index (0-based), while the global thread_index is used for
  // movegen to avoid cache conflicts.
  const int local_worker_index =
      thread_index - simmer->parent_worker_thread_index;
  if (local_worker_index < 0 || local_worker_index >= simmer->num_threads) {
    log_fatal("local worker index (%d) is out of bounds for simmer with "
              "num_threads %d (thread_index=%d, parent=%d)",
              local_worker_index, simmer->num_threads, thread_index,
              simmer->parent_worker_thread_index);
  }
  SimmerWorker *simmer_worker = simmer->workers[local_worker_index];
  Game *game = simmer_worker->game;
  MoveList *move_list = simmer_worker->move_list;
  const int plies = sim_results_get_num_plies(sim_results);

  // Seeding shuffles the bag, so there is no need to call bag_shuffle
  // explicitly. With value net plies the seed comes from this thread's
  // queue for the play along with the moves chosen for it.
  uint64_t seed = 0;
  int value_net_moves = 0;
  if (simmer->value_net_evaluate != NULL && plies >= 1) {
    value_net_moves = sim_value_net_next(simmer, simmer_worker, simmed_play,
                                         (int)play_index, &seed);
    game_copy(game, simmer->value_net_shared.start);
    sim_set_rollout_pat(simmer, game);
  } else {
    seed = simmed_play_get_seed(simmed_play);
  }
  const int player_off_turn_index =
      sim_start_iteration(simmer, game, simmer_worker->prng, seed);

  Equity leftover = 0;
  game_set_backup_mode(game, BACKUP_MODE_SIMULATION);
  // For one-ply sims, we need to account for the candidate move's leave value
  if (plies == 1) {
    Rack candidate_rack;
    const Player *player_on_turn =
        game_get_player(game, simmer->initial_player);
    rack_copy(&candidate_rack, player_get_rack(player_on_turn));
    leftover += get_leave_value_for_move(player_get_klv(player_on_turn),
                                         simmed_play_get_move(simmed_play),
                                         &candidate_rack);
  }
  // play move
  play_move(simmed_play_get_move(simmed_play), game, NULL);
  sim_results_increment_node_count(sim_results);
  game_set_backup_mode(game, BACKUP_MODE_OFF);
  // further plies will NOT be backed up.
  Rack spare_rack;
  for (int ply = 0; ply < plies; ply++) {
    const int player_on_turn_index = game_get_player_on_turn_index(game);
    const Player *player_on_turn = game_get_player(game, player_on_turn_index);

    if (game_over(game)) {
      break;
    }

    const Move *best_play = ply < value_net_moves
                                ? &simmer_worker->value_net.iteration_moves[ply]
                                : get_top_equity_move(game, move_list);
#ifndef NDEBUG
    if (ply < value_net_moves && !sim_rack_holds_move(game, best_play)) {
      log_fatal("a queued value net move diverged from its iteration");
    }
#endif
    rack_copy(&spare_rack, player_get_rack(player_on_turn));

    // On the final ply the resulting cross-sets are never read (no further move
    // generation happens before game_unplay_last_move restores the board), so
    // skip the cross-set update for that play.
    if (ply == plies - 1) {
      play_move_no_cross_set_update(best_play, game, NULL);
    } else {
      play_move(best_play, game, NULL);
    }
    sim_results_increment_node_count(sim_results);
    if (ply == plies - 2 || ply == plies - 1) {
      Equity this_leftover = get_leave_value_for_move(
          player_get_klv(player_on_turn), best_play, &spare_rack);
      if (player_on_turn_index == simmer->initial_player) {
        leftover += this_leftover;
      } else {
        leftover -= this_leftover;
      }
    }
    simmed_play_add_stats_for_ply(simmed_play, ply, best_play);
  }

  const Equity spread =
      player_get_score(game_get_player(game, simmer->initial_player)) -
      player_get_score(game_get_player(game, 1 - simmer->initial_player));
  const int horizon_on_turn_index = game_get_player_on_turn_index(game);
  const int bag_tiles = bag_get_letters(game_get_bag(game));
  const int on_turn_rack_tiles = rack_get_total_letters(
      player_get_rack(game_get_player(game, horizon_on_turn_index)));
  const int off_turn_rack_tiles = rack_get_total_letters(
      player_get_rack(game_get_player(game, 1 - horizon_on_turn_index)));
  const game_end_reason_t game_end_reason = game_get_game_end_reason(game);
  // With the margin forecast, the equity and the utility's spread term use
  // the projected final margin: the horizon spread and leave values plus the
  // expected swing from the horizon's bag and rack sizes, which carries who is
  // likely to get the last turns. The win% lookup already accounts for the
  // state and still uses the horizon spread.
  Equity projected_leftover = leftover;
  if (simmer->use_margin_forecast && game_end_reason == GAME_END_REASON_NONE) {
    Equity expected_swing = win_pct_get_expected_swing(
        simmer->win_pcts, (unsigned int)bag_tiles,
        (unsigned int)on_turn_rack_tiles, (unsigned int)off_turn_rack_tiles);
    if (horizon_on_turn_index != simmer->initial_player) {
      expected_swing = -expected_swing;
    }
    projected_leftover = leftover + expected_swing;
  }
  simmed_play_add_equity_stat(simmed_play, simmer->initial_spread, spread,
                              projected_leftover);
  const double wpct = simmed_play_add_win_pct_stat(
      simmer->win_pcts, simmed_play, spread, leftover, game_end_reason,
      bag_tiles, on_turn_rack_tiles, off_turn_rack_tiles, plies % 2);
  // reset to first state. we only need to restore one backup.
  game_unplay_last_move(game);
  return_rack_to_bag(game, player_off_turn_index);

  if (simmer->print_interval > 0 &&
      sample_count % simmer->print_interval == 0) {
    sim_results_print(simmer->thread_control, simmer_worker->game,
                      simmer->sim_results, simmer->max_num_display_plays,
                      simmer->max_num_display_plies, true, false, NULL);
  }
  sim_results_increment_iteration_count(sim_results);

  const Equity utility_spread =
      simmer->use_margin_forecast ? spread + projected_leftover : spread;
  const double utility =
      sim_utility_blend(wpct, utility_spread, simmer->utility_w_winpct,
                        simmer->utility_w_spread, simmer->utility_spread_scale);
  // With a zero spread weight, utility_stat is never read: BU is hidden from
  // display (see show_bu in sim_string.c) and both the best-move choice and
  // sort comparator fall back to win_pct_stat/equity_stat instead (see
  // sim_results_get_best_move and compare_simmed_plays). Skip the extra
  // mutex lock + stat_push on this hot path in that case.
  if (simmer->utility_w_spread > 0.0) {
    simmed_play_add_utility_stat(simmed_play, utility);
  }
  return utility;
}

static int rv_sim_get_best_arm_index(const RandomVariables *rvs) {
  const Simmer *simmer = (const Simmer *)rvs->data;
  return sim_results_get_best_move_index(simmer->sim_results);
}

bool rv_sim_are_similar(RandomVariables *rvs, const int i, const int j) {
  const Simmer *simmer = (Simmer *)rvs->data;
  return sim_results_plays_are_similar(simmer->sim_results, i, j);
}

void rv_sim_destroy(RandomVariables *rvs) {
  Simmer *simmer = (Simmer *)rvs->data;
  sim_value_net_shared_free(&simmer->value_net_shared);
  rack_destroy(simmer->known_opp_rack);
  for (int thread_index = 0; thread_index < simmer->num_threads;
       thread_index++) {
    simmer_worker_destroy(simmer->workers[thread_index]);
  }
  free(simmer->workers);
  free(simmer);
}

// True when the sim args request a resume and the existing results are
// compatible (same play and ply counts). The reset is skipped in that
// case so sampling keeps accumulating onto the existing per-play stats
// — the move list is expected to hold the same plays the SimResults
// was built from (sampling reads moves from the SimmedPlays).
static bool rv_sim_can_resume(const SimArgs *sim_args,
                              const SimResults *sim_results) {
  return sim_args->resume_results &&
         sim_results_get_number_of_plays(sim_results) ==
             move_list_get_count(sim_args->move_list) &&
         sim_results_get_num_plies(sim_results) == sim_args->num_plies;
}

// Takes sim_args' value net rollout settings (SimArgs.rollout_value_net_*)
// and rollout PAT settings, and prepares the value net iterations' start.
static void simmer_set_value_net(Simmer *simmer, const SimArgs *sim_args) {
  simmer->value_net_evaluate = sim_args->rollout_value_net_evaluate;
  simmer->value_net_context = sim_args->rollout_value_net_context;
  simmer->value_net_candidates = sim_args->rollout_value_net_candidates;
  simmer->value_net_batch = sim_args->rollout_value_net_batch;
  simmer->value_net_history = sim_args->rollout_value_net_history;
  simmer->value_net_own_history = sim_args->rollout_value_net_own_history;
  simmer->pat_rollout_disabled = sim_args->pat_rollout_disabled;
  simmer->pat_rollout_disabled_classes_mask =
      sim_args->pat_rollout_disabled_classes_mask;
  if (simmer->value_net_evaluate == NULL) {
    return;
  }
  int plies = sim_args->rollout_value_net_plies;
  if (plies > sim_args->num_plies) {
    plies = sim_args->num_plies;
  }
  if (plies < 1) {
    plies = 1;
  }
  sim_value_net_shared_prepare(
      &simmer->value_net_shared, move_list_get_count(sim_args->move_list),
      simmer->value_net_batch > 0 ? simmer->value_net_batch
                                  : SIM_VALUE_NET_DEFAULT_BATCH,
      plies, sim_args->game);
}

RandomVariables *rv_sim_create(RandomVariables *rvs, const SimArgs *sim_args,
                               SimResults *sim_results) {
  rvs->sample_func = rv_sim_sample;
  rvs->similar_func = rv_sim_are_similar;
  rvs->destroy_data_func = rv_sim_destroy;
  rvs->get_best_arm_index_func = rv_sim_get_best_arm_index;

  rvs->num_rvs = move_list_get_count(sim_args->move_list);

  Simmer *simmer = calloc_or_die(1, sizeof(Simmer));
  ThreadControl *thread_control = sim_args->thread_control;

  simmer->initial_player = game_get_player_on_turn_index(sim_args->game);
  const Player *player =
      game_get_player(sim_args->game, simmer->initial_player);
  const Player *opponent =
      game_get_player(sim_args->game, 1 - simmer->initial_player);

  simmer->initial_spread =
      player_get_score(player) - player_get_score(opponent);

  const Rack *known_opp_rack = sim_args->known_opp_rack;
  if (known_opp_rack && !rack_is_empty(known_opp_rack)) {
    simmer->known_opp_rack = rack_duplicate(known_opp_rack);
  } else {
    simmer->known_opp_rack = NULL;
  }

  simmer->dist_size = ld_get_size(game_get_ld(sim_args->game));

  simmer->num_threads = sim_args->num_threads;
  simmer->parent_worker_thread_index =
      sim_args->bai_options.parent_worker_thread_index;
  simmer->print_interval = sim_args->print_interval;
  simmer->max_num_display_plays = sim_args->max_num_display_plays;
  simmer->max_num_display_plies = sim_args->max_num_display_plies;

  simmer->workers =
      malloc_or_die((sizeof(SimmerWorker *)) * (simmer->num_threads));
  for (int thread_index = 0; thread_index < simmer->num_threads;
       thread_index++) {
    simmer->workers[thread_index] = simmer_create_worker(sim_args);
  }

  simmer->win_pcts = sim_args->win_pcts;
  simmer->use_inference = sim_args->use_inference;
  simmer->use_alias_method =
      simmer->use_inference &&
      (!simmer->known_opp_rack || rack_is_empty(simmer->known_opp_rack));
  simmer->inference_results = sim_args->inference_results;

  simmer->utility_w_winpct = sim_args->utility_w_winpct;
  simmer->utility_w_spread = sim_args->utility_w_spread;
  simmer->utility_spread_scale = sim_args->utility_spread_scale;
  simmer->use_margin_forecast = sim_args->use_margin_forecast;
  simmer_set_value_net(simmer, sim_args);

  simmer->thread_control = thread_control;

  if (!rv_sim_can_resume(sim_args, sim_results)) {
    sim_results_reset(sim_args->move_list, sim_results, sim_args->num_plies,
                      sim_args->seed, sim_args->use_heat_map);
  }
  simmer->sim_results = sim_results;

  rvs->data = simmer;
  return rvs;
}

void rv_sim_reset(RandomVariables *rvs, const SimArgs *sim_args) {
  Simmer *simmer = (Simmer *)rvs->data;
  rvs->num_rvs = move_list_get_count(sim_args->move_list);

  simmer->initial_player = game_get_player_on_turn_index(sim_args->game);
  const Player *player =
      game_get_player(sim_args->game, simmer->initial_player);
  const Player *opponent =
      game_get_player(sim_args->game, 1 - simmer->initial_player);

  simmer->initial_spread =
      player_get_score(player) - player_get_score(opponent);

  const Rack *known_opp_rack = sim_args->known_opp_rack;
  if (known_opp_rack && !rack_is_empty(known_opp_rack)) {
    if (!simmer->known_opp_rack) {
      simmer->known_opp_rack = rack_duplicate(known_opp_rack);
    } else {
      rack_copy(simmer->known_opp_rack, known_opp_rack);
    }
  } else {
    if (simmer->known_opp_rack) {
      // FIXME: avoid repeated alloc/free if resetting multiple times
      rack_destroy(simmer->known_opp_rack);
    }
    simmer->known_opp_rack = NULL;
  }

  for (int thread_index = 0; thread_index < simmer->num_threads;
       thread_index++) {
    simmer_reset_worker(simmer->workers[thread_index], sim_args);
  }

  simmer->win_pcts = sim_args->win_pcts;
  simmer->use_inference = sim_args->use_inference;
  simmer->use_alias_method =
      simmer->use_inference &&
      (!simmer->known_opp_rack || rack_is_empty(simmer->known_opp_rack));

  simmer->utility_w_winpct = sim_args->utility_w_winpct;
  simmer->utility_w_spread = sim_args->utility_w_spread;
  simmer->utility_spread_scale = sim_args->utility_spread_scale;
  simmer->use_margin_forecast = sim_args->use_margin_forecast;
  simmer_set_value_net(simmer, sim_args);

  if (!rv_sim_can_resume(sim_args, simmer->sim_results)) {
    sim_results_reset(sim_args->move_list, simmer->sim_results,
                      sim_args->num_plies, sim_args->seed,
                      sim_args->use_heat_map);
  }
}

RandomVariables *rvs_create(const RandomVariablesArgs *rvs_args) {
  RandomVariables *rvs = malloc_or_die(sizeof(RandomVariables));
  // The num_rvs field will be overwritten in the rv_sim_create function
  // since it is cumbersome and unnecessary for the caller to set
  // rvs_args->num_rvs for simmed plays.
  rvs->num_rvs = rvs_args->num_rvs;
  atomic_store(&rvs->total_samples, 0);
  switch (rvs_args->type) {
  case RANDOM_VARIABLES_UNIFORM:
    rv_uniform_create(rvs, rvs_args->seed);
    break;
  case RANDOM_VARIABLES_UNIFORM_PREDETERMINED:
    rv_uniform_predetermined_create(rvs, rvs_args->samples,
                                    rvs_args->num_samples);
    break;
  case RANDOM_VARIABLES_NORMAL:
    rv_normal_create(rvs, rvs_args->seed, rvs_args->means_and_vars);
    break;
  case RANDOM_VARIABLES_NORMAL_PREDETERMINED:
    rv_normal_predetermined_create(rvs, rvs_args->samples,
                                   rvs_args->num_samples,
                                   rvs_args->means_and_vars);
    break;
  case RANDOM_VARIABLES_SIMMED_PLAYS:
    rv_sim_create(rvs, rvs_args->sim_args, rvs_args->sim_results);
    break;
  }
  return rvs;
}

void rvs_reset(RandomVariables *rvs, const RandomVariablesArgs *rvs_args) {
  rvs->num_rvs = rvs_args->num_rvs;
  atomic_store(&rvs->total_samples, 0);
  switch (rvs_args->type) {
  case RANDOM_VARIABLES_UNIFORM:
    rv_uniform_reset(rvs, rvs_args->seed);
    break;
  case RANDOM_VARIABLES_UNIFORM_PREDETERMINED:
    rv_uniform_predetermined_reset(rvs);
    break;
  case RANDOM_VARIABLES_NORMAL:
    rv_normal_reset(rvs, rvs_args->seed);
    break;
  case RANDOM_VARIABLES_NORMAL_PREDETERMINED:
    rv_normal_predetermined_reset(rvs);
    break;
  case RANDOM_VARIABLES_SIMMED_PLAYS:
    rv_sim_reset(rvs, rvs_args->sim_args);
    break;
  }
}

void rvs_destroy(RandomVariables *rvs) {
  if (!rvs) {
    return;
  }
  if (rvs->destroy_data_func == NULL) {
    return;
  }
  rvs->destroy_data_func(rvs);
  free(rvs);
}

double rvs_sample(RandomVariables *rvs, const uint64_t k,
                  const int thread_index, BAILogger *bai_logger) {
  uint64_t prev_total_samples = atomic_fetch_add(&rvs->total_samples, 1);
  return rvs->sample_func(rvs, k, thread_index, prev_total_samples + 1,
                          bai_logger);
}

bool rvs_are_similar(RandomVariables *rvs, const int i, const int j) {
  return rvs->similar_func(rvs, i, j);
}

uint64_t rvs_get_num_rvs(const RandomVariables *rvs) { return rvs->num_rvs; }

// NOT THREAD SAFE: caller is responsible for ensuring thread safety.
uint64_t rvs_get_total_samples(const RandomVariables *rvs) {
  return rvs->total_samples;
}

int rvs_get_best_arm_index(const RandomVariables *rvs) {
  return rvs->get_best_arm_index_func(rvs);
}