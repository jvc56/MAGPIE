#include "simmer.h"

#include "../def/game_defs.h"
#include "../def/inference_defs.h"
#include "../def/sim_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/bag.h"
#include "../ent/game.h"
#include "../ent/inference_results.h"
#include "../ent/move.h"
#include "../ent/sim_args.h"
#include "../ent/sim_results.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "bai.h"
#include "inference.h"
#include "random_variable.h"
#include <stdint.h>
#include <stdlib.h>

// The sim context allows zero-alloc autoplay sims by maintaining the
// dynamically allocated sim structs in between sim calls. The following
// options are allowed to change in between sim calls:
// - move list
// - player on turn
// - spread
// - known opp rack
// - use inference
// - num plies
struct SimCtx {
  RandomVariables *rvs;
  RandomVariables *rng;
  InferenceCtx *inference_ctx;
};

void sim_ctx_destroy(SimCtx *sim_ctx) {
  if (!sim_ctx) {
    return;
  }
  rvs_destroy(sim_ctx->rvs);
  rvs_destroy(sim_ctx->rng);
  inference_ctx_destroy(sim_ctx->inference_ctx);
  free(sim_ctx);
}

void simulate(SimArgs *sim_args, SimCtx **sim_ctx, SimResults *sim_results,
              ErrorStack *error_stack) {
  if (!sim_args->move_list || move_list_get_count(sim_args->move_list) == 0) {
    error_stack_push(error_stack, ERROR_STATUS_SIM_NO_MOVES,
                     string_duplicate("cannot simulate without moves, use the "
                                      "'generate' command to generate moves"));
    return;
  }
  if (game_get_game_end_reason(sim_args->game) != GAME_END_REASON_NONE) {
    error_stack_push(
        error_stack, ERROR_STATUS_SIM_GAME_OVER,
        string_duplicate("cannot simulate when the game is already over"));
    return;
  }

  // If the bag is empty, set sample_limit to the number of moves and
  // sample_minimum to 1 for endgame simulations
  if (bag_is_empty(game_get_bag(sim_args->game))) {
    sim_args->bai_options.sample_limit =
        move_list_get_count(sim_args->move_list);
    sim_args->bai_options.sample_minimum = 1;
    sim_args->num_plies = MAX_PLIES;
  }

  if (*sim_ctx == NULL) {
    *sim_ctx = malloc_or_die(sizeof(SimCtx));
    (*sim_ctx)->rvs = NULL;
    (*sim_ctx)->rng = NULL;
    (*sim_ctx)->inference_ctx = NULL;
  }

  uint64_t num_infer_leaves = 0;
  if (sim_args->use_inference) {
    infer(&sim_args->inference_args, &((*sim_ctx)->inference_ctx),
          sim_args->inference_results, error_stack);
    if (!error_stack_is_empty(error_stack) ||
        thread_control_get_status(sim_args->thread_control) !=
            THREAD_CONTROL_STATUS_STARTED) {
      return;
    }
    num_infer_leaves =
        stat_get_num_unique_samples(inference_results_get_equity_values(
            sim_args->inference_results, INFERENCE_TYPE_LEAVE));
  }

  RandomVariablesArgs rv_sim_args = {
      .type = RANDOM_VARIABLES_SIMMED_PLAYS,
      .sim_args = sim_args,
      .sim_results = sim_results,
  };

  RandomVariablesArgs rng_args = {
      .type = RANDOM_VARIABLES_UNIFORM,
      .seed = sim_args->seed,
  };

  if ((*sim_ctx)->rvs) {
    rvs_reset((*sim_ctx)->rvs, &rv_sim_args);
    rvs_reset((*sim_ctx)->rng, &rng_args);
  } else {
    (*sim_ctx)->rvs = rvs_create(&rv_sim_args);
    (*sim_ctx)->rng = rvs_create(&rng_args);
  }

  sim_results_set_rack(sim_results, move_list_get_rack(sim_args->move_list));
  sim_results_set_known_opp_rack(sim_results, sim_args->known_opp_rack);
  sim_results_set_cutoff(sim_results, sim_args->bai_options.cutoff);
  sim_results_set_num_infer_leaves(sim_results, num_infer_leaves);

  // Multi-fidelity: run BAI for each fidelity level sequentially.
  // Each level overrides sample_limit/sample_minimum and ply strategy.
  // The last level's result is the final answer.
  const int num_levels = sim_args->num_fidelity_levels;
  for (int level_idx = 0; level_idx < num_levels; level_idx++) {
    const FidelityLevel *level = &sim_args->fidelity_levels[level_idx];

    // Update ply strategy for this fidelity level
    rvs_set_fidelity_level((*sim_ctx)->rvs, level);

    // Set BAI options for this level
    BAIOptions level_bai_options = sim_args->bai_options;
    level_bai_options.sample_limit = level->sample_limit;
    level_bai_options.sample_minimum = level->sample_minimum;

    bai(&level_bai_options, (*sim_ctx)->rvs, (*sim_ctx)->rng,
        sim_args->thread_control, NULL,
        sim_results_get_bai_result(sim_results));

    // Check for early termination (user interrupt, etc.)
    if (thread_control_get_status(sim_args->thread_control) !=
        THREAD_CONTROL_STATUS_STARTED) {
      break;
    }
  }
}

void simulate_without_ctx(SimArgs *sim_args, SimResults *sim_results,
                          ErrorStack *error_stack) {
  SimCtx *ctx = NULL;
  simulate(sim_args, &ctx, sim_results, error_stack);
  sim_ctx_destroy(ctx);
}