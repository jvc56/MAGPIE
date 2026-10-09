
#include "autoplay.h"

#include "../compat/cpthread.h"
#include "../compat/ctime.h"
#include "../def/autoplay_defs.h"
#include "../def/cpthread_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/pat_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/autoplay_results.h"
#include "../ent/autoplay_solver_settings.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/checkpoint.h"
#include "../ent/data_filepaths.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_timer.h"
#include "../ent/inference_args.h"
#include "../ent/inference_results.h"
#include "../ent/klv.h"
#include "../ent/klv_csv.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/pat.h"
#include "../ent/pat_features.h"
#include "../ent/pat_file.h"
#include "../ent/player.h"
#include "../ent/players_data.h"
#include "../ent/rack.h"
#include "../ent/sim_results.h"
#include "../ent/thread_control.h"
#include "../ent/transposition_table.h"
#include "../ent/xoshiro.h"
#include "../str/game_string.h"
#include "../str/inference_string.h"
#include "../str/move_string.h"
#include "../str/sim_string.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "autoplay_solvers.h"
#include "gameplay.h"
#include "pat_gen.h"
#include "play_chooser.h"
#include "rack_list.h"
#include "simmer.h"
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Benchmark instrumentation: accumulates total sim iterations across all
// turns in all games. Read/reset via autoplay_get_total_sim_iterations().
static _Atomic uint64_t autoplay_total_sim_iterations;

uint64_t autoplay_get_total_sim_iterations(void) {
  return atomic_load_explicit(&autoplay_total_sim_iterations,
                              memory_order_relaxed);
}

void autoplay_reset_total_sim_iterations(void) {
  atomic_store_explicit(&autoplay_total_sim_iterations, 0,
                        memory_order_relaxed);
}

// Benchmark mode: when true, get_top_simming_move still runs the sim (so
// iteration counts and per-turn timing are measured) but the turn's played
// move is the top-equity move instead of the sim's chosen move. This makes
// the game trajectory independent of sim throughput, so different RIT/BAI
// variants run through the same sequence of positions.
static _Atomic bool autoplay_bench_static_move_enabled;

int autoplay_overtime_penalty_points(double overtime_seconds,
                                     int points_per_period,
                                     double period_seconds) {
  if (overtime_seconds <= 0.0 || points_per_period <= 0 ||
      period_seconds <= 0.0) {
    return 0;
  }
  const double periods = ceil(overtime_seconds / period_seconds);
  const int max_penalty_points = (int)EQUITY_MAX_DOUBLE;
  if (periods >= (double)max_penalty_points / (double)points_per_period) {
    return max_penalty_points;
  }
  return (int)periods * points_per_period;
}

void autoplay_set_bench_static_move(bool enabled) {
  atomic_store_explicit(&autoplay_bench_static_move_enabled, enabled,
                        memory_order_relaxed);
}

bool autoplay_get_bench_static_move(void) {
  return atomic_load_explicit(&autoplay_bench_static_move_enabled,
                              memory_order_relaxed);
}

typedef struct LeavegenSharedData {
  int num_gens;
  int gens_completed;
  uint64_t gen_start_games;
  int *min_rack_targets;
  AutoplayResults *gen_autoplay_results;
  const LetterDistribution *ld;
  const char *data_paths;
  KLV *klv;
  RackList *rack_list;
  Checkpoint *postgen_checkpoint;
  AutoplayResults *primary_autoplay_results;
  AutoplayResults **autoplay_results_list;
  // See AutoplayArgs.leavegen_write_files.
  bool write_files;
  // The first generation-file write that failed, if any. Recorded rather
  // than fatal -- postgen runs on a worker thread with no error stack to
  // push to -- and pushed onto the caller's stack when autoplay returns.
  char *postgen_error;
} LeavegenSharedData;

// Shared state for AUTOPLAY_TYPE_PAT_GEN: the live weights object
// (owned by players_data and shared by both players), one regression
// accumulator per worker thread (lock-free; consolidated single-threaded
// at the generation-boundary checkpoint), and the per-generation game
// budget. There are no forced draws and no early bag truncation: games run
// to completion and observations self-gate on the bag.
// One training observation waiting for its label: the features of the board
// left by the move that opened it, and the opponent's net gain so far over
// the plies played since. Scores of the opponent's moves count positively
// and the observing player's own moves negatively, so a board that hands
// the opponent a big play but pays it back next turn is not scored as a
// mistake.
typedef struct PATPendingObservation {
  bool valid;
  int plies_seen;
  double label;
  double features[PAT_NUM_FEATURES];
} PATPendingObservation;

typedef struct PATGenSharedData {
  // Plies of net result the label spans (1 is the opponent's reply score
  // alone).
  int label_plies;
  int num_gens;
  int gens_completed;
  uint64_t *games_per_gen;
  PATWeights *pat;
  const char *data_paths;
  const char *output_name;
  PATRegression *regressions;
  // Observations from every PAT_GEN_HELDOUT_EVERY-th game pair, kept out
  // of the fit and used to score installed candidates (see
  // pat_regression_installed_mse).
  PATRegression *heldout_regressions;
  int num_threads;
  Checkpoint *postgen_checkpoint;
} PATGenSharedData;

typedef struct AutoplaySharedData {
  int num_threads;
  int print_interval;
  Timer timer;
  uint64_t max_iter_count;
  uint64_t seed;
  XoshiroPRNG *prng;
  uint64_t iter_count;
  cpthread_mutex_t iter_mutex;
  uint64_t iter_count_completed;
  cpthread_mutex_t iter_completed_mutex;
  ThreadControl *thread_control;
  LeavegenSharedData *leavegen_shared_data;
  // The endgame transposition table every worker's endgame and PEG leaf solves
  // share, or NULL when no player solves.
  TranspositionTable *solver_tt;
  // The threads each endgame or PEG solve gets; see
  // autoplay_solver_num_threads.
  int solver_num_threads;
  PATGenSharedData *pat_gen_shared_data;
} AutoplaySharedData;

typedef struct AutoplayIterOutput {
  uint64_t seed;
  uint64_t iter_count;
} AutoplayIterOutput;

typedef struct AutoplayIterCompletedOutput {
  uint64_t iter_count_completed;
  double time_elapsed;
  bool print_info;
} AutoplayIterCompletedOutput;

// Returns true if the iter_count is already greater than or equal to
// stop_iter_count and does nothing else.
// Returns false if the iter_count is less than the stop_iter_count
// and increments the iter_count and sets the next seed.
bool autoplay_get_next_iter_output(AutoplaySharedData *shared_data,
                                   AutoplayIterOutput *iter_output) {
  bool at_stop_count = false;
  cpthread_mutex_lock(&shared_data->iter_mutex);
  if (shared_data->iter_count >= shared_data->max_iter_count) {
    at_stop_count = true;
  } else {
    iter_output->seed = prng_next(shared_data->prng);
    iter_output->iter_count = shared_data->iter_count++;
  }
  cpthread_mutex_unlock(&shared_data->iter_mutex);
  return at_stop_count;
}

// This function should be called when a thread has completed computation
// for an iteration given by autoplay_get_next_iter_output.
// It increments the count completed and records the elapsed time.
void autoplay_complete_iter(
    AutoplaySharedData *shared_data,
    AutoplayIterCompletedOutput *iter_completed_output) {
  cpthread_mutex_lock(&shared_data->iter_completed_mutex);
  // Update internal fields
  shared_data->iter_count_completed++;
  // Set output
  iter_completed_output->iter_count_completed =
      shared_data->iter_count_completed;
  iter_completed_output->time_elapsed =
      ctimer_elapsed_seconds(&shared_data->timer);
  iter_completed_output->print_info =
      shared_data->print_interval > 0 &&
      shared_data->iter_count_completed % shared_data->print_interval == 0;
  cpthread_mutex_unlock(&shared_data->iter_completed_mutex);
}

// Copies the thread control PRNG to the other PRNG and performs a PRNG
// jump on the thread control PRNG.
void autoplay_shared_data_copy_to_dst_and_jump(AutoplaySharedData *shared_data,
                                               XoshiroPRNG *dst) {
  prng_copy(dst, shared_data->prng);
  prng_jump(shared_data->prng);
}

