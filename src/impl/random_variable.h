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