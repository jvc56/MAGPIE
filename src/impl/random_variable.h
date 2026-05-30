#ifndef RANDOM_VARIABLE_H
#define RANDOM_VARIABLE_H

#include "../def/bai_defs.h"
#include "../def/sim_defs.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/inference_results.h"
#include "../ent/move.h"
#include "../ent/rack.h"
#include "../ent/sim_args.h"
#include "../ent/sim_results.h"
#include "../ent/thread_control.h"
#include "../ent/win_pct.h"
#include "bai_logger.h"
#include "inference.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct RandomVariables RandomVariables;

typedef enum {
  RANDOM_VARIABLES_UNIFORM,
  RANDOM_VARIABLES_UNIFORM_PREDETERMINED,
  RANDOM_VARIABLES_NORMAL,
  RANDOM_VARIABLES_NORMAL_PREDETERMINED,
  RANDOM_VARIABLES_SIMMED_PLAYS,
} random_variables_t;

typedef struct RandomVariablesArgs {
  random_variables_t type;
  uint64_t num_rvs;
  uint64_t seed;
  uint64_t num_samples;
  const double *samples;
  const double *means_and_vars;
  const SimArgs *sim_args;
  SimResults *sim_results;
} RandomVariablesArgs;

RandomVariables *rvs_create(const RandomVariablesArgs *rvs_args);
void rvs_reset(RandomVariables *rvs, const RandomVariablesArgs *rvs_args);
void rvs_destroy(RandomVariables *rvs);
double rvs_sample(RandomVariables *rvs, uint64_t k, int thread_index,
                  BAILogger *bai_logger);
bool rvs_are_similar(RandomVariables *rvs, int i, int j);
uint64_t rvs_get_num_rvs(const RandomVariables *rvs);
uint64_t rvs_get_total_samples(const RandomVariables *rvs);
int rvs_get_best_arm_index(const RandomVariables *rvs);
void rvs_set_fidelity_level(RandomVariables *rvs, const FidelityLevel *level);

// Aggregate diagnostics collected across all SimmerWorkers, reset at the
// start of each simulate() via simmer_reset_worker. Used by analysis tests
// to characterize how often the inner BAI sim disagrees with the top-equity
// candidate, and how much utility is at stake when it does.
//
//   calls         = number of inner-sim invocations
//   rollouts      = total inner mini-rollouts across all calls
//   early_stops   = inner calls that hit the 99% Welch stop before max
//   agree_count   = inner calls where BAI picked cand[0] (top equity)
//   loss_sum      = sum over calls of (util[BAI_pick] - util[cand_0])
//   loss_sum_sq   = sum of squared losses (for stddev)
//   loss_max      = largest single-call loss (always >= 0; BAI never picks
//                   a candidate with utility < cand_0's by construction)
typedef struct InnerDiag {
  uint64_t calls;
  uint64_t rollouts;
  uint64_t early_stops;
  uint64_t agree_count;
  double loss_sum;
  double loss_sum_sq;
  double loss_max;
} InnerDiag;

void rvs_get_inner_diag(const RandomVariables *rvs, InnerDiag *out);

// Exposed for test use: the production inner-sim machinery. SimmerWorker
// owns scratch buffers (nested_game, nested_move_list, candidate_move_list).
typedef struct SimmerWorker SimmerWorker;

SimmerWorker *simmer_create_worker(const Game *game,
                                   ply_strategy_t ply_strategy,
                                   int nested_candidates, int thread_index,
                                   int num_sim_threads);
void simmer_worker_destroy(SimmerWorker *simmer_worker);
// Re-seed the worker's internal PRNG (drives CRN base in
// get_top_nested_sim_move). Used by repeatability tests.
void simmer_worker_seed_prng(SimmerWorker *worker, uint64_t seed);

// Production inner-sim entry point. Generates top-K candidates by equity,
// runs phase-1 floor (num_rollouts per arm) + phase-2 TOP_TWO up to
// max_samples total or early-stop on Welch t >= stop_z (0 disables).
// Returns picked Move (pointer into worker's candidate_move_list, valid
// until the next call on this worker).
Move *get_top_nested_sim_move(Game *game, int thread_index,
                              SimmerWorker *worker, const WinPct *win_pcts,
                              int num_rollouts, int num_plies, int max_samples,
                              double stop_z, double inner_w_winpct,
                              double inner_w_spread, double inner_spread_scale,
                              double *best_wpct_out);

// Single mini-rollout primitive. CRN-friendly: passing the same seed for the
// same rollout index across different candidates produces paired draws.
void run_inner_rollout(Game *game, int thread_index, SimmerWorker *worker,
                       const Move *candidate, const WinPct *win_pcts,
                       int num_plies, int player_index, bool plies_are_odd,
                       uint64_t seed, double *wpct_out, double *spread_out);

#endif