// Writes the generation's KLV, leaves CSV and report into the data directory.
// A failed write is recorded in postgen_error (the first one only) and ends
// this generation's writes; it does not end the process.
static void leavegen_write_generation_files(AutoplaySharedData *shared_data) {
  LeavegenSharedData *lg_shared_data = shared_data->leavegen_shared_data;
  char *label = get_formatted_string("_gen_%d", lg_shared_data->gens_completed);
  char *gen_labeled_klv_name =
      insert_before_dot(lg_shared_data->klv->name, label);
  ErrorStack *error_stack = error_stack_create();
  const char *failure = NULL;

  char *gen_labeled_klv_filename = data_filepaths_get_writable_filename(
      lg_shared_data->data_paths, gen_labeled_klv_name, DATA_FILEPATH_TYPE_KLV,
      error_stack);
  char *leaves_filename = data_filepaths_get_writable_filename(
      lg_shared_data->data_paths, gen_labeled_klv_name,
      DATA_FILEPATH_TYPE_LEAVES, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    failure = "leavegen could not find a writable location for its results";
  }

  if (!failure) {
    klv_write(lg_shared_data->klv, lg_shared_data->data_paths,
              gen_labeled_klv_name, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      failure = "leavegen failed to write the generation's klv";
    }
  }

  if (!failure) {
    klv_write_to_csv(lg_shared_data->klv, lg_shared_data->ld,
                     lg_shared_data->data_paths, gen_labeled_klv_name, NULL,
                     error_stack);
    if (!error_stack_is_empty(error_stack)) {
      failure = "leavegen failed to write the generation's klv to CSV";
    }
  }

  if (!failure) {
    // Print info about the current state.
    StringBuilder *leave_gen_sb = string_builder_create();

    string_builder_add_string(
        leave_gen_sb, "************************\n"
                      "Cumulative Autoplay Data\n************************\n\n");

    string_builder_add_formatted_string(
        leave_gen_sb, "Seconds: %f\n",
        ctimer_elapsed_seconds(&shared_data->timer));
    char *cumul_game_data_str = autoplay_results_to_string(
        lg_shared_data->primary_autoplay_results, true, false);
    string_builder_add_string(leave_gen_sb, cumul_game_data_str);
    free(cumul_game_data_str);

    string_builder_add_string(
        leave_gen_sb,
        "\n**************************\n"
        "Generational Autoplay Data\n**************************\n\n");

    char *gen_game_data_str = autoplay_results_to_string(
        lg_shared_data->gen_autoplay_results, true, false);
    string_builder_add_string(leave_gen_sb, gen_game_data_str);
    free(gen_game_data_str);

    string_builder_add_formatted_string(
        leave_gen_sb,
        "\nTarget Minimum "
        "Leave "
        "Count: %d\nLeaves Under "
        "Target Minimum Leave Count: %d\n\n",
        rack_list_get_target_rack_count(lg_shared_data->rack_list),
        rack_list_get_racks_below_target_count(lg_shared_data->rack_list));

    char *report_name_prefix =
        cut_off_after_last_char(gen_labeled_klv_filename, '.');
    char *report_name =
        get_formatted_string("%s_report.txt", report_name_prefix);

    write_string_to_file(report_name, "w", string_builder_peek(leave_gen_sb),
                         error_stack);
    if (!error_stack_is_empty(error_stack)) {
      failure = "leavegen failed to write the generation's report";
    }
    string_builder_destroy(leave_gen_sb);
    free(report_name);
    free(report_name_prefix);
  }

  if (failure) {
    error_stack_print_and_reset(error_stack);
    if (!lg_shared_data->postgen_error) {
      lg_shared_data->postgen_error = get_formatted_string(
          "%s (generation %d)", failure, lg_shared_data->gens_completed);
    }
  }

  error_stack_destroy(error_stack);
  free(gen_labeled_klv_filename);
  free(gen_labeled_klv_name);
  free(label);
  free(leaves_filename);
}

void postgen_prebroadcast_func(void *data) {
  AutoplaySharedData *shared_data = (AutoplaySharedData *)data;
  LeavegenSharedData *lg_shared_data = shared_data->leavegen_shared_data;
  rack_list_write_to_klv(lg_shared_data->rack_list, lg_shared_data->ld,
                         lg_shared_data->klv);
  // The direct RackList read that contribute's leave_generation executor
  // needs (birdtest's PLAN.md, "Leave generation on the client"): the
  // results go back in the task's JSON response rather than through a file.
  autoplay_results_set_leave_results_json(
      lg_shared_data->primary_autoplay_results,
      rack_list_get_rack_equity_json(lg_shared_data->rack_list,
                                     lg_shared_data->ld));
  lg_shared_data->gens_completed++;

  // Get total game data.
  autoplay_results_consolidate(lg_shared_data->autoplay_results_list,
                               shared_data->num_threads,
                               lg_shared_data->primary_autoplay_results);

  // Get generational game data
  autoplay_results_reset(lg_shared_data->gen_autoplay_results);
  autoplay_results_consolidate(lg_shared_data->autoplay_results_list,
                               shared_data->num_threads,
                               lg_shared_data->gen_autoplay_results);

  for (int i = 0; i < shared_data->num_threads; i++) {
    autoplay_results_reset(lg_shared_data->autoplay_results_list[i]);
  }

  if (lg_shared_data->write_files) {
    leavegen_write_generation_files(shared_data);
  }

  // Reset data for the next generation.
  if (lg_shared_data->gens_completed < lg_shared_data->num_gens) {
    rack_list_reset(
        lg_shared_data->rack_list,
        lg_shared_data->min_rack_targets[lg_shared_data->gens_completed]);
    lg_shared_data->gen_start_games = shared_data->iter_count;
  }
}

// Whether a game pair's observations go to the validation accumulator:
// a deterministic hash of the pair's number rather than its position in
// the sequence, so no periodic structure in the schedule lines up with
// the split.
static bool pat_gen_pair_is_validation(uint64_t game_number) {
  uint64_t z = game_number + 0x9E3779B97F4A7C15ULL;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  z ^= z >> 31;
  return (z % PAT_GEN_HELDOUT_EVERY) == 0;
}

// Per-observation ridge strength for the PAT regression, in
// (points per feature unit)^2.
static const double PAT_GEN_RIDGE_LAMBDA = 1.0;

// Runs single-threaded at the PAT generation boundary while every
// worker is parked in checkpoint_wait: consolidates the per-thread
// regressions, refits the weights (rewriting the live PATWeights that
// every subsequent gen_load_position re-reads), snapshots the generation's
// weights and a fit report, and extends the game budget for the next
// generation.
void pat_postgen_prebroadcast_func(void *data) {
  AutoplaySharedData *shared_data = (AutoplaySharedData *)data;
  PATGenSharedData *pat_gen_shared_data = shared_data->pat_gen_shared_data;

  // Heap-allocated: each accumulator is a few hundred KB and this runs on
  // a worker thread's stack, next to the solver's own matrix.
  PATRegression *total_regression_ptr = malloc_or_die(sizeof(PATRegression));
  PATRegression *heldout_regression_ptr = malloc_or_die(sizeof(PATRegression));
  pat_regression_reset(total_regression_ptr);
  pat_regression_reset(heldout_regression_ptr);
  for (int thread_index = 0; thread_index < pat_gen_shared_data->num_threads;
       thread_index++) {
    pat_regression_merge(total_regression_ptr,
                         &pat_gen_shared_data->regressions[thread_index]);
    pat_regression_reset(&pat_gen_shared_data->regressions[thread_index]);
    pat_regression_merge(
        heldout_regression_ptr,
        &pat_gen_shared_data->heldout_regressions[thread_index]);
    pat_regression_reset(
        &pat_gen_shared_data->heldout_regressions[thread_index]);
  }
#define total_regression (*total_regression_ptr)
#define heldout_regression (*heldout_regression_ptr)
  PATWeights *pat = pat_gen_shared_data->pat;
  const double validation_loaded_mse =
      pat_regression_installed_mse(&heldout_regression, pat);
  const double validation_baseline_mse =
      pat_regression_baseline_mse(&heldout_regression);
  const PATSolveResult solve_result = pat_regression_solve_into_weights(
      &total_regression, PAT_GEN_RIDGE_LAMBDA, pat);
  const double validation_fit_mse =
      pat_regression_installed_mse(&heldout_regression, pat);

  pat_gen_shared_data->gens_completed++;

  char *gen_labeled_pat_name =
      get_formatted_string("%s_gen_%d", pat_gen_shared_data->output_name,
                           pat_gen_shared_data->gens_completed);

  ErrorStack *error_stack = error_stack_create();
  pat_write(pat_gen_shared_data->pat, pat_gen_shared_data->data_paths,
            gen_labeled_pat_name, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("patgen failed to write weights to file");
  }

  StringBuilder *report_sb = string_builder_create();
  string_builder_add_formatted_string(
      report_sb, "PAT generation %d\nSeconds: %f\nObservations: %llu\n",
      pat_gen_shared_data->gens_completed,
      ctimer_elapsed_seconds(&shared_data->timer),
      (unsigned long long)solve_result.num_observations);
  if (solve_result.solved) {
    string_builder_add_formatted_string(
        report_sb,
        "Intercept (mean reply baseline): %f\nFit MSE: %f\nBaseline MSE: "
        "%f\nValidation observations: %llu\nValidation baseline MSE: "
        "%f\nValidation MSE, loaded weights (intercept refit): %f\n"
        "Validation MSE, installed weights (intercept refit): %f\n",
        solve_result.intercept, solve_result.mean_squared_error,
        solve_result.baseline_mean_squared_error,
        (unsigned long long)heldout_regression.num_observations,
        validation_baseline_mse, validation_loaded_mse, validation_fit_mse);
    string_builder_add_string(report_sb,
                              "\nfeature,raw_coefficient,applied_weight\n");
    char feature_name[64];
    for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
         feature_index++) {
      pat_feature_name(feature_index, feature_name, sizeof(feature_name));
      string_builder_add_formatted_string(
          report_sb, "%s,%f,%d\n", feature_name,
          solve_result.coefficients[feature_index],
          pat_get_weight(pat_gen_shared_data->pat, feature_index));
    }
  } else {
    string_builder_add_string(
        report_sb, "The regression could not be solved; the weights were "
                   "left unchanged.\n");
  }

  char *gen_labeled_pat_filename = data_filepaths_get_writable_filename(
      pat_gen_shared_data->data_paths, gen_labeled_pat_name,
      DATA_FILEPATH_TYPE_PAT, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("patgen failed to build the report filename");
  }
  char *report_name_prefix =
      cut_off_after_last_char(gen_labeled_pat_filename, '.');
  char *report_name = get_formatted_string("%s_report.txt", report_name_prefix);
  write_string_to_file(report_name, "w", string_builder_peek(report_sb),
                       error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("patgen failed to write the fit report to file");
  }

  string_builder_destroy(report_sb);
  error_stack_destroy(error_stack);
#undef total_regression
#undef heldout_regression
  free(total_regression_ptr);
  free(heldout_regression_ptr);
  free(report_name);
  free(report_name_prefix);
  free(gen_labeled_pat_filename);
  free(gen_labeled_pat_name);

  // Extend the game budget for the next generation. Every worker is parked
  // in checkpoint_wait, so this is safe without the iter mutex.
  if (pat_gen_shared_data->gens_completed < pat_gen_shared_data->num_gens) {
    shared_data->max_iter_count +=
        pat_gen_shared_data->games_per_gen[pat_gen_shared_data->gens_completed];
  }
}

