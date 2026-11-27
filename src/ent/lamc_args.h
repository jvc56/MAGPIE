#ifndef LAMC_ARGS_H
#define LAMC_ARGS_H

#include "../def/lamc_defs.h"
#include "../def/play_feature_defs.h"
#include <stdbool.h>

typedef struct LamcArgs {
  bool enabled;
  // P: the plays sampled each round.
  int pool_size;
  // K: the plays retained from one round to the next.
  int keep;
  // Of the first pool, the plays taken in static equity order; the rest
  // spread over the embedding.
  int seed_plays;
  // I_shallow: samples per play per round.
  int shallow_samples;
  // The cap on rounds.
  int max_rounds;
  // Exploring stops once the top K is unchanged for this many rounds.
  int stable_rounds;
  // The share of the sim's rollouts, and of its time limit, exploring may use.
  double explore_share;
  // Plays more than this many points of static equity below the best are not
  // considered.
  double equity_floor;
  // The most plays considered, best by static equity first.
  int max_universe;
  // The most plays evaluated: the surrogate's capacity.
  int max_evaluated;
  // The search runs only when the bag holds at most this many tiles.
  int max_bag;
  // The plays handed to the sim; 0 hands it as many as it would simulate
  // without LAMC.
  int num_finalists;
  // Whether exploring samples cycle the bag as the sim's do (sbagcycle).
  bool explore_bag_cycle;
  // Whether the surrogate's prior mean is linear in static equity, not
  // constant.
  bool equity_prior_mean;
  // Whether candidates come from every play by the surrogate's mean, not only
  // the evaluated ones.
  bool finalists_from_all;
  // The candidates always include this many top plays by static equity.
  int static_finalists;
  // Then this many exchanges, the best by the selection score below, so
  // that a prior linear in static equity cannot leave every exchange out.
  int final_exchanges;
  // Whether the sim of the candidates continues the samples the search took
  // of them, instead of starting over with other seeds. Off by default: BAI
  // starts its own statistics from zero, so it can set aside a play after a
  // few poor samples of its own while the sim's ranking, which also holds
  // the search's samples, still puts it first. The search keeps plays whose
  // samples were lucky, so such a play is often chosen on a handful of
  // samples (docs/lamc-plan.md, "Letting BAI count the search's samples").
  bool reuse_samples;
  // Whether a search that ends with rollouts left in its share spends them
  // on the plays whose place relative to the cut for the candidates is least
  // certain: rounds of shallow_samples for the pool_size plays whose
  // surrogate means are fewest standard deviations from the cut. On by
  // default: without it the search may stop after a fraction of its share,
  // with a few samples a play, and miss strong plays that static equity
  // ranks low, such as exchanges (RLAMC.md, log 78).
  bool refine;
  lamc_proposal_t proposal;
  gp_kernel_t kernel;
  // The surrogate's length scale, in standard deviations of each feature. Fits
  // choose among multiples of it by marginal likelihood.
  double length_scale;
  // The ascent step, in the same units.
  double trust_radius;
  // The weight of the standard deviation in the acquire proposal.
  double ucb_beta;
  // The candidates after the top static plays are the best by the
  // surrogate's mean plus final_z of its standard deviations: a play
  // sampled a few times with poor luck stays in while it may be good.
  double final_z;
  // Indexed by play_feature_t.
  bool features[NUMBER_OF_PLAY_FEATURES];
} LamcArgs;

// LAMC off, with every other field at its default. The features are left to
// the caller (see play_feature_parse_set and PLAY_FEATURE_DEFAULT_SET).
static inline void lamc_args_set_defaults(LamcArgs *args) {
  args->enabled = false;
  args->pool_size = LAMC_DEFAULT_POOL_SIZE;
  args->keep = LAMC_DEFAULT_KEEP;
  args->seed_plays = LAMC_DEFAULT_SEED_PLAYS;
  args->shallow_samples = LAMC_DEFAULT_SHALLOW_SAMPLES;
  args->max_rounds = LAMC_DEFAULT_MAX_ROUNDS;
  args->stable_rounds = LAMC_DEFAULT_STABLE_ROUNDS;
  args->explore_share = LAMC_DEFAULT_EXPLORE_SHARE;
  args->equity_floor = LAMC_DEFAULT_EQUITY_FLOOR;
  args->max_universe = LAMC_DEFAULT_MAX_UNIVERSE;
  args->max_evaluated = LAMC_DEFAULT_MAX_EVALUATED;
  args->max_bag = LAMC_DEFAULT_MAX_BAG;
  args->num_finalists = 0;
  args->explore_bag_cycle = false;
  args->equity_prior_mean = true;
  args->finalists_from_all = true;
  args->static_finalists = LAMC_DEFAULT_STATIC_FINALISTS;
  args->final_exchanges = 0;
  args->refine = true;
  args->final_z = 0.0;
  args->reuse_samples = false;
  args->proposal = LAMC_PROPOSAL_ACQUIRE;
  args->kernel = GP_KERNEL_MATERN52;
  args->length_scale = LAMC_DEFAULT_LENGTH_SCALE;
  args->trust_radius = LAMC_DEFAULT_TRUST_RADIUS;
  args->ucb_beta = LAMC_DEFAULT_UCB_BETA;
  for (int feature_idx = 0; feature_idx < NUMBER_OF_PLAY_FEATURES;
       feature_idx++) {
    args->features[feature_idx] = false;
  }
}

#endif
