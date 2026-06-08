#ifndef PEG_H
#define PEG_H

#include "../def/letter_distribution_defs.h"
#include "../def/peg_defs.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// Unified pre-endgame (PEG) solver
//
// Handles 1, 2, 3, and 4 tiles in bag (unseen 8..11). Larger positions are
// rejected — PEG is not for midgame.
//
// Design:
// - Stage 0 (greedy): movegen + sort by static equity descending, then a
//   per-cand greedy d=0 playout over the mover-draw scenarios. Cands processed
//   top-equity-first, so interruption returns the partial top-K by
//   greedy-evaluated-so-far. Seeds the ranking the halving stages refine.
// - Halving stages 1..N: by default 32 -> 16 -> 8 -> 4 -> 2 (five stages).
//   Each stage re-ranks the surviving top-K from the previous stage at one
//   more ply of fidelity (non-emptier inner depth d, emptier endgame plies):
//   stage 1 = d=0/plies=2, ascending. The schedule (and thus N) is the default
//   table length but is fully caller-overridable via PegArgs.stage_top_k, with
//   no fixed upper bound.
//
//   A stage exists to RE-RANK a candidate set at higher fidelity, so its output
//   is only meaningful with >= 2 candidates to compare — there is deliberately
//   no "top-1" stage. The top two candidates of a stage are a parallel pair
//   (the leader has no utility without a runner-up to compare against), not a
//   serial dependency. Past those two, finishing one more candidate is the unit
//   of incremental progress.
// - Time budget (PegArgs.time_budget_seconds). Checked at each stage and each
//   cand boundary. On exceeded: returns the last *fully-completed* stage's
//   ranking; partial-stage data is discarded. A stage is not even started if
//   the remaining budget cannot fit >= 2 candidates — a 1-candidate result is
//   uncomparable, so banking that time for the rest of the game is strictly
//   better.
// - All scenario evaluation uses nested PEG (non-emptier) or
//   endgame_solve(plies, use_heuristics=true) (emptier). No omniscient endgame
//   at the PEG-analysis level.
//
// All tuning is via PegArgs (CLI-driven). No environment-variable knobs.
// ============================================================================

// The bag-size caps (PEG_MIN_BAG, PEG_MAX_BAG) and PEG_MAX_UNSEEN live in
// peg_defs.h. The cascade's default halving schedule (top 32, 16, 8, 4, 2)
// is internal to peg.c; its length sets the default number of halving stages.
// There is no fixed upper bound on stages — a caller may supply a longer
// schedule via PegArgs.stage_top_k.

// ----- Progress callbacks -----------------------------------------------
//
// All callbacks are optional (set to NULL to skip). They are invoked from
// the solver thread that completed the event. Callbacks must be quick and
// thread-safe — the solver may invoke them concurrently from worker
// threads. `user_data` is the value passed in PegArgs.

typedef void (*PegOnStageStart)(int stage_idx, int k_cands, int inner_d,
                                int emptier_plies, void *user_data);

typedef void (*PegOnCandDone)(int stage_idx, int cand_rank, const Move *cand,
                              double win_pct, double mean_spread, int scen_done,
                              void *user_data);

typedef void (*PegOnScenarioDone)(int stage_idx, int cand_rank,
                                  int scenario_idx, int32_t mover_total,
                                  int64_t weight, void *user_data);

// ----- Solver inputs ----------------------------------------------------

// Thread-safe, caller-owned live view of the in-progress ranking (see below).
typedef struct PegPoll PegPoll;