typedef struct AutoplayWorker {
  int worker_index;
  AutoplayArgs args;
  AutoplayResults *autoplay_results;
  AutoplaySharedData *shared_data;
  XoshiroPRNG *prng;
  int *min_rack_targets;
  SimCtx *sim_ctx;
  SimResults *sim_results;
  InferenceResults *inference_results;
  ErrorStack *error_stack;
  Rack target_played_tiles;
  Rack nontarget_known_rack;
  Rack target_known_rack;
  MoveList *move_lists[2];
  // Endgame and pre-endgame solving; NULL when no player solves.
  AutoplaySolverCtx *solver_ctx;
  // Whether a solver chose the move this turn, so the positions recorder takes
  // the solver's analysis instead of the move list and sim results, which then
  // describe an earlier turn.
  bool turn_was_solved;
  // Whether a simulation ran this turn, so the positions recorder takes the
  // sim results only then. A simming player's turn with one legal play (a
  // forced pass) runs none, and sim_results still holds the last simulation
  // this worker ran -- another turn's, or another game's.
  bool turn_was_simmed;
  // Whether the positions recorder is active for this run. A static player
  // otherwise only ever ranks the one move it plays; this asks it to keep the
  // whole ranked list instead, the same way a simming player already does.
  bool captures_positions;
} AutoplayWorker;

AutoplayWorker *autoplay_worker_create(const AutoplayArgs *args,
                                       const AutoplayResults *target,
                                       int worker_index,
                                       AutoplaySharedData *shared_data) {
  AutoplayWorker *autoplay_worker = malloc_or_die(sizeof(AutoplayWorker));
  autoplay_worker->args = *args;
  autoplay_worker->captures_positions =
      (autoplay_results_get_options(target) &
       autoplay_results_build_option(AUTOPLAY_RECORDER_TYPE_POSITION)) != 0;
  // A static player normally only needs a move list capacity of 1 (0 plies
  // indicates static equity), but position capture wants the whole ranked
  // list up to the job's reporting cap, the same as a simming player's
  // num_plays already provides.
  const int position_play_cap =
      autoplay_worker->captures_positions ? args->position_play_cap : 0;
  if (autoplay_worker->args.p1_sim_args.num_plays == 0) {
    autoplay_worker->args.p1_sim_args.num_plays = 1;
  }
  if (autoplay_worker->args.p2_sim_args.num_plays == 0) {
    autoplay_worker->args.p2_sim_args.num_plays = 1;
  }
  if (autoplay_worker->args.p1_sim_args.num_plays < position_play_cap) {
    autoplay_worker->args.p1_sim_args.num_plays = position_play_cap;
  }
  if (autoplay_worker->args.p2_sim_args.num_plays < position_play_cap) {
    autoplay_worker->args.p2_sim_args.num_plays = position_play_cap;
  }
  // A captured position keeps the opponent's most drawn inferred leaves,
  // which inference lists only when asked to keep a list of them; nothing
  // else in autoplay asks.
  if (autoplay_worker->captures_positions) {
    SimArgs *seats[] = {&autoplay_worker->args.p1_sim_args,
                        &autoplay_worker->args.p2_sim_args};
    for (int i = 0; i < 2; i++) {
      if (seats[i]->inference_args.leave_list_capacity <
          AUTOPLAY_CAPTURED_INFERENCE_LEAVES) {
        seats[i]->inference_args.leave_list_capacity =
            AUTOPLAY_CAPTURED_INFERENCE_LEAVES;
      }
    }
  }
  autoplay_worker->worker_index = worker_index;
  autoplay_worker->autoplay_results =
      autoplay_results_create_empty_copy(target);
  autoplay_worker->prng = NULL;
  if (shared_data->leavegen_shared_data) {
    autoplay_worker->prng = prng_create(0);
    autoplay_shared_data_copy_to_dst_and_jump(shared_data,
                                              autoplay_worker->prng);
  }
  autoplay_worker->shared_data = shared_data;
  autoplay_worker->sim_ctx = NULL;
  const AutoplayArgs *ap_args = &autoplay_worker->args;

  autoplay_worker->move_lists[0] =
      move_list_create(ap_args->p1_sim_args.num_plays);
  autoplay_worker->move_lists[1] =
      move_list_create(ap_args->p2_sim_args.num_plays);

  autoplay_worker->sim_results = NULL;
  autoplay_worker->inference_results = NULL;
  autoplay_worker->error_stack = NULL;
  autoplay_worker->solver_ctx = NULL;
  autoplay_worker->turn_was_solved = false;
  autoplay_worker->turn_was_simmed = false;

  const bool any_player_sims =
      ap_args->p1_sim_args.num_plies > 0 || ap_args->p2_sim_args.num_plies > 0;
  const bool any_player_uses_play_chooser =
      ap_args->use_play_chooser[0] || ap_args->use_play_chooser[1];
  // PlayChooser needs an error stack. The remaining objects are specific to
  // the legacy autoplay simmer.
  if (any_player_uses_play_chooser) {
    autoplay_worker->error_stack = error_stack_create();
  }
  if (shared_data->solver_tt) {
    autoplay_worker->solver_ctx = autoplay_solver_ctx_create();
    if (autoplay_worker->error_stack == NULL) {
      autoplay_worker->error_stack = error_stack_create();
    }
  }
  if (any_player_sims) {
    autoplay_worker->sim_results = sim_results_create(ap_args->cutoff);
    autoplay_worker->inference_results = inference_results_create(NULL);
    if (autoplay_worker->error_stack == NULL) {
      autoplay_worker->error_stack = error_stack_create();
    }
    rack_set_dist_size_and_reset(&autoplay_worker->target_played_tiles,
                                 ld_get_size(ap_args->game_args->ld));
    rack_set_dist_size_and_reset(&autoplay_worker->nontarget_known_rack,
                                 ld_get_size(ap_args->game_args->ld));
    rack_set_dist_size_and_reset(&autoplay_worker->target_known_rack,
                                 ld_get_size(ap_args->game_args->ld));
  }

  return autoplay_worker;
}

void autoplay_worker_destroy(AutoplayWorker *autoplay_worker) {
  if (!autoplay_worker) {
    return;
  }
  autoplay_results_destroy(autoplay_worker->autoplay_results);
  prng_destroy(autoplay_worker->prng);
  sim_ctx_destroy(autoplay_worker->sim_ctx);
  sim_results_destroy(autoplay_worker->sim_results);
  inference_results_destroy(autoplay_worker->inference_results);
  autoplay_solver_ctx_destroy(autoplay_worker->solver_ctx);
  error_stack_destroy(autoplay_worker->error_stack);
  move_list_destroy(autoplay_worker->move_lists[0]);
  move_list_destroy(autoplay_worker->move_lists[1]);
  free(autoplay_worker);
}

// forced_racks is optional (num_forced_racks 0 for an unrestricted run) and
// is passed straight through to rack_list_create; see its documentation for
// what it does. Pushes to error_stack and returns NULL if a forced rack is
// malformed or duplicated.
LeavegenSharedData *leavegen_shared_data_create(
    AutoplayResults *primary_autoplay_results,
    AutoplayResults **autoplay_results_list, const LetterDistribution *ld,
    const char *data_paths, KLV *klv, int number_of_threads, int num_gens,
    int *min_rack_targets, const char *const *forced_racks,
    int num_forced_racks, ErrorStack *error_stack) {
  LeavegenSharedData *shared_data = malloc_or_die(sizeof(LeavegenSharedData));

  shared_data->num_gens = num_gens;
  shared_data->gens_completed = 0;
  shared_data->write_files = true;
  shared_data->postgen_error = NULL;
  shared_data->gen_start_games = 0;
  shared_data->klv = klv;
  shared_data->gen_autoplay_results =
      autoplay_results_create_empty_copy(primary_autoplay_results);
  shared_data->primary_autoplay_results = primary_autoplay_results;
  shared_data->autoplay_results_list = autoplay_results_list;
  shared_data->ld = ld;
  shared_data->data_paths = data_paths;
  shared_data->min_rack_targets = min_rack_targets;
  shared_data->rack_list = rack_list_create(
      ld, min_rack_targets[0], forced_racks, num_forced_racks, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    autoplay_results_destroy(shared_data->gen_autoplay_results);
    free(shared_data);
    return NULL;
  }
  shared_data->postgen_checkpoint =
      checkpoint_create(number_of_threads, postgen_prebroadcast_func);
  return shared_data;
}

// Use NULL for the KLV when not running in leave gen mode. forced_racks and
// num_forced_racks are only meaningful when klv is non-NULL (see
// leavegen_shared_data_create); pushes to error_stack and returns NULL on
// the same conditions that function does.
AutoplaySharedData *
autoplay_shared_data_create(const AutoplayArgs *args, int num_autoplay_threads,
                            const uint64_t first_gen_num_games,
                            AutoplayResults *primary_autoplay_results,
                            AutoplayResults **autoplay_results_list, KLV *klv,
                            int num_gens, int *min_rack_targets,
                            const char *const *forced_racks,
                            int num_forced_racks, ErrorStack *error_stack) {
  AutoplaySharedData *shared_data = malloc_or_die(sizeof(AutoplaySharedData));
  shared_data->num_threads = num_autoplay_threads;
  shared_data->print_interval = args->print_interval;
  ctimer_start(&shared_data->timer);
  shared_data->max_iter_count = first_gen_num_games;
  shared_data->seed = args->seed;
  shared_data->prng = prng_create(args->seed);
  shared_data->iter_count = 0;
  cpthread_mutex_init(&shared_data->iter_mutex);
  shared_data->iter_count_completed = 0;
  cpthread_mutex_init(&shared_data->iter_completed_mutex);
  shared_data->thread_control = args->thread_control;
  shared_data->leavegen_shared_data = NULL;
  shared_data->solver_tt = NULL;
  shared_data->solver_num_threads = 1;
  if (args->type == AUTOPLAY_TYPE_DEFAULT &&
      (autoplay_solver_settings_solves(&args->solver_settings[0]) ||
       autoplay_solver_settings_solves(&args->solver_settings[1]))) {
    shared_data->solver_tt =
        transposition_table_create(args->solver_tt_fraction_of_mem);
  }
  shared_data->pat_gen_shared_data = NULL;
  if (klv) {
    shared_data->leavegen_shared_data = leavegen_shared_data_create(
        primary_autoplay_results, autoplay_results_list, args->game_args->ld,
        args->data_paths, klv, num_autoplay_threads, num_gens, min_rack_targets,
        forced_racks, num_forced_racks, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      prng_destroy(shared_data->prng);
      transposition_table_destroy(shared_data->solver_tt);
      free(shared_data);
      return NULL;
    }
    shared_data->leavegen_shared_data->write_files = args->leavegen_write_files;
  }
  return shared_data;
}

