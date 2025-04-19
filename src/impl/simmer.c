#include "simmer.h"

#include "../def/simmer_defs.h"

#include "../ent/sim_results.h"
#include "../ent/thread_control.h"

#include "../str/sim_string.h"

#include "bai.h"
#include "move_gen.h"
#include "random_variable.h"

sim_status_t simulate(const SimArgs *sim_args, SimResults *sim_results) {
  // The BAI call will reset the thread control.

  if (!sim_args->move_list) {
    return SIM_STATUS_NO_MOVES;
  }

  int move_list_count = move_list_get_count(sim_args->move_list);

  if (move_list_count == 0) {
    return SIM_STATUS_NO_MOVES;
  }

  RandomVariablesArgs rv_sim_args = {
      .type = RANDOM_VARIABLES_SIMMED_PLAYS,
      .sim_args = sim_args,
      .sim_results = sim_results,
  };

  RandomVariables *rvs = rvs_create(&rv_sim_args);

  RandomVariablesArgs rng_args = {
      .type = RANDOM_VARIABLES_UNIFORM,
      .seed = thread_control_get_seed(sim_args->thread_control),
  };

  RandomVariables *rng = rvs_create(&rng_args);

  BAIResult bai_result;

  bai(&sim_args->bai_options, rvs, rng, sim_args->thread_control, NULL,
      &bai_result);

  print_ucgi_sim_stats(
      sim_args->game, sim_results, sim_args->thread_control,
      (double)sim_results_get_node_count(sim_results) /
          thread_control_get_seconds_elapsed(sim_args->thread_control),
      true);

  rvs_destroy(rvs);
  rvs_destroy(rng);
  gen_destroy_cache();

  return SIM_STATUS_SUCCESS;
}