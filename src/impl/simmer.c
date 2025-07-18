#include "simmer.h"

#include "../ent/sim_results.h"
#include "../ent/thread_control.h"

#include "../util/io_util.h"

#include "../str/sim_string.h"

#include "bai.h"
#include "move_gen.h"
#include "random_variable.h"

void simulate(const SimArgs *sim_args, SimResults *sim_results,
              ErrorStack *error_stack) {
  // The BAI call will reset the thread control.

  if (!sim_args->move_list || move_list_get_count(sim_args->move_list) == 0) {
    error_stack_push(error_stack, ERROR_STATUS_SIM_NO_MOVES,
                     string_duplicate("cannot simulate without moves, use the "
                                      "'generate' command to generate moves"));
    return;
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

  assert(thread_control_set_mode_status_ready(sim_args->thread_control));

  bai(&sim_args->bai_options, rvs, rng, sim_args->thread_control, NULL,
      sim_results_get_bai_result(sim_results));

  sim_results_set_iteration_count(sim_results, rvs_get_total_samples(rvs));

  print_ucgi_sim_stats(
      sim_args->game, sim_results, sim_args->thread_control,
      (double)sim_results_get_node_count(sim_results) /
          thread_control_get_seconds_elapsed(sim_args->thread_control),
      true);

  rvs_destroy(rvs);
  rvs_destroy(rng);
  gen_destroy_cache();

  return;
}