void leavegen_shared_data_destroy(LeavegenSharedData *lg_shared_data) {
  if (!lg_shared_data) {
    return;
  }
  rack_list_destroy(lg_shared_data->rack_list);
  checkpoint_destroy(lg_shared_data->postgen_checkpoint);
  autoplay_results_destroy(lg_shared_data->gen_autoplay_results);
  free(lg_shared_data->postgen_error);
  free(lg_shared_data);
}

PATGenSharedData *pat_gen_shared_data_create(
    PATWeights *pat, const char *data_paths, const char *output_name,
    int num_threads, int num_gens, uint64_t *games_per_gen, int label_plies) {
  PATGenSharedData *pat_gen_shared_data =
      malloc_or_die(sizeof(PATGenSharedData));
  pat_gen_shared_data->label_plies = label_plies;
  pat_gen_shared_data->num_gens = num_gens;
  pat_gen_shared_data->gens_completed = 0;
  pat_gen_shared_data->games_per_gen = games_per_gen;
  pat_gen_shared_data->pat = pat;
  pat_gen_shared_data->data_paths = data_paths;
  pat_gen_shared_data->output_name = output_name;
  pat_gen_shared_data->num_threads = num_threads;
  pat_gen_shared_data->regressions =
      malloc_or_die(sizeof(PATRegression) * (size_t)num_threads);
  pat_gen_shared_data->heldout_regressions =
      malloc_or_die(sizeof(PATRegression) * (size_t)num_threads);
  for (int thread_index = 0; thread_index < num_threads; thread_index++) {
    pat_regression_reset(&pat_gen_shared_data->regressions[thread_index]);
    pat_regression_reset(
        &pat_gen_shared_data->heldout_regressions[thread_index]);
  }
  pat_gen_shared_data->postgen_checkpoint =
      checkpoint_create(num_threads, pat_postgen_prebroadcast_func);
  return pat_gen_shared_data;
}

void pat_gen_shared_data_destroy(PATGenSharedData *pat_gen_shared_data) {
  if (!pat_gen_shared_data) {
    return;
  }
  checkpoint_destroy(pat_gen_shared_data->postgen_checkpoint);
  free(pat_gen_shared_data->regressions);
  free(pat_gen_shared_data->heldout_regressions);
  free(pat_gen_shared_data);
}

void autoplay_shared_data_destroy(AutoplaySharedData *shared_data) {
  if (!shared_data) {
    return;
  }
  prng_destroy(shared_data->prng);
  leavegen_shared_data_destroy(shared_data->leavegen_shared_data);
  transposition_table_destroy(shared_data->solver_tt);
  pat_gen_shared_data_destroy(shared_data->pat_gen_shared_data);
  free(shared_data);
}

typedef struct GameRunner {
  bool force_draw;
  int turn_number;
  int pair_game_number; // 0 for non-paired games, 1 or 2 for game pairs
  uint64_t game_number;
  uint64_t seed;
  Game *game;
  // Used for inference args in autoplay with
  // inference
  Game *game_one_move_behind;
  Move previous_move;
  Move play_chooser_move;
  // PAT training (AUTOPLAY_TYPE_PAT_GEN): observations
  // opened by recent moves of this game, each still accumulating its label.
  // One opens per move and one closes every label_plies plies later, so at
  // most that many are ever in flight. Observations still unlabeled when
  // the game ends are dropped: their plies do not exist.
  PATPendingObservation pat_obs[PAT_MAX_LABEL_PLIES];
  PlayChooser *play_choosers[2];
  GameTimer game_timer;
  AutoplayGameTiming timing;
  AutoplaySharedData *shared_data;
  // The opening move as chosen, for the results' opening-length
  // statistic: tiles played (-1 until a tile placement opens the game)
  // and its static equity in points.
  int opening_tiles;
  double opening_equity;
} GameRunner;

static void game_runner_destroy_play_choosers(GameRunner *game_runner) {
  for (int player_index = 0; player_index < 2; player_index++) {
    play_chooser_destroy(game_runner->play_choosers[player_index]);
    game_runner->play_choosers[player_index] = NULL;
  }
}

GameRunner *game_runner_create(AutoplayWorker *autoplay_worker) {
  const AutoplayArgs *args = &autoplay_worker->args;
  GameRunner *game_runner = malloc_or_die(sizeof(GameRunner));
  game_runner->shared_data = autoplay_worker->shared_data;
  game_runner->game = game_create(args->game_args);
  game_runner->game_one_move_behind = NULL;
  if (args->p1_sim_args.use_inference || args->p2_sim_args.use_inference) {
    game_runner->game_one_move_behind = game_create(args->game_args);
  }
  game_runner->pair_game_number =
      0; // Will be set in game_runner_start if using pairs
  game_runner->play_choosers[0] = NULL;
  game_runner->play_choosers[1] = NULL;
  game_timer_reset(&game_runner->game_timer, 0.0);
  game_runner->timing = (AutoplayGameTiming){0};
  return game_runner;
}

void game_runner_destroy(GameRunner *game_runner) {
  if (!game_runner) {
    return;
  }
  game_runner_destroy_play_choosers(game_runner);
  game_destroy(game_runner->game);
  game_destroy(game_runner->game_one_move_behind);
  free(game_runner);
}

static void game_runner_create_play_choosers(AutoplayWorker *autoplay_worker,
                                             GameRunner *game_runner,
                                             uint64_t seed) {
  game_runner_destroy_play_choosers(game_runner);
  const AutoplayArgs *args = &autoplay_worker->args;
  for (int player_index = 0; player_index < 2; player_index++) {
    if (!args->use_play_chooser[player_index]) {
      continue;
    }
    PlayChooserStrategy strategy = args->play_chooser_strategies[player_index];
    strategy.game_timer = &game_runner->game_timer;
    strategy.overtime_period_seconds = args->overtime_period_seconds;
    strategy.seed = seed + (uint64_t)player_index;
    game_runner->play_choosers[player_index] = play_chooser_create(&strategy);
  }
}

void game_runner_start(AutoplayWorker *autoplay_worker, GameRunner *game_runner,
                       const AutoplayIterOutput *iter_output,
                       int starting_player_index, int pair_game_number) {
  Game *game = game_runner->game;
  game_reset(game);
  game_runner->seed = iter_output->seed;
  game_runner->game_number = iter_output->iter_count;
  game_runner->pair_game_number = pair_game_number;
  game_seed(game, iter_output->seed);
  autoplay_worker->args.p1_sim_args.seed = iter_output->seed;
  autoplay_worker->args.p2_sim_args.seed = iter_output->seed;
  game_set_starting_player_index(game, starting_player_index);
  draw_starting_racks(game);
  game_timer_reset_for_players(&game_runner->game_timer,
                               autoplay_worker->args.time_control_seconds[0],
                               autoplay_worker->args.time_control_seconds[1]);
  game_runner->timing = (AutoplayGameTiming){0};
  game_runner_create_play_choosers(autoplay_worker, game_runner,
                                   iter_output->seed);
  if (game_runner->game_one_move_behind) {
    Game *game_one_move_behind = game_runner->game_one_move_behind;
    game_reset(game_one_move_behind);
    game_seed(game_one_move_behind, iter_output->seed);
    game_set_starting_player_index(game_one_move_behind, starting_player_index);
    draw_starting_racks(game_one_move_behind);
  }

  game_runner->turn_number = 0;
  game_runner->opening_tiles = -1;
  game_runner->opening_equity = 0.0;
  game_runner->force_draw = false;
  for (int obs_index = 0; obs_index < PAT_MAX_LABEL_PLIES; obs_index++) {
    game_runner->pat_obs[obs_index].valid = false;
  }
  if (game_runner->shared_data->leavegen_shared_data &&
      // We only force draws if we've played enough games for this
      // generation. This also applies when leavegen's rack list is
      // restricted to a set of forced racks (see rack_list_create): clients
      // fulfilling requests can just pass 0 if they want forcing
      // from the start.
      (iter_output->iter_count -
       game_runner->shared_data->leavegen_shared_data->gen_start_games) >=
          (uint64_t)autoplay_worker->args.games_before_force_draw_start) {
    game_runner->force_draw = true;
  }
}

bool game_runner_is_game_over(GameRunner *game_runner) {
  return game_over(game_runner->game) ||
         (game_runner->shared_data->leavegen_shared_data &&
          bag_get_letters(game_get_bag(game_runner->game)) < (RACK_SIZE));
}

