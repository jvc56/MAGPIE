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
  // Whether the sim of the candidates continues the samples the search took
  // of them, instead of starting over with other seeds.
  bool reuse_samples;
  lamc_proposal_t proposal;
  gp_kernel_t kernel;
  // The surrogate's length scale, in standard deviations of each feature. Fits
  // choose among multiples of it by marginal likelihood.
  double length_scale;
  // The ascent step, in the same units.
  double trust_radius;
  // The weight of the standard deviation in the acquire proposal.
  double ucb_beta;
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
  args->reuse_samples = true;
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
