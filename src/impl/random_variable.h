#ifndef RANDOM_VARIABLE_H
#define RANDOM_VARIABLE_H

#include "../def/bai_defs.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/inference_results.h"
#include "../ent/rack.h"
#include "../ent/sim_args.h"
#include "../ent/sim_results.h"
#include "../ent/thread_control.h"
#include "bai_logger.h"
#include "inference.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

typedef struct RandomVariables RandomVariables;

// The rotations a bag-cycled sample (SimArgs.bag_cycle) plays from game: the
// letters unseen by the player on turn (the bag and the opponent's rack)
// divided by RACK_SIZE, rounded up, and at least 1.
int rv_sim_bag_cycle_rotations(const Game *game);

typedef enum {
  RANDOM_VARIABLES_UNIFORM,
  RANDOM_VARIABLES_UNIFORM_PREDETERMINED,
  RANDOM_VARIABLES_NORMAL,
  RANDOM_VARIABLES_NORMAL_PREDETERMINED,
  RANDOM_VARIABLES_SIMMED_PLAYS,
  // A view of some of another random variables' arms: arm k is the base's
  // arm arm_indices[k]. Samples, seeds and records go to the base, so an
  // arm's samples continue its own seed sequence from one view to the next.
  // Locally Adaptive Monte Carlo (lamc.h) samples a pool of simmed plays
  // this way.
  RANDOM_VARIABLES_SUBSET,
  // Arm k's i-th sample is samples[k * num_samples + i]: recorded samples,
  // replayed. Running out of an arm's samples is fatal.
  RANDOM_VARIABLES_TABLE,
} random_variables_t;

// Per base arm: the count, sum and sum of squares of the samples a subset view
// took, the sums in BAI's fixed point (BAI_FIXED_POINT_SCALE), so that they do
// not depend on the order threads finish samples in. The caller owns the
// arrays, sized for the base's arms, and clears them.
typedef struct RVSubsetStats {
  atomic_uint_least64_t *counts;
  atomic_int_least64_t *sums;
  atomic_int_least64_t *squares;
} RVSubsetStats;

typedef struct RandomVariablesArgs {
  random_variables_t type;
  uint64_t num_rvs;
  uint64_t seed;
  uint64_t num_samples;
  const double *samples;
  const double *means_and_vars;
  const SimArgs *sim_args;
  SimResults *sim_results;
  // RANDOM_VARIABLES_SUBSET only: the base and the num_rvs arm indices into
  // it, which the caller owns and keeps unchanged while the view is in use.
  RandomVariables *base_rvs;
  const int *arm_indices;
  // RANDOM_VARIABLES_SUBSET only: where to accumulate the samples taken, or
  // NULL.
  RVSubsetStats *subset_stats;
} RandomVariablesArgs;

RandomVariables *rvs_create(const RandomVariablesArgs *rvs_args);
void rvs_reset(RandomVariables *rvs, const RandomVariablesArgs *rvs_args);
void rvs_destroy(RandomVariables *rvs);
// Sentinel meaning "no seed was reserved for this sample; draw one".
#define RVS_SEED_UNRESERVED UINT64_MAX

double rvs_sample(RandomVariables *rvs, uint64_t k, int thread_index,
                  BAILogger *bai_logger);
double rvs_sample_with_seed(RandomVariables *rvs, uint64_t k,
                            uint64_t reserved_seed, int thread_index,
                            BAILogger *bai_logger, void *record);
// Bytes in the record a sample can write instead of applying its effect on
// the random variable's results, or 0 when samples have no such effect.
size_t rvs_get_sample_record_size(const RandomVariables *rvs);
// Applies a record written by rvs_sample_with_seed for arm k.
void rvs_apply_sample_record(const RandomVariables *rvs, uint64_t k,
                             const void *record);
uint64_t rvs_next_seed(RandomVariables *rvs, uint64_t k);
// True when every arm's n-th sample uses the same seed, so that samples of
// different arms at one ordinal share their luck (common random numbers).
// Sims' samples do; the test distributions' don't.
bool rvs_arms_share_seeds(const RandomVariables *rvs);
// The seed rvs_next_seed would return for arm k after `ahead` more calls,
// without advancing it, or RVS_SEED_UNRESERVED when this kind of random
// variable does not draw from seeds. A sample drawn with a peeked seed is the
// sample the slot reserving that seed would produce.
uint64_t rvs_peek_seed(RandomVariables *rvs, uint64_t k, uint64_t ahead);
bool rvs_are_similar(RandomVariables *rvs, int i, int j);
uint64_t rvs_get_num_rvs(const RandomVariables *rvs);
uint64_t rvs_get_total_samples(const RandomVariables *rvs);
int rvs_get_best_arm_index(const RandomVariables *rvs);

#endif