const Move *game_runner_get_top_simming_move(AutoplayWorker *autoplay_worker,
                                             GameRunner *game_runner) {
  Game *game = game_runner->game;
  const int player_on_turn_index = game_get_player_on_turn_index(game);
  MoveList *move_list = autoplay_worker->move_lists[player_on_turn_index];
  SimArgs *sim_args = (player_on_turn_index == 0)
                          ? &autoplay_worker->args.p1_sim_args
                          : &autoplay_worker->args.p2_sim_args;
  sim_args->move_list = move_list;
  sim_args->game = game_runner->game;

  const bool player_uses_inference = sim_args->use_inference;
  sim_args->use_inference =
      player_uses_inference && game_runner->turn_number > 0 &&
      move_get_type(&game_runner->previous_move) != GAME_EVENT_PASS;
  if (sim_args->use_inference) {
    InferenceArgs *infer_args = &sim_args->inference_args;
    // Set target played tiles
    rack_reset(&autoplay_worker->target_played_tiles);
    const int move_tiles_length =
        move_get_tiles_length(&game_runner->previous_move);
    if (move_get_type(&game_runner->previous_move) ==
        GAME_EVENT_TILE_PLACEMENT_MOVE) {
      for (int i = 0; i < move_tiles_length; i++) {
        if (move_get_tile(&game_runner->previous_move, i) !=
            PLAYED_THROUGH_MARKER) {
          if (get_is_blanked(move_get_tile(&game_runner->previous_move, i))) {
            rack_add_letter(&autoplay_worker->target_played_tiles,
                            BLANK_MACHINE_LETTER);
          } else {
            rack_add_letter(&autoplay_worker->target_played_tiles,
                            move_get_tile(&game_runner->previous_move, i));
          }
        }
      }
    }
    // Set nontarget known rack
    rack_copy(&autoplay_worker->nontarget_known_rack,
              player_get_rack(game_get_player(game, player_on_turn_index)));
    // The target known rack was set to empty when the autoplay worker was
    // created. It does not need to be modified after initial creation as it
    // will always be empty because autoplay does not support challenged phonies
    // (yet).
    infer_args_fill(
        infer_args, infer_args->leave_list_capacity, infer_args->equity_margin,
        infer_args->game_history, game_runner->game_one_move_behind,
        infer_args->num_threads, infer_args->parent_worker_thread_index,
        infer_args->print_interval, infer_args->thread_control,
        infer_args->use_game_history,
        infer_args->use_inference_cutoff_optimization,
        // We can use 1 - player_on_turn_index for the target index because
        // autoplay does not support challenged phonies (yet).
        1 - player_on_turn_index, move_get_score(&game_runner->previous_move),
        move_get_type(&game_runner->previous_move) == GAME_EVENT_EXCHANGE
            ? move_get_tiles_played(&game_runner->previous_move)
            : 0,
        &autoplay_worker->target_played_tiles,
        &autoplay_worker->target_known_rack,
        &autoplay_worker->nontarget_known_rack);
  }

  ErrorStack *error_stack = autoplay_worker->error_stack;
  const Move *move =
      get_top_simming_move(game, move_list, sim_args, &autoplay_worker->sim_ctx,
                           autoplay_worker->sim_results,
                           &autoplay_worker->turn_was_simmed, error_stack);
  if (autoplay_worker->turn_was_simmed) {
    atomic_fetch_add_explicit(
        &autoplay_total_sim_iterations,
        sim_results_get_iteration_count(autoplay_worker->sim_results),
        memory_order_relaxed);
  }
  // In benchmark mode the sim still runs (above) but we play the top-equity
  // static move to pin the game trajectory so different variants compare
  // like-for-like positions.
  if (autoplay_get_bench_static_move() && move_list_get_count(move_list) > 0) {
    move = move_list_get_move(move_list, 0);
  }
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("autoplay worker %d failed to get top simming move for player %d "
              "on turn %d of game number %llu with seed %llu",
              autoplay_worker->worker_index, player_on_turn_index,
              game_runner->turn_number + 1,
              (unsigned long long)game_runner->game_number + 1,
              (unsigned long long)game_runner->seed);
  }
  sim_args->use_inference = player_uses_inference;
  return move;
}

const Move *game_runner_get_best_move(AutoplayWorker *autoplay_worker,
                                      GameRunner *game_runner) {
  autoplay_worker->turn_was_solved = false;
  autoplay_worker->turn_was_simmed = false;
  const int player_on_turn_index =
      game_get_player_on_turn_index(game_runner->game);
  PlayChooser *play_chooser = game_runner->play_choosers[player_on_turn_index];
  if (play_chooser != NULL) {
    game_timer_start_turn(&game_runner->game_timer, player_on_turn_index);
    play_chooser_choose_move(play_chooser, game_runner->game,
                             &game_runner->play_chooser_move,
                             autoplay_worker->error_stack);
    game_timer_end_turn(&game_runner->game_timer);
    if (!error_stack_is_empty(autoplay_worker->error_stack)) {
      error_stack_print_and_reset(autoplay_worker->error_stack);
      log_fatal("autoplay PlayChooser failed for player %d on turn %d of "
                "game number %llu with seed %llu",
                player_on_turn_index + 1, game_runner->turn_number + 1,
                (unsigned long long)game_runner->game_number + 1,
                (unsigned long long)game_runner->seed);
    }
    if (autoplay_get_bench_static_move()) {
      return get_top_equity_move(
          game_runner->game, autoplay_worker->move_lists[player_on_turn_index]);
    }
    return &game_runner->play_chooser_move;
  }
  // The end of the game, for a player that solves it: the endgame once the bag
  // is empty, the pre-endgame while it is small.
  const AutoplaySolverSettings *solver_settings =
      &autoplay_worker->args.solver_settings[player_on_turn_index];
  if (autoplay_worker->solver_ctx &&
      autoplay_solver_applies(solver_settings, game_runner->game)) {
    ErrorStack *error_stack = autoplay_worker->error_stack;
    const Move *solved = autoplay_solver_solve(
        autoplay_worker->solver_ctx, solver_settings, game_runner->game,
        autoplay_worker->shared_data->solver_tt,
        autoplay_worker->shared_data->solver_num_threads,
        autoplay_solver_seed(game_runner->seed, game_runner->turn_number,
                             player_on_turn_index),
        autoplay_worker->shared_data->thread_control,
        autoplay_worker->captures_positions, error_stack);
    if (!solved && error_stack_is_empty(error_stack)) {
      // The run was stopped -- by the user, or a contribute task's time limit
      // -- before or during the solve, which chose nothing. The run is being
      // abandoned (a task's results are discarded), so the game plays out on
      // static plays, as a stopped simulation's turn does: every later solve
      // and simulation returns at once.
      return get_top_move_for_player_on_turn(
          game_runner->game, autoplay_worker->move_lists[player_on_turn_index],
          autoplay_worker->captures_positions);
    }
    if (!error_stack_is_empty(error_stack)) {
      error_stack_print_and_reset(error_stack);
      log_fatal("autoplay worker %d failed to solve for player %d on turn %d "
                "of game number %llu with seed %llu",
                autoplay_worker->worker_index, player_on_turn_index + 1,
                game_runner->turn_number + 1,
                (unsigned long long)game_runner->game_number + 1,
                (unsigned long long)game_runner->seed);
    }
    autoplay_worker->turn_was_solved = true;
    return solved;
  }
  const SimArgs *sim_args = (player_on_turn_index == 0)
                                ? &autoplay_worker->args.p1_sim_args
                                : &autoplay_worker->args.p2_sim_args;
  if (sim_args->num_plies == 0) {
    return get_top_move_for_player_on_turn(
        game_runner->game, autoplay_worker->move_lists[player_on_turn_index],
        autoplay_worker->captures_positions);
  }
  return game_runner_get_top_simming_move(autoplay_worker, game_runner);
}

