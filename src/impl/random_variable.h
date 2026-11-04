#ifndef RANDOM_VARIABLE_H
#define RANDOM_VARIABLE_H

#include "../def/bai_defs.h"
#include "../ent/best_move_carry.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/inference_results.h"
#include "../ent/move.h"
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

// An experiment's own move generation for sim rollouts, in place of the
// sim's. Each sim worker keeps a state the hooks create for it, and the hooks
// see the start of each sim and each rollout's candidate. A NULL hook leaves
// that step as it is; best_move NULL generates as the sim does.
typedef struct SimRolloutHooks {
  void *context;
  void *(*create_worker)(void *context);
  void (*destroy_worker)(void *context, void *worker);
  // At the start of a sim, on the thread that starts it, before any rollout.
  void (*prepare)(void *context, const Game *game, const MoveList *candidates);
  // In a rollout: the game before its candidate is played, and after.
  void (*before_candidate)(void *worker, const Game *game);
  void (*after_candidate)(void *worker, const Game *game,
                          const Move *candidate);
  // The best move of the player on turn, ply 0 being the reply to the
  // candidate; carry is the sim's best-move carry.
  const Move *(*best_move)(void *worker, Game *game, MoveList *move_list,
                           BestMoveCarry *carry, int ply);
  // The buckets of the sim's best-move carry, a power of two, or 0 for
  // SIM_BEST_MOVE_CARRY_BUCKETS.
  int carry_buckets;
} SimRolloutHooks;

// The hooks the sims created or reset from now on use, or NULL for none.
// For experiments and tests; set it with no sim running.
void rv_sim_set_rollout_hooks(const SimRolloutHooks *hooks);

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