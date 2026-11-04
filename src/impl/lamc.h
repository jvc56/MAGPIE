#ifndef LAMC_H
#define LAMC_H

#include "../ent/lamc_args.h"
#include "../ent/move.h"
#include "../ent/sim_args.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include "random_variable.h"
#include <stdbool.h>
#include <stdint.h>

// Locally Adaptive Monte Carlo: chooses the plays a sim simulates by searching
// all of the position's plays instead of taking the top ones by static
// equity. See docs/lamc-plan.md.
//
// Every play is embedded in a feature space (play_feature_registry.h). A pool
// of plays spread over that space gets a few rollouts each; a Gaussian
// process (gp.h) fit to their win percentages predicts the rest; the best
// plays by its prediction are kept, and the others are replaced by plays the
// prediction favors. When the search settles, its best plays by prediction
// are the sim's candidates, simulated as usual with what is left of the
// budget.

typedef struct LamcCtx LamcCtx;

LamcCtx *lamc_ctx_create(void);
void lamc_ctx_destroy(LamcCtx *ctx);

// The steps lamc_choose_candidates takes, for tests that replay recorded
// samples instead of rolling out.

// Generates the plays of sim_args' position within the equity floor, best by
// static equity first, and embeds them. Returns how many there are.
int lamc_build(LamcCtx *ctx, const SimArgs *sim_args);
const MoveList *lamc_get_universe(const LamcCtx *ctx);

typedef struct LamcSearchLimits {
  // UINT64_MAX for none.
  uint64_t rollout_budget;
  // 0 for none.
  double time_budget_seconds;
  // The rollouts one sample averages: a bag cycle's rotations, or 1.
  uint64_t rollouts_per_sample;
  int num_threads;
  int parent_worker_thread_index;
  ThreadControl *thread_control;
} LamcSearchLimits;

// Searches the built plays, sampling base_rvs, whose arm k is the built play
// k, a pool at a time. Returns the rounds it ran.
int lamc_search(LamcCtx *ctx, const LamcArgs *args, RandomVariables *base_rvs,
                const LamcSearchLimits *limits);
int lamc_get_num_evaluated(const LamcCtx *ctx);
// The built play (its index) at the rank-th best surrogate mean of the
// evaluated plays.
int lamc_get_ranked_play(const LamcCtx *ctx, int rank);
// Chooses up to count candidates after a search: the best by the surrogate's
// mean among the evaluated plays, or among all plays if the args say so.
// Returns how many it chose.
int lamc_select_candidates(LamcCtx *ctx, const LamcArgs *args, int count);
// The built play (its index) of the rank-th candidate.
int lamc_get_candidate(const LamcCtx *ctx, int rank);
// The rollouts the last search took.
uint64_t lamc_get_search_rollouts(const LamcCtx *ctx);

// What one search did.
typedef struct LamcReport {
  // The plays the search considered and evaluated.
  int num_universe;
  int num_evaluated;
  int num_rounds;
  uint64_t explore_rollouts;
  double explore_seconds;
  // The candidates it chose that are not among the plays a sim without LAMC
  // would simulate.
  int num_finalists;
  int finalists_outside_static;
} LamcReport;

// Searches the position of sim_args, whose inference (if any) has already
// run, and returns its candidates, or NULL when the search does not apply:
// the position has no more plays than the sim would simulate anyway. The
// candidates are at most baseline_plays plays, unless the LAMC args ask for
// another number, and stay valid until the next search with the same ctx.
// The search's rollouts and time are in the report.
const MoveList *lamc_choose_candidates(const SimArgs *sim_args, LamcCtx **ctx,
                                       int baseline_plays, LamcReport *report);

// Totals over every search since the last reset, for experiments: the
// searches, their rollouts, and how often the sim then chose a play outside
// the top baseline plays by static equity.
typedef struct LamcTotals {
  uint64_t searches;
  uint64_t rounds;
  uint64_t evaluated;
  uint64_t explore_rollouts;
  uint64_t explore_microseconds;
  uint64_t finalists_outside_static;
  uint64_t chosen_outside_static;
} LamcTotals;

// Records the sim's choice after a search: whether it is outside the top
// baseline_plays of the search's plays by static equity.
void lamc_note_choice(const LamcCtx *ctx, const Move *chosen,
                      int baseline_plays);
void lamc_totals_reset(void);
LamcTotals lamc_get_totals(void);

#endif