// Returns the played move
const Move *game_runner_play_move(AutoplayWorker *autoplay_worker,
                                  GameRunner *game_runner) {
  if (game_runner_is_game_over(game_runner)) {
    log_fatal("game runner attempted to play a move when the game is over");
  }
  Game *game = game_runner->game;
  const int player_on_turn_index = game_get_player_on_turn_index(game);
  LeavegenSharedData *lg_shared_data =
      game_runner->shared_data->leavegen_shared_data;
  // If we are forcing a draw, we need to draw a rare leave. The drawn
  // leave does not necessarily fit in the bag. If we've reached the
  // target minimum leave count for all leaves, no rare leave can be
  // drawn.
  Rack *player_rack =
      player_get_rack(game_get_player(game, player_on_turn_index));
  const int ld_size = ld_get_size(game_get_ld(game));
  Rack rare_rack_or_move_leave;
  rack_set_dist_size(&rare_rack_or_move_leave, ld_size);

  if (game_runner->force_draw &&
      rack_list_get_rare_rack(lg_shared_data->rack_list, autoplay_worker->prng,
                              &rare_rack_or_move_leave)) {
    // Backup the original rack
    Rack original_rack;
    rack_copy(&original_rack, player_rack);

    // Set the rack to the rare leave
    rack_copy(player_rack, &rare_rack_or_move_leave);

    const Move *forced_move =
        game_runner_get_best_move(autoplay_worker, game_runner);
    // A forced rack is under no obligation to have a legal play, and a
    // pass's equity is a sentinel value that can't be recorded, so passes
    // are skipped entirely here. This is more likely than usual when
    // lg_shared_data->rack_list is restricted to forced racks (see
    // rack_list_create), since those racks are picked externally rather
    // than drawn from the actual remaining tile pool.
    if (move_get_type(forced_move) != GAME_EVENT_PASS) {
      rack_list_add_rack(lg_shared_data->rack_list, &rare_rack_or_move_leave,
                         equity_to_double(move_get_equity(forced_move)));
    }

    rack_copy(player_rack, &original_rack);
  }

  const Move *move = game_runner_get_best_move(autoplay_worker, game_runner);
  const SimArgs *sim_args_for_player =
      (game_get_player_on_turn_index(game_runner->game) == 0)
          ? &autoplay_worker->args.p1_sim_args
          : &autoplay_worker->args.p2_sim_args;

  if (game_runner->turn_number == 0 &&
      move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    game_runner->opening_tiles = move_get_tiles_played(move);
    game_runner->opening_equity = equity_to_double(move_get_equity(move));
  }

  if (lg_shared_data) {
    rack_list_add_rack(lg_shared_data->rack_list, player_rack,
                       equity_to_double(move_get_equity(move)));
  }

  // PAT training: the move just chosen is the next ply for every
  // observation still open in this game. It counts against the observing
  // player when the opponent made it and for them when they made it
  // themselves; an observation that has now seen all its plies is complete
  // and goes to the regression. A pass or exchange scores zero, which is a
  // legitimate "nothing happened" ply.
  PATGenSharedData *pat_gen_shared_data =
      game_runner->shared_data->pat_gen_shared_data;
  if (pat_gen_shared_data) {
    const double move_score = equity_to_double(move_get_score(move));
    for (int obs_index = 0; obs_index < PAT_MAX_LABEL_PLIES; obs_index++) {
      PATPendingObservation *observation = &game_runner->pat_obs[obs_index];
      if (!observation->valid) {
        continue;
      }
      // Plies alternate, and ply 0 of an observation is always the
      // opponent's reply to the move that opened it.
      observation->label +=
          (observation->plies_seen % 2 == 0) ? move_score : -move_score;
      observation->plies_seen++;
      if (observation->plies_seen >= pat_gen_shared_data->label_plies) {
        // Whole game pairs are held out (both games of a pair share a
        // game number), so the held-out rows are never from a game the
        // fit saw.
        PATRegression *target =
            (pat_gen_pair_is_validation(game_runner->game_number))
                ? &pat_gen_shared_data
                       ->heldout_regressions[autoplay_worker->worker_index]
                : &pat_gen_shared_data
                       ->regressions[autoplay_worker->worker_index];
        pat_regression_add_observation_double(target, observation->features,
                                              observation->label);
        observation->valid = false;
      }
    }
  }
  const int pat_pre_move_bag_count =
      pat_gen_shared_data ? bag_get_letters(game_get_bag(game)) : 0;
  get_leave_for_move(move, game, &rare_rack_or_move_leave);
  // Only when a simulation actually ran this turn: sim_results holds
  // whatever the last simulation produced, so passing it on a static player's
  // turn, on a turn a solver decided, or on a simmer's turn with one legal
  // play -- which is recorded as the static analysis of that play -- would
  // attribute another turn's analysis to this one.
  const bool simmed_this_turn = autoplay_worker->turn_was_simmed;
  // And it inferred first exactly when game_runner_get_sim_move turned
  // inference on for the sim: the player infers, and the opponent has a
  // previous move that was not a pass to infer from. The inference runs
  // inside the simulation, so a turn that did not simulate did not infer.
  const bool inferred_this_turn =
      simmed_this_turn && sim_args_for_player->use_inference &&
      game_runner->turn_number > 0 &&
      move_get_type(&game_runner->previous_move) != GAME_EVENT_PASS;
  // The move list holds the candidates this turn; it is reused next turn, so a
  // recorder that keeps them must copy.
  autoplay_results_add_move(
      autoplay_worker->autoplay_results, game_runner->game, move,
      // previous_move is only meaningful once a turn has actually been
      // played; game_runner->previous_move is stale/uninitialized before that.
      game_runner->turn_number > 0 ? &game_runner->previous_move : NULL,
      &rare_rack_or_move_leave,
      autoplay_worker
          ->move_lists[game_get_player_on_turn_index(game_runner->game)],
      simmed_this_turn ? autoplay_worker->sim_results : NULL,
      inferred_this_turn ? autoplay_worker->inference_results : NULL,
      autoplay_worker->turn_was_solved
          ? autoplay_solver_get_analysis(autoplay_worker->solver_ctx)
          : NULL,
      (int)game_runner->game_number, game_runner->pair_game_number,
      game_runner->turn_number, autoplay_worker->args.position_play_cap);

  // Print board with move about to be played if requested
  if (autoplay_worker->args.print_boards) {
    StringBuilder *output = string_builder_create();
    if (game_runner->pair_game_number == 0) {
      string_builder_add_formatted_string(
          output, "\n=== Game %llu, Turn %d ===\n",
          (unsigned long long)game_runner->game_number + 1,
          game_runner->turn_number + 1);
    } else {
      string_builder_add_formatted_string(
          output, "\n=== Game Pair %llu, Game %d, Turn %d ===\n",
          (unsigned long long)game_runner->game_number + 1,
          game_runner->pair_game_number, game_runner->turn_number + 1);
    }
    string_builder_add_game(
        game, NULL, autoplay_worker->args.game_string_options, NULL, output);
    string_builder_add_move(output, game_get_board(game), move,
                            game_get_ld(game), true);
    string_builder_add_string(output, "\n");
    const SimArgs *sim_args = (player_on_turn_index == 0)
                                  ? &autoplay_worker->args.p1_sim_args
                                  : &autoplay_worker->args.p2_sim_args;
    if (autoplay_worker->turn_was_simmed &&
        !autoplay_worker->args.use_play_chooser[player_on_turn_index]) {
      char *sim_str = sim_results_get_string(
          game, autoplay_worker->sim_results, sim_args->max_num_display_plays,
          sim_args->max_num_display_plies, -1, -1, NULL, 0, false, false, false,
          NULL);
      string_builder_add_string(output, sim_str);
      free(sim_str);
      if (sim_args->use_inference && game_runner->turn_number > 0 &&
          move_get_type(&game_runner->previous_move) != GAME_EVENT_PASS) {
        string_builder_add_inference(
            output, autoplay_worker->inference_results, game_get_ld(game),
            sim_args->inference_args.leave_list_capacity, false);
      }
    }
    thread_control_print(autoplay_worker->args.thread_control,
                         string_builder_peek(output));
    string_builder_destroy(output);
  }

  play_move(move, game, NULL);

  // PAT training: open the next observation from the post-move
  // board. Gated on the pre-move bag matching the deployment gate for the
  // defense term, and skipped when the move ended the game (its
  // observation could never be labeled).
  if (pat_gen_shared_data && pat_pre_move_bag_count > 0 && !game_over(game)) {
    for (int obs_index = 0; obs_index < PAT_MAX_LABEL_PLIES; obs_index++) {
      PATPendingObservation *observation = &game_runner->pat_obs[obs_index];
      if (observation->valid) {
        continue;
      }
      // The features come from the post-move board, so the rack excluded
      // from the unseen pool is the move's leave: the played tiles are
      // already off the pool as board tiles, and dist - board_post - leave
      // is exactly what move evaluation computes at decision time as
      // dist - board_pre - rack_pre. The row is the one the live weights'
      // combination rule makes linear, so that fitting and evaluating
      // agree; with gamma 1 it is the plain sum.
      pat_extract_features_combined(
          board_get_readonly_lanes(game_get_board(game), 0), game_get_ld(game),
          &rare_rack_or_move_leave, pat_gen_shared_data->pat,
          rack_get_total_letters(
              player_get_rack(game_get_player(game, 1 - player_on_turn_index))),
          observation->features);
      observation->plies_seen = 0;
      observation->label = 0.0;
      observation->valid = true;
      break;
    }
  }

  if (game_runner->game_one_move_behind && game_runner->turn_number > 0) {
    play_move(&game_runner->previous_move, game_runner->game_one_move_behind,
              NULL);
  }
  move_copy(&game_runner->previous_move, move);
  game_runner->turn_number++;
  return move;
}

static void game_runner_assess_overtime(AutoplayWorker *autoplay_worker,
                                        GameRunner *game_runner) {
  const AutoplayArgs *args = &autoplay_worker->args;
  game_timer_end_turn(&game_runner->game_timer);
  for (int player_index = 0; player_index < 2; player_index++) {
    if (!args->use_play_chooser[player_index]) {
      continue;
    }
    game_runner->timing.active[player_index] = true;
    game_runner->timing.seconds_used[player_index] =
        game_timer_get_seconds_used(&game_runner->game_timer, player_index);
    game_runner->timing.overtime_seconds[player_index] =
        game_timer_get_overtime_seconds(&game_runner->game_timer, player_index);
    const int penalty_points = autoplay_overtime_penalty_points(
        game_runner->timing.overtime_seconds[player_index],
        args->overtime_penalty_points, args->overtime_period_seconds);
    game_runner->timing.penalty_points[player_index] = penalty_points;
    player_add_to_score(game_get_player(game_runner->game, player_index),
                        -int_to_equity(penalty_points));
  }
}

void print_current_status(AutoplayWorker *autoplay_worker,
                          AutoplayIterCompletedOutput *iter_completed_output,
                          const GameRunner *game_runner) {
  StringBuilder *status_sb = string_builder_create();
  AutoplaySharedData *shared_data = autoplay_worker->shared_data;
  string_builder_add_formatted_string(
      status_sb, "Played %ld games in %.3f seconds.",
      iter_completed_output->iter_count_completed,
      iter_completed_output->time_elapsed);
  const LeavegenSharedData *lg_shared_data = shared_data->leavegen_shared_data;
  if (lg_shared_data) {
    string_builder_add_formatted_string(
        status_sb,
        " Played %ld games in generation %d with %ld rack under target "
        "count.\n",
        iter_completed_output->iter_count_completed -
            lg_shared_data->gen_start_games,
        lg_shared_data->gens_completed + 1,
        rack_list_get_racks_below_target_count(lg_shared_data->rack_list));
  } else {
    // The game just completed, so a run can be followed game by game
    // (a game pair's two games share a seed).
    const Game *game = game_runner->game;
    string_builder_add_formatted_string(
        status_sb, " Last game: seed %llu p1 %d p2 %d\n",
        (unsigned long long)game_runner->seed,
        equity_to_int(player_get_score(game_get_player(game, 0))),
        equity_to_int(player_get_score(game_get_player(game, 1))));
  }
  thread_control_print(autoplay_worker->args.thread_control,
                       string_builder_peek(status_sb));
  string_builder_destroy(status_sb);
}

void autoplay_add_game(AutoplayWorker *autoplay_worker,
                       const GameRunner *game_runner,
                       const GameRunner *pair_runner, bool divergent) {
  autoplay_results_add_game_with_pair(
      autoplay_worker->autoplay_results, game_runner->game,
      game_runner->turn_number, divergent, game_runner->seed,
      &game_runner->timing, pair_runner ? pair_runner->game : NULL,
      game_runner->opening_tiles, game_runner->opening_equity);
  AutoplayIterCompletedOutput iter_completed_output;
  autoplay_complete_iter(autoplay_worker->shared_data, &iter_completed_output);
  if (iter_completed_output.print_info) {
    print_current_status(autoplay_worker, &iter_completed_output, game_runner);
  }
}

