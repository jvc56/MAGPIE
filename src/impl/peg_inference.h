#ifndef PEG_INFERENCE_H
#define PEG_INFERENCE_H

#include "../ent/inference_args.h"
#include "../ent/inference_results.h"
#include "../ent/move.h"
#include "../impl/peg.h"
#include "../util/io_util.h"

// Pre-endgame (PEG) inference: evaluates candidate opponent racks by asking,
// for each candidate leave, whether a PEG player holding that rack would have
// made the observed play. It is the pre-endgame analogue of simmed inference
// (simmed_inference.c) -- same leave enumeration, weighting, and result
// recording -- but the inner "would they play this?" test runs one pre-endgame
// solve over the candidate's top moves and compares their score+win utilities,
// rather than an inner Monte Carlo sim.
//
// Applies only when the bag is in the PEG range [PEG_MIN_BAG, PEG_MAX_BAG]; the
// caller is responsible for gating on bag size.
//
// The per-move value is the simmer's score+win utility (sim_utility_blend):
// win% blended with a sigmoid of the mean spread, in [0, 1]. The whole codebase
// is unifying on this metric, so inference uses it too: the acceptance gap is
// best_utility - observed_utility, in the same [0, 1] units as the margin, and
// no win_pct table is needed.

// Maximum leave size evaluated by exhaustive enumeration; larger leaves are
// Monte Carlo sampled within the time budget. Smaller than simmed inference's
// because each inner evaluation is a full PEG solve (far more expensive than a
// short sim), so exhaustive enumeration saturates sooner. Overridable per call
// via PegInferenceArgs.exhaustive_max_leave.
#define PEG_INFER_EXHAUSTIVE_MAX_DEFAULT 1

// Callback invoked after each candidate leave is fully evaluated (debugging /
// inspection). gap and the utilities are in [0, 1] utility units.
typedef void (*PegInferLeaveCallback)(const Rack *candidate_leave,
                                      const MoveList *move_list, int obs_idx,
                                      double gap, double weight,
                                      const Game *inner_game, void *user_data);

// Arguments for pre-endgame inference. Wraps InferenceArgs and adds the
// parameters needed for the inner PEG evaluation of candidate racks.
typedef struct PegInferenceArgs {
  // Standard inference parameters: game state, target player index, known
  // tiles, equity margin, thread count, etc.
  const InferenceArgs *base;

  // The exact Move the target was observed to play.
  //   - Tile placement: position/tiles/score used for matching.
  //   - Exchange: only the tile count is matched.
  const Move *observed_move;

  // Candidate moves generated per leave -- the "top-N by equity" field the
  // inner PEG solve ranks. The observed move is force-included even if it falls
  // outside the top N (a low-static-equity setup can still be the pre-endgame
  // best). Small (single digits) keeps each PEG solve affordable. 0 = default.
  int num_candidate_plays;

  // Score+win utility weights (sim_utility_blend, sim_args.h). The per-move
  // value is (w_winpct*win% + w_spread*sigmoid(spread/spread_scale)) /
  // (w_winpct + w_spread), in [0, 1]. Zero/unset uses w_winpct 1.0, w_spread
  // 0.0 (pure win%), spread_scale 100.0. Set w_spread > 0 to weight margin.
  double utility_w_winpct;
  double utility_w_spread;
  double utility_spread_scale;

  // Inner PEG-solver configuration. The cost/depth levers that scale the
  // inference from 1peg (afford a deeper solve per leave) to 4peg (cheap greedy
  // solve, fewer candidates, sampled scenarios). Optional; 0/false uses a
  // per-bag default chosen by peg_infer.
  //   greedy_seed_only    : rank the field by the stage-0 greedy win% only
  //                         (bounded, deterministic; the default for big bags).
  //   peg_max_stage       : cap on PEG halving stages when not greedy-only.
  //   peg_scenario_stride : PEG scenario sampling (1 = full, k > 1 = sampled).
  //   peg_opp_model       : opponent model for the inner solve (default rational).
  bool greedy_seed_only;
  int peg_max_stage;
  int peg_scenario_stride;
  PegOppModel peg_opp_model;

  // Per-leave wall-clock budget (seconds) for the inner PEG solve. 0 = the
  // solver's own default. Scaled down as the outer leave count grows.
  double peg_time_budget_s;

  // Leave enumeration: exhaustive when leave_size <= exhaustive_max_leave, else
  // Monte Carlo sampling. 0 = PEG_INFER_EXHAUSTIVE_MAX_DEFAULT.
  int exhaustive_max_leave;

  // Wall-clock budget (seconds) for the whole Monte Carlo sampling loop.
  // Sampling stops when the budget is spent or an external interrupt arrives.
  double time_budget_s;

  // Logistic weight margin in utility units [0, 1]:
  //   gap    = best_utility - observed_utility
  //   weight = logistic centred at peg_utility_margin:
  //     gap <= 0                 -> ~1.0  (observed is the PEG best)
  //     gap = peg_utility_margin -> 0.5   (borderline)
  //     gap >> peg_utility_margin -> ~0.0 (clearly not the best)
  // Must be > 0; <= 0 uses a default of 0.3 (pre-endgame utility gaps run much
  // larger than the simmer's few-point margins).
  double peg_utility_margin;

  // Static pre-filter margin, in equity points. For leaves larger than one
  // tile, a candidate whose observed move trails the top static move by more
  // than 4x this is skipped before any PEG solve. Kept generous in the
  // pre-endgame, where a low-static setup can still be the win%-best move.
  // <= 0 uses a default of 20.0.
  double static_prefilter_margin;

  // Optional per-leave callback; NULL to disable.
  PegInferLeaveCallback leave_callback;
  void *leave_callback_data;
} PegInferenceArgs;

// Runs pre-endgame inference, populating results with a weighted distribution
// over likely opponent leaves. Threading: the outer candidate loop is
// single-threaded; all base->num_threads are used for each inner PEG solve.
void peg_infer(const PegInferenceArgs *args, InferenceResults *results,
               ErrorStack *error_stack);

#endif
