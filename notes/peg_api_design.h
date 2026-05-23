#ifndef PEG_H
#define PEG_H

#include <stdbool.h>
#include <stdint.h>
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"

// ============================================================================
// Unified pre-endgame (PEG) solver
//
// Handles 1, 2, 3, and 4 tiles in bag (unseen 8..11). Larger positions are
// rejected — PEG is not for midgame.
//
// Design:
// - Stage 0 (greedy): movegen + sort by static equity descending, then
//   per-cand greedy d=0 playout. Cands processed top-equity-first, so
//   interruption returns the partial top-K by greedy-evaluated-so-far.
// - Stage 1..6: halving (32→16→8→4→2→1). Each stage adds one ply to the
//   inner depth (non-emptier inner d, emptier endgame plies) — parity is
//   preserved between cand kinds. Stage 1 = d=0 / plies=2; Stage 6 =
//   d=5 / plies=7.
// - Time budget (PegArgs.time_budget_seconds). Checked at each stage and
//   each cand boundary. On exceeded: returns the last *fully-completed*
//   stage's ranking. Partial-stage data is discarded.
// - All scenario evaluation uses nested PEG (non-emptier) or
//   endgame_solve(plies, use_heuristics=true) (emptier). No omniscient
//   endgame at the PEG-analysis level.
// ============================================================================

// Hard caps. PEG rejects positions outside these.
#define PEG_MIN_BAG 1
#define PEG_MAX_BAG 4
// = full distribution count - mover rack - board, exclusive cap.
#define PEG_MAX_UNSEEN 11

// Number of halving stages after the greedy seed (Stage 0).
// Stages 1..6 evaluate top 32, 16, 8, 4, 2, 1 respectively.
#define PEG_NUM_STAGES 7

// Stage table — top-K cand count and inner depth per stage. Read-only.
// Stage 0 is greedy (no inner depth); stages 1..6 use the table entries.
extern const int PEG_STAGE_TOP_K[PEG_NUM_STAGES];
extern const int PEG_STAGE_NONEMPTY_INNER_D[PEG_NUM_STAGES];
extern const int PEG_STAGE_EMPTIER_PLIES[PEG_NUM_STAGES];

// ----- Progress callbacks -----------------------------------------------
//
// All callbacks are optional (set to NULL to skip). They are invoked from
// the solver thread that completed the event. Callbacks must be quick and
// thread-safe — the solver may invoke them concurrently from worker
// threads. `user_data` is the value passed in PegArgs.

typedef void (*PegOnStageStart)(int stage_idx, int k_cands, int inner_d,
                                int emptier_plies, void *user_data);

typedef void (*PegOnCandDone)(int stage_idx, int cand_rank, const Move *cand,
                              double win_pct, double mean_spread,
                              int scen_done, void *user_data);

typedef void (*PegOnScenarioDone)(int stage_idx, int cand_rank,
                                  int scenario_idx, int32_t mover_total,
                                  int64_t weight, void *user_data);

// ----- Solver inputs ----------------------------------------------------

typedef struct PegArgs {
  // Required: PEG position to analyze. Must have bag size in [PEG_MIN_BAG,
  // PEG_MAX_BAG]. Caller retains ownership.
  const Game *game;

  // Required: thread control for movegen / endgame coordination.
  ThreadControl *thread_control;

  // Required: number of worker threads (>= 1). The solver parallelizes
  // both across cands and across scenarios within a cand.
  int num_threads;

  // Total wall-clock budget in seconds for the whole peg_solve call.
  // 0 = unbounded (run to last stage). When budget is hit mid-stage, the
  // last *fully-completed* stage's top-K is returned; partial-stage work
  // is discarded for the published ranking.
  double time_budget_seconds;

  // Inner top-K cap: when running nested PEG for a non-emptier cand, only
  // evaluate the inner_top_k highest-equity opp moves per opp_rack. Set
  // to 0 for no cap (evaluate all opp cands). Default: 8.
  int inner_top_k;

  // Scenario stride: weight-stratified sampling. 1 = full enumeration.
  // k > 1 = sample one multiset per k weight-units, scaled accordingly.
  // Set to 0 to use the bag-size default (peg.c picks 1/1/7/TBD for bag
  // sizes 1/2/3/4).
  int scenario_stride;

  // If true, fill PegResult.per_scenario detail for the final stage's
  // top cand. Default false (saves memory).
  bool include_per_scenario;

  // Optional progress callbacks. NULL to skip.
  PegOnStageStart   on_stage_start;
  PegOnCandDone     on_cand_done;
  PegOnScenarioDone on_scenario_done;
  void             *user_data;
} PegArgs;

// ----- Solver outputs ---------------------------------------------------

typedef struct PegRankedCand {
  Move move;          // the cand
  double win_pct;     // 0..1
  double mean_spread; // signed (mover − opp), millipoints / 1000
  int64_t weight_sum; // total scenario weight evaluated
  int n_scenarios;    // distinct multisets evaluated
} PegRankedCand;

typedef struct PegResult {
  // Best move = top of the last fully-completed stage.
  Move best_move;
  double best_win;
  double best_spread;

  // Index of the last stage that completed (0 = greedy only, 6 = final).
  int last_completed_stage;

  // Wall time used (seconds).
  double elapsed_seconds;

  // Top-K cand list from the last completed stage, sorted descending by
  // (win_pct + 1e-4 * mean_spread). Caller owns/frees. NULL on failure.
  PegRankedCand *top_cands;
  int n_top_cands;

  // Optional per-scenario breakdown for top_cands[0]. Only populated when
  // PegArgs.include_per_scenario is set. NULL otherwise.
  struct PegPerScenario *per_scenario;
  int n_per_scenario;
} PegResult;

// Per-scenario detail row.
typedef struct PegPerScenario {
  int scenario_idx;
  char drawn[16];          // mover's drawn tile letters (e.g., "EI")
  char remaining[16];      // bag-remaining letters
  int64_t weight;
  int32_t mover_total;
} PegPerScenario;

// ----- Entry points -----------------------------------------------------

// Solve PEG analysis for args->game. Fills *out. Caller owns out->top_cands
// and out->per_scenario; free via peg_result_destroy. Pushes onto error_stack
// on failure (game out of PEG range, alloc fail, etc.).
void peg_solve(const PegArgs *args, PegResult *out, ErrorStack *error_stack);

// Free PegResult dynamically-allocated members. Idempotent.
void peg_result_destroy(PegResult *r);

#endif  // PEG_H