void play_autoplay_game_or_game_pair(AutoplayWorker *autoplay_worker,
                                     GameRunner *game_runner1,
                                     GameRunner *game_runner2,
                                     const AutoplayIterOutput *iter_output) {
  const int starting_player_index = (int)(iter_output->iter_count % 2);
  game_runner_start(autoplay_worker, game_runner1, iter_output,
                    starting_player_index, game_runner2 ? 1 : 0);
  if (game_runner2) {
    game_runner_start(autoplay_worker, game_runner2, iter_output,
                      1 - starting_player_index, 2);
  }
  bool games_are_divergent = false;
  // Positions recorded this turn are held until the comparison below says
  // whether it is the pair's first divergence, and only that turn's are kept.
  AutoplayResults *results = autoplay_worker->autoplay_results;
  const bool keep_first_divergence =
      game_runner2 && autoplay_results_keeps_first_divergences(results);
  while (true) {
    // Each game's move, as that game's runner kept it after playing it.
    // Not the pointer game_runner_play_move returns: a solved turn returns
    // the worker's one solver buffer, which the other game's solve
    // overwrites, so two solved turns compared as one move however
    // differently they were played.
    const Move *move1 = NULL;
    bool game1_is_over = game_runner_is_game_over(game_runner1);
    if (!game1_is_over) {
      game_runner_play_move(autoplay_worker, game_runner1);
      move1 = &game_runner1->previous_move;
    }

    const Move *move2 = NULL;
    bool game2_is_over = true;
    if (game_runner2) {
      game2_is_over = game_runner_is_game_over(game_runner2);
      if (!game2_is_over) {
        game_runner_play_move(autoplay_worker, game_runner2);
        move2 = &game_runner2->previous_move;
      }
    }

    if (game1_is_over && game2_is_over) {
      break;
    }

    // It is guaranteed that at least one move is not null
    // at this point.
    if (!games_are_divergent &&
        (!move1 || !move2 ||
         compare_moves_without_equity(move1, move2, true) != -1)) {
      games_are_divergent = true;
      if (keep_first_divergence) {
        autoplay_results_commit_positions(results);
      }
    }
    if (keep_first_divergence) {
      autoplay_results_discard_positions(results);
    }
  }
  game_runner_assess_overtime(autoplay_worker, game_runner1);
  if (game_runner2) {
    game_runner_assess_overtime(autoplay_worker, game_runner2);
  }
  if (autoplay_worker->args.print_boards) {
    StringBuilder *output = string_builder_create();
    if (game_runner1->pair_game_number == 0) {
      string_builder_add_formatted_string(
          output, "\n=== Game %llu (Final) ===\n",
          (unsigned long long)game_runner1->game_number + 1);
    } else {
      string_builder_add_formatted_string(
          output, "\n=== Game Pair %llu, Game %d (Final) ===\n",
          (unsigned long long)game_runner1->game_number + 1,
          game_runner1->pair_game_number);
    }
    string_builder_add_game(game_runner1->game, NULL,
                            autoplay_worker->args.game_string_options, NULL,
                            output);
    if (game_runner2) {
      string_builder_add_formatted_string(
          output, "\n=== Game Pair %llu, Game %d (Final) ===\n",
          (unsigned long long)game_runner2->game_number + 1,
          game_runner2->pair_game_number);
      string_builder_add_game(game_runner2->game, NULL,
                              autoplay_worker->args.game_string_options, NULL,
                              output);
    }
    thread_control_print(autoplay_worker->args.thread_control,
                         string_builder_peek(output));
    string_builder_destroy(output);
  }
  autoplay_add_game(autoplay_worker, game_runner1, NULL, games_are_divergent);
  if (game_runner2) {
    // We do not check for min leave counts here because leave gen
    // does not use game pairs and therefore does not have a second
    // game runner. The second game carries the pair's combined spread.
    autoplay_add_game(autoplay_worker, game_runner2, game_runner1,
                      games_are_divergent);
    // Both games of the pair are final here, which is the only point at which
    // the pair's own outcome exists. Recorded whether or not the two games
    // diverged: an identically-played pair is a 1-1 tie and belongs in the
    // distribution, since dropping it would condition the sample on its
    // outcome.
    autoplay_results_add_game_pair(autoplay_worker->autoplay_results,
                                   game_runner1->game, game_runner2->game);
  }
}

bool target_min_leave_count_reached(AutoplayWorker *autoplay_worker) {
  const LeavegenSharedData *leavegen_shared_data =
      autoplay_worker->shared_data->leavegen_shared_data;
  return leavegen_shared_data && rack_list_get_racks_below_target_count(
                                     leavegen_shared_data->rack_list) == 0;
}

void autoplay_single_generation(AutoplayWorker *autoplay_worker,
                                GameRunner *game_runner1,
                                GameRunner *game_runner2) {
  ThreadControl *thread_control = autoplay_worker->args.thread_control;
  AutoplayIterOutput iter_output;
  while (
      // Check if autoplay was exited by the user.
      thread_control_get_status(thread_control) !=
          THREAD_CONTROL_STATUS_USER_INTERRUPT &&
      // Check if the maximum iteration has been reached.
      !autoplay_get_next_iter_output(autoplay_worker->shared_data,
                                     &iter_output) &&
      // Check if the target minimum leave count has been reached.
      // This will never be true for the default autoplay mode.
      !target_min_leave_count_reached(autoplay_worker)) {
    play_autoplay_game_or_game_pair(autoplay_worker, game_runner1, game_runner2,
                                    &iter_output);
  }
}

void autoplay_leave_gen(AutoplayWorker *autoplay_worker,
                        GameRunner *game_runner) {
  AutoplaySharedData *shared_data = autoplay_worker->shared_data;
  LeavegenSharedData *lg_shared_data = shared_data->leavegen_shared_data;
  for (int i = 0; i < lg_shared_data->num_gens; i++) {
    autoplay_single_generation(autoplay_worker, game_runner, NULL);
    checkpoint_wait(lg_shared_data->postgen_checkpoint, shared_data);
    if (thread_control_get_status(shared_data->thread_control) ==
        THREAD_CONTROL_STATUS_USER_INTERRUPT) {
      break;
    }
  }
}

void autoplay_pat_gen(AutoplayWorker *autoplay_worker,
                      GameRunner *game_runner) {
  AutoplaySharedData *shared_data = autoplay_worker->shared_data;
  PATGenSharedData *pat_gen_shared_data = shared_data->pat_gen_shared_data;
  for (int gen_index = 0; gen_index < pat_gen_shared_data->num_gens;
       gen_index++) {
    autoplay_single_generation(autoplay_worker, game_runner, NULL);
    checkpoint_wait(pat_gen_shared_data->postgen_checkpoint, shared_data);
    if (thread_control_get_status(shared_data->thread_control) ==
        THREAD_CONTROL_STATUS_USER_INTERRUPT) {
      break;
    }
  }
}

// - The sim args for autoplay share the same inference results, since only one
//   inference will be running at a time per autoplay worker.
// - The game of the sim args needs to be set explicitly before each move, since
//   there is only one pair of p1 and p2 sim args but potentially 2 games if
//   using game pairs.
void init_sim_args_for_player(AutoplayWorker *autoplay_worker,
                              int player_index) {
  SimArgs *sim_args = (player_index == 0) ? &autoplay_worker->args.p1_sim_args
                                          : &autoplay_worker->args.p2_sim_args;
  sim_args->bai_options.parent_worker_thread_index =
      autoplay_worker->worker_index;
  sim_args->inference_args.parent_worker_thread_index =
      autoplay_worker->worker_index;
  sim_args->inference_results = autoplay_worker->inference_results;
}

void *autoplay_worker(void *uncasted_autoplay_worker) {
  AutoplayWorker *autoplay_worker = (AutoplayWorker *)uncasted_autoplay_worker;
  const AutoplayArgs *args = &autoplay_worker->args;
  GameRunner *game_runner1 = game_runner_create(autoplay_worker);
  init_sim_args_for_player(autoplay_worker, 0);
  init_sim_args_for_player(autoplay_worker, 1);
  GameRunner *game_runner2 = NULL;
  switch (args->type) {
  case AUTOPLAY_TYPE_DEFAULT:
    if (args->use_game_pairs) {
      game_runner2 = game_runner_create(autoplay_worker);
    }
    autoplay_single_generation(autoplay_worker, game_runner1, game_runner2);
    game_runner_destroy(game_runner2);
    break;
  case AUTOPLAY_TYPE_LEAVE_GEN:
    autoplay_leave_gen(autoplay_worker, game_runner1);
    break;
  case AUTOPLAY_TYPE_PAT_GEN:
    autoplay_pat_gen(autoplay_worker, game_runner1);
    break;
  }

  game_runner_destroy(game_runner1);
  return NULL;
}

void parse_min_rack_targets(const AutoplayArgs *args,
                            const StringSplitter *split_min_rack_targets,
                            int *min_rack_targets, ErrorStack *error_stack) {
  int num_gens = string_splitter_get_number_of_items(split_min_rack_targets);
  for (int i = 0; i < num_gens; i++) {
    const char *item = string_splitter_get_item(split_min_rack_targets, i);
    if (is_string_empty_or_whitespace(item)) {
      error_stack_push(
          error_stack, ERROR_STATUS_AUTOPLAY_MALFORMED_MINIMUM_LEAVE_TARGETS,
          get_formatted_string("found an empty value for one or more of the "
                               "minimum rack targets: %s",
                               args->num_games_or_min_rack_targets));
      return;
    }
    min_rack_targets[i] = string_to_int(item, error_stack);
    if (!error_stack_is_empty(error_stack) || min_rack_targets[i] < 0) {
      error_stack_push(
          error_stack, ERROR_STATUS_AUTOPLAY_MALFORMED_MINIMUM_LEAVE_TARGETS,
          get_formatted_string("failed to parse minimum rack targets: %s",
                               args->num_games_or_min_rack_targets));
      return;
    }
  }
}

