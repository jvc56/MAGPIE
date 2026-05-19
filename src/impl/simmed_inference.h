#ifndef SIMMED_INFERENCE_H
#define SIMMED_INFERENCE_H

#include "../ent/inference_args.h"
#include "../ent/inference_results.h"
#include "../ent/move.h"
#include "../ent/sim_results.h"
#include "../ent/win_pct.h"
#include "../util/io_util.h"

// Maximum leave size for which exhaustive enumeration is used.
// For leave_size > this value, Monte Carlo sampling is used instead.
#define SIMMED_INFER_EXHAUSTIVE_MAX 2

// Callback invoked after each candidate leave is fully evaluated.
//
// Parameters:
//   candidate_leave  — the leave rack being evaluated
//   move_list        — candidate moves with sim equities populated
//                      (static equities if statically rejected before any sim)
//   sim_results      — inner sim results from the last sim stage run;
//                      may have 0 iterations if statically pre-filtered
//   obs_idx          — index of the observed move in move_list
//   gap              — best_sim_equity - obs_sim_equity (or static gap if
//                      rejected before sim)
//   weight           — final acceptance weight in [0, 1]
//   inner_game       — the inner game used for evaluation (for board/LD access)
//   user_data        — caller-supplied context pointer
typedef void (*SimmedInferLeaveCallback)(const Rack *candidate_leave,
                                         const MoveList *move_list,
                                         const SimResults *sim_results,
                                         int obs_idx, double gap, double weight,
                                         const Game *inner_game,
                                         void *user_data);

// Arguments for simulation-based inference. Wraps InferenceArgs and adds
// the parameters needed for inner-sim evaluation of candidate racks.
typedef struct SimmedInferenceArgs {
  // Standard inference parameters: game state, target player index, known
  // tiles, equity margin, thread count, etc.
  const InferenceArgs *base;

  // The exact Move that was observed by the inferring player.
  //   - Tile placement: all fields (position, tiles, score) used for matching.
  //   - Exchange: only tiles_played (count) is matched; position/tiles ignored.
  const Move *observed_move;

  // Win-percentage table required by the inner simulator.
  WinPct *win_pcts;

  // Number of candidate plays to generate per rack evaluation.
  int num_candidate_plays;

  // Sim plies for each inner evaluation (1 or 2 recommended).
  int num_inner_sim_plies;

  // Quick probe: total sim iterations run first to detect obvious cases.
  // If gap is clearly above or below the accept threshold, stops here.
  int probe_iterations;

  // Full evaluation: additional iterations run when the probe result is
  // uncertain (gap is within the midrange). The total in the uncertain case
  // is probe_iterations + full_iterations.
  int full_iterations;

  // Wall-clock budget (seconds) for the Monte Carlo loop when
  // leave_size > SIMMED_INFER_EXHAUSTIVE_MAX. Sampling stops when either
  // the budget is exhausted or an external interrupt arrives.
  double time_budget_s;

  // Optional per-leave callback for debugging/inspection. Called after every
  // candidate leave evaluation with the inner move list and sim results.
  // Set to NULL to disable (default behaviour).
  SimmedInferLeaveCallback leave_callback;
  void *leave_callback_data;

  // Sim equity margin in display points (e.g. 3.0) used by the weight function.
  //   gap  = best_sim_equity - observed_sim_equity
  //   weight = logistic centred at sim_equity_margin:
  //     gap <= 0             → ~1.0  (observed is the sim best)
  //     gap = sim_equity_margin → 0.5  (borderline)
  //     gap >> sim_equity_margin → ~0.0  (clearly not the best)
  // Must be > 0; if <= 0 a default of 3.0 is used.
  double sim_equity_margin;
} SimmedInferenceArgs;

// Runs simulation-based inference, populating results with a weighted
// distribution over likely opponent leaves. Compared to static inference,
// this evaluates whether a candidate rack would produce the observed play
// under sim (not static-equity) play, making it sensitive to setup plays.
//
// Threading: the outer candidate loop is single-threaded; all threads in
// base->num_threads are used for each inner sim evaluation.
void simmed_infer(const SimmedInferenceArgs *args, InferenceResults *results,
                  ErrorStack *error_stack);

#endif