typedef struct PegArgs {
  // Required: PEG position to analyze. Must have bag size in [PEG_MIN_BAG,
  // PEG_MAX_BAG]. Caller retains ownership.
  const Game *game;

  // Required: thread control for movegen / endgame coordination.
  ThreadControl *thread_control;

  // Required: number of worker threads (>= 1). The solver parallelizes both
  // across cands and across scenarios within a cand.
  int num_threads;

  // Total wall-clock budget in seconds for the whole peg_solve call.
  // 0 = unbounded (run to the last stage). When the budget is hit mid-stage,
  // the last *fully-completed* stage's top-K is returned; partial-stage work
  // is discarded for the published ranking.
  double time_budget_seconds;

  // Last halving stage to run, inclusive (1-based; stage 0 is the greedy seed).
  // Lets the caller cap depth below the full cascade. <= 0 is treated as "run
  // all stages".
  int max_stage;

  // Optional per-stage candidate counts for the halving stages (stage 1
  // onward), overriding the built-in PEG_STAGE_TOP_K table. NULL = use the
  // default. When set, num_stages is the array length and defines how many
  // halving stages run; stage 0 (greedy) always evaluates all candidates.
  // Each entry should be >= 2 (a stage re-ranks a set, so a top-1 stage is
  // meaningless) and is normally non-increasing. Powers of two are
  // conventional but not required.
  const int *stage_top_k;
  int num_stages;

  // Inner top-K cap: when running nested PEG for a non-emptier cand, only
  // evaluate the inner_top_k highest-equity opp moves per opp_rack. 0 = no cap
  // (evaluate all opp cands).
  int inner_top_k;

  // Scenario stride: weight-stratified sampling. 1 = full enumeration.
  // k > 1 = sample one multiset per k weight-units, scaled accordingly.
  // 0 = use the bag-size default (the solver picks a sane stride per bag size).
  int scenario_stride;

  // Optional: pin scenario evaluation to a single bag ordering instead of
  // enumerating all scenarios. When eval_bag_order_len > 0, each candidate is
  // evaluated against exactly one scenario: the bag is set to
  // eval_bag_order[0..len-1] in that order (the mover draws the first
  // tiles_played of them; the rest stay in the bag), and the opponent gets the
  // remaining unseen tiles. The length must equal the position's bag size and
  // the tiles must be drawable from the unseen pool, else peg_solve fails with
  // ERROR_STATUS_PEG_INVALID_BAG_ORDER. Only the greedy stage 0 runs — a single
  // pinned scenario has nothing for the halving stages to re-rank. The
  // pointed-to tiles must outlive the solve. NULL / 0 = enumerate (default).
  const MachineLetter *eval_bag_order;
  int eval_bag_order_len;

  // Optional "only solve" set: a fixed list of root candidate moves to
  // evaluate instead of generating the full move list. When n_only_moves > 0,
  // the solver skips move generation and uses exactly these moves as the
  // stage-0 candidates (still ranked and cascaded normally). Each must be a
  // legal play for the mover on the root board; the pointed-to Move objects
  // must outlive the solve. NULL / 0 = generate all moves (default).
  const Move *const *only_moves;
  int n_only_moves;

  // Optional "protect from pruning" set (like the simmer's snoprune): moves
  // that must survive every stage's top-K cut even when their win% rank falls
  // below it. Each stage advances its top-K plus any of these protected moves
  // not already in the top-K, carrying them to the deepest fidelity. Matched
  // against the candidate set by move-similarity key. The pointed-to Move
  // objects must outlive the solve. NULL / 0 = no protection (default).
  const Move *const *protect_moves;
  int n_protect_moves;

  // If true, fill PegResult.per_scenario detail for the final stage's top
  // cand. Default false (saves memory).
  bool include_per_scenario;

  // Optional progress callbacks. NULL to skip.
  PegOnStageStart on_stage_start;
  PegOnCandDone on_cand_done;
  PegOnScenarioDone on_scenario_done;
  void *user_data;

  // Optional live poll. NULL = no polling (zero overhead). When set, peg_solve
  // updates it as candidates complete (stage 0) and at each stage boundary, so
  // a separate thread (e.g. a TUI render loop) can read the current ranking
  // concurrently via peg_poll_read. The caller owns the PegPoll.
  PegPoll *poll;
} PegArgs;

// ----- Solver outputs ---------------------------------------------------

typedef struct PegRankedCand {
  Move move;          // the cand
  double win_pct;     // 0..1
  double mean_spread; // signed (mover - opp), in points
  int64_t weight_sum; // total scenario weight evaluated
  int n_scenarios;    // distinct multisets evaluated
} PegRankedCand;

typedef struct PegResult {
  // Best move = top of the last fully-completed stage.
  Move best_move;
  double best_win;
  double best_spread;

  // Index of the last stage that completed (0 = greedy only; the final halving
  // stage = the deepest stage actually run).
  int last_completed_stage;

  // Wall time used (seconds).
  double elapsed_seconds;

  // Top-K cand list from the last completed stage, sorted descending by
  // (win_pct + 1e-4 * mean_spread). Caller owns/frees via peg_result_destroy.
  // NULL on failure.
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
  char drawn[16];     // mover's drawn tile letters (e.g., "EI")
  char remaining[16]; // bag-remaining letters
  int64_t weight;
  int32_t mover_total;
} PegPerScenario;

// ----- Live polling (optional) ------------------------------------------
//
// Sim-style pollable view: a mutex-guarded leaderboard the solver refreshes
// while it runs, so a render thread can poll the current ranking at any rate
// (e.g. 60fps) without the engine pushing callbacks. The caller creates a
// PegPoll, passes it in PegArgs.poll, polls it via peg_poll_read on another
// thread, and destroys it after peg_solve returns. peg_solve never owns it.
//
// Update cadence: the top-K leaderboard is upserted per candidate during the
// stage-0 greedy seed (so it animates as thousands of candidates resolve), and
// replaced authoritatively at every stage boundary along with the stage /
// fidelity / field-size metadata. The deep halving stages refresh per stage
// (a future refinement could poll per-scenario running win% for finer liveness
// there). `done` flips true when the solve finishes.

enum { PEG_POLL_MAX_ENTRIES = 64 };

typedef struct PegPollSnapshot {
  int stage;          // current stage (0 = greedy seed); -1 before stage 0
  int fidelity_plies; // current stage's fidelity
  int field_size;     // candidates being evaluated in the current stage
  bool done;          // solve finished
  uint64_t version;   // bumped on every update (skip redundant redraws)
  int n_entries;      // populated leaderboard rows below
  PegRankedCand entries[PEG_POLL_MAX_ENTRIES]; // current top-K, sorted desc
} PegPollSnapshot;

PegPoll *peg_poll_create(void);
void peg_poll_destroy(PegPoll *poll);
// Copy a consistent snapshot of the current live view into *out (under lock).
void peg_poll_read(PegPoll *poll, PegPollSnapshot *out);

// ----- Entry points -----------------------------------------------------

// Solve PEG analysis for args->game. Fills *out. Caller owns out->top_cands
// and out->per_scenario; free via peg_result_destroy. Pushes onto error_stack
// on failure (game out of PEG range, alloc fail, etc.).
void peg_solve(const PegArgs *args, PegResult *out, ErrorStack *error_stack);

// Free PegResult dynamically-allocated members. Idempotent.
void peg_result_destroy(PegResult *r);

#endif // PEG_H