void valid_autoplay_results_options(const AutoplayResults *autoplay_results,
                                    const AutoplayArgs *args,
                                    ErrorStack *error_stack) {
  const uint64_t options = autoplay_results_get_options(autoplay_results);
  if (options == 0) {
    return;
  }
  // The other recorders accumulate per-leave or per-rack statistics that game
  // pairs would double-count. The positions recorder does not: it records each
  // turn independently and carries its own game and pair numbers, so it is
  // meaningful alongside pairing.
  const uint64_t pairable_options =
      autoplay_results_build_option(AUTOPLAY_RECORDER_TYPE_GAME) |
      autoplay_results_build_option(AUTOPLAY_RECORDER_TYPE_POSITION);
  if ((options & ~pairable_options) != 0 && args->use_game_pairs) {
    error_stack_push(
        error_stack, ERROR_STATUS_AUTOPLAY_INVALID_OPTIONS,
        string_duplicate("the game pairs setting can only be used with the "
                         "games and positions recorders"));
    return;
  }
  // A first divergence is a pair's: without pairs nothing would be kept.
  if (autoplay_results_keeps_first_divergences(autoplay_results) &&
      !args->use_game_pairs) {
    error_stack_push(
        error_stack, ERROR_STATUS_AUTOPLAY_INVALID_OPTIONS,
        string_duplicate("divergentpositions keeps each game pair's first "
                         "divergence, so it needs game pairs (-gp true)"));
    return;
  }
}

int autoplay_solver_num_threads(int total_num_threads,
                                int num_concurrent_games) {
  if (num_concurrent_games < 1) {
    num_concurrent_games = 1;
  }
  const int share = total_num_threads / num_concurrent_games;
  return share > 1 ? share : 1;
}

void autoplay(const AutoplayArgs *args, AutoplayResults *autoplay_results,
              ErrorStack *error_stack) {
  valid_autoplay_results_options(autoplay_results, args, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  const bool is_leavegen_mode = args->type == AUTOPLAY_TYPE_LEAVE_GEN;
  const bool is_patgen_mode = args->type == AUTOPLAY_TYPE_PAT_GEN;
  autoplay_results_set_play_chooser_config(
      autoplay_results, args->use_play_chooser, args->time_control_seconds,
      args->overtime_penalty_points, args->overtime_period_seconds);
  int num_gens = 1;
  int *min_rack_targets = NULL;
  uint64_t *pat_games_per_gen = NULL;
  uint64_t first_gen_num_games;
  if (is_patgen_mode) {
    // The first argument is a comma-separated list of games per generation.
    StringSplitter *split_games_per_gen =
        split_string(args->num_games_or_min_rack_targets, ',', false);
    num_gens = string_splitter_get_number_of_items(split_games_per_gen);
    pat_games_per_gen = malloc_or_die(sizeof(uint64_t) * (size_t)num_gens);
    for (int gen_index = 0; gen_index < num_gens; gen_index++) {
      pat_games_per_gen[gen_index] = string_to_uint64(
          string_splitter_get_item(split_games_per_gen, gen_index),
          error_stack);
      if (!error_stack_is_empty(error_stack) ||
          pat_games_per_gen[gen_index] == 0) {
        error_stack_push(
            error_stack, ERROR_STATUS_AUTOPLAY_MALFORMED_NUM_GAMES,
            get_formatted_string(
                "failed to parse the games per generation (every generation "
                "needs at least one game): %s",
                args->num_games_or_min_rack_targets));
        string_splitter_destroy(split_games_per_gen);
        free(pat_games_per_gen);
        return;
      }
    }
    string_splitter_destroy(split_games_per_gen);
    first_gen_num_games = pat_games_per_gen[0];
  } else if (is_leavegen_mode) {
    StringSplitter *split_min_rack_targets =
        split_string(args->num_games_or_min_rack_targets, ',', false);
    num_gens = string_splitter_get_number_of_items(split_min_rack_targets);
    min_rack_targets = malloc_or_die((sizeof(int)) * (num_gens));
    parse_min_rack_targets(args, split_min_rack_targets, min_rack_targets,
                           error_stack);
    string_splitter_destroy(split_min_rack_targets);
    if (!error_stack_is_empty(error_stack)) {
      free(min_rack_targets);
      error_stack_push(
          error_stack, ERROR_STATUS_AUTOPLAY_MALFORMED_MINIMUM_LEAVE_TARGETS,
          get_formatted_string("failed to parse minimum rack targets: %s",
                               args->num_games_or_min_rack_targets));
      return;
    }
    first_gen_num_games =
        args->leavegen_max_games > 0 ? args->leavegen_max_games : UINT64_MAX;
  } else {
    first_gen_num_games =
        string_to_uint64(args->num_games_or_min_rack_targets, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack, ERROR_STATUS_AUTOPLAY_MALFORMED_NUM_GAMES,
                       get_formatted_string(
                           "failed to parse the specified number of games: %s",
                           args->num_games_or_min_rack_targets));
      return;
    }
  }

  ThreadControl *thread_control = args->thread_control;

  autoplay_results_reset(autoplay_results);

  KLV *klv = NULL;
  bool show_divergent_results = args->use_game_pairs;
  if (is_leavegen_mode) {
    // We can use player index 0 here since it is guaranteed that
    // players share the the KLV.
    klv = players_data_get_klv(args->game_args->players_data, 0);
    show_divergent_results = false;
  } else if (is_patgen_mode) {
    // Like leavegen, patgen never uses game pairs.
    show_divergent_results = false;
  }

  const int autoplay_num_threads = args->num_threads;

  AutoplayResults **autoplay_results_list =
      malloc_or_die((sizeof(AutoplayResults *)) * (autoplay_num_threads));

  AutoplaySharedData *shared_data = autoplay_shared_data_create(
      args, autoplay_num_threads, first_gen_num_games, autoplay_results,
      autoplay_results_list, klv, num_gens, min_rack_targets,
      args->forced_racks, args->num_forced_racks, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(autoplay_results_list);
    free(min_rack_targets);
    free(pat_games_per_gen);
    return;
  }

  // Every worker plays a game at a time, and a worker the run has no game
  // for exits before it plays one, so the run plays this many games at once.
  int num_concurrent_games = autoplay_num_threads;
  if (!is_leavegen_mode && !is_patgen_mode &&
      first_gen_num_games < (uint64_t)num_concurrent_games) {
    num_concurrent_games = (int)first_gen_num_games;
  }
  shared_data->solver_num_threads = autoplay_solver_num_threads(
      args->total_num_threads, num_concurrent_games);

  if (is_patgen_mode) {
    // We can use player index 0 here since it is guaranteed that the
    // players share the PAT weights (see impl_pat_gen).
    PATWeights *pat = players_data_get_pat(args->game_args->players_data, 0);
    if (!pat) {
      log_fatal("patgen started without PAT weights loaded");
    }
    shared_data->pat_gen_shared_data = pat_gen_shared_data_create(
        pat, args->data_paths, args->pat_gen_output_name, autoplay_num_threads,
        num_gens, pat_games_per_gen, args->pat_label_plies);
  }

  AutoplayWorker **autoplay_workers =
      malloc_or_die((sizeof(AutoplayWorker *)) * (autoplay_num_threads));
  cpthread_t *worker_ids =
      malloc_or_die((sizeof(cpthread_t)) * (autoplay_num_threads));

  for (int thread_index = 0; thread_index < autoplay_num_threads;
       thread_index++) {
    autoplay_workers[thread_index] = autoplay_worker_create(
        args, autoplay_results, thread_index, shared_data);
    autoplay_results_list[thread_index] =
        autoplay_workers[thread_index]->autoplay_results;
    cpthread_create(&worker_ids[thread_index], autoplay_worker,
                    autoplay_workers[thread_index]);
  }

  autoplay_results_set_status_data(
      autoplay_results, autoplay_results_list, autoplay_num_threads, false,
      args->human_readable, show_divergent_results);

  for (int thread_index = 0; thread_index < autoplay_num_threads;
       thread_index++) {
    cpthread_join(worker_ids[thread_index]);
  }

  // The stats have already been combined in leavegen mode
  if (!is_leavegen_mode) {
    autoplay_results_consolidate(autoplay_results_list, autoplay_num_threads,
                                 autoplay_results);
  }

  autoplay_results_set_status_data(autoplay_results, NULL, 0, true,
                                   args->human_readable,
                                   show_divergent_results);

  free(autoplay_results_list);

  for (int thread_index = 0; thread_index < autoplay_num_threads;
       thread_index++) {
    autoplay_worker_destroy(autoplay_workers[thread_index]);
  }

  // The trained weights live in the players_data-owned PATWeights object,
  // which every position load re-reads, so no reload is needed; write the
  // final weights under the plain output name for convenience.
  if (is_patgen_mode) {
    pat_write(shared_data->pat_gen_shared_data->pat, args->data_paths,
              args->pat_gen_output_name, error_stack);
  }

  free(autoplay_workers);
  free(worker_ids);
  char *postgen_error = NULL;
  if (shared_data->leavegen_shared_data) {
    postgen_error = shared_data->leavegen_shared_data->postgen_error;
    shared_data->leavegen_shared_data->postgen_error = NULL;
  }
  autoplay_shared_data_destroy(shared_data);
  free(min_rack_targets);
  free(pat_games_per_gen);

  // Only reload KLV if it was modified during leavegen
  if (is_leavegen_mode) {
    players_data_reload(args->game_args->players_data, PLAYERS_DATA_TYPE_KLV,
                        args->data_paths, error_stack);
  }
  if (postgen_error) {
    error_stack_push(error_stack, ERROR_STATUS_RW_WRITE_ERROR, postgen_error);
  }

  if (args->print_results) {
    char *autoplay_results_string = autoplay_results_to_string(
        autoplay_results, args->human_readable, show_divergent_results);
    thread_control_print(thread_control, autoplay_results_string);
    free(autoplay_results_string);
  }
}
