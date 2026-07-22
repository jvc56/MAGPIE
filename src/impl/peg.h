#ifndef PEG_H
#define PEG_H

#include "../compat/ctime.h"
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
// Solves positions with PEG_MIN_BAG..PEG_MAX_BAG tiles in the bag; larger
// positions are midgame and are rejected.
//
// The solver ranks candidate moves by their win% (and spread) over every way
// the bag could be drawn, in a cascade of progressively-deeper stages:
//
// - Stage 0 (greedy seed): generate moves, sort by static equity, then score
//   each candidate with a greedy playout over its mover-draw scenarios.
//   Candidates are evaluated top-equity-first, so an interrupted stage 0 still
//   returns a sensible partial top-K. This seeds the ranking the halving stages
//   refine.
// - Halving stages 1..N: each re-ranks the surviving top-K from the previous
//   stage at one more ply of fidelity (a deeper endgame_solve on the emptier
//   scenarios), then cuts to a smaller K. The default schedule is
//   32 -> 16 -> 8 -> 4 -> 2; a caller may override it (and thus N) via
//   PegArgs.stage_top_k, with no fixed upper bound. A stage re-ranks a set, so
//   it is only meaningful with >= 2 candidates: there is deliberately no
//   "top-1" stage.
// - Time budget (PegArgs.time_budget_seconds), checked at each stage and
//   candidate boundary. When exceeded, the last *fully-completed* stage's
//   ranking is returned and partial-stage work is discarded. A stage is not
//   started unless the remaining budget can fit >= 2 candidates, since a
//   1-candidate result is uncomparable and banking the time is strictly better.
//
// Scenario leaves: a non-emptier scenario (the opponent still has tiles to
// draw) is scored by a playout to game end under the chosen opponent model
// (see PegOppModel); an emptier scenario (the bag is empty after the candidate)
// is scored by an exact endgame_solve. There is no omniscient endgame at the
// PEG-analysis level.
//
// All tuning is via PegArgs.
// ============================================================================

// The bag-size caps (PEG_MIN_BAG, PEG_MAX_BAG) and PEG_MAX_UNSEEN live in
// peg_defs.h. The cascade's default halving schedule (top 32, 16, 8, 4, 2)
// is internal to peg.c; its length sets the default number of halving stages.
// There is no fixed upper bound on stages — a caller may supply a longer
// schedule via PegArgs.stage_top_k.

// How the solver models the opponent when scoring a candidate's scenarios.
// Both models solve emptier (post-cand bag-empty) scenarios with the same exact
// endgame — in a zero-sum endgame an optimal opponent already plays the worst
// line for the mover — so they differ only on non-emptier scenarios, where the
// opponent still has tiles to draw:
//   PEG_OPP_RATIONAL    — the opponent plays its highest-equity reply (a
//                         self-interested, "realistic" opponent). Default.
//   PEG_OPP_PESSIMISTIC — the opponent plays the reply that minimizes the
//                         mover's result (worst-case / guaranteed-win
//                         analysis). The mover's spread under this model is
//                         always <= the rational model's.
typedef enum {
  PEG_OPP_RATIONAL = 0,
  PEG_OPP_PESSIMISTIC,
} PegOppModel;

// ----- Progress callbacks -----------------------------------------------
//
// All callbacks are optional (set to NULL to skip). They run on whichever
// solver thread completed the event, possibly several worker threads at once,
// so they must be quick and thread-safe. `user_data` is the value passed in
// PegArgs. on_stage_start reports the stage's fidelity as inner_d (the
// non-emptier playout depth, currently always 0) and emptier_plies (the
// emptier-scenario endgame ply count for the stage).

typedef void (*PegOnStageStart)(int stage_idx, int k_cands, int inner_d,
                                int emptier_plies, void *user_data);

// Fired when a candidate finishes. completed_ns is its absolute monotonic-clock
// completion timestamp (ctimer_monotonic_ns-compatible), including in the
// scenario-parallel path where callbacks are delivered after the stage's work
// barrier. reordered is true when the candidate slotted in above the bottom of
// the live ranking (so the displayed order changed and the whole list should be
// redrawn); false when it sorted to the bottom (the new worst), so a streaming
// view can just append its one row.
typedef void (*PegOnCandDone)(int stage_idx, int cand_rank, const Move *cand,
                              double win_pct, double mean_spread, int scen_done,
                              int64_t completed_ns, bool reordered,
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

  // Run only the greedy seed (stage 0): rank the full candidate field by the
  // greedy-playout win% and skip the halving stages' exact endgame refinement.
  // A fast, bounded, deterministic evaluation -- full scenario enumeration and
  // a deterministic playout, with no open-ended deep endgame solves -- at the
  // cost of the endgame-exact fidelity the halving stages add. Overrides
  // max_stage and stage_top_k. Default false.
  bool greedy_seed_only;

  // Optional per-stage candidate counts for the halving stages (stage 1
  // onward), overriding the built-in default schedule. NULL = use the default.
  // When set, num_stages is the array length and defines how many halving
  // stages run; stage 0 (greedy) always evaluates all candidates. Each entry
  // should be >= 2 (a stage re-ranks a set, so a top-1 stage is meaningless)
  // and is normally non-increasing. Powers of two are conventional but not
  // required.
  const int *stage_top_k;
  int num_stages;

  // Pessimistic-opponent cap: in the PEG_OPP_PESSIMISTIC playout, weigh only
  // the inner_top_k highest-equity opponent replies at each opponent turn.
  // 0 = weigh every reply (the unbounded worst case). No effect under
  // PEG_OPP_RATIONAL.
  int inner_top_k;

  // Opponent model for scoring scenarios (see PegOppModel). Defaults to
  // PEG_OPP_RATIONAL (the zero value).
  PegOppModel opp_model;

  // Scenario stride: weight-stratified sampling. 1 = full enumeration.
  // k > 1 = sample one multiset per k weight-units, scaled accordingly.
  // 0 = use the bag-size default (the solver picks a sane stride per bag size).
  int scenario_stride;

  // Nested pre-endgame lookahead for NON-EMPTIER leaves. When nested_enabled,
  // a leaf that still has bag tiles is evaluated by recursively solving the
  // opponent's sub-pre-endgame (alternating sides down to emptier endgames)
  // instead of by a single greedy/pessimistic rollout. Its lookahead horizon
  // is the outer stage's fidelity_plies, so non-emptier candidates gain depth
  // as the cascade deepens, matching the emptiers' move-count horizon.
  // When off, such a leaf takes the single greedy/pessimistic rollout instead.
  // A zero-initialized PegArgs leaves this off; the CLI turns it on by default
  // (-pegnested).
  bool nested_enabled;
  // Cost knobs for the nested recursion (the outer solve is unaffected):
  //   nested_cand_cap  : max candidates considered per nested level (0 = all).
  //   nested_cand_caps : optional PER-LEVEL cap sequence (e.g. {8,4,2}) that
  //                      overrides nested_cand_cap; level L uses caps[L], or
  //                      the last entry for deeper levels. NULL = use the flat
  //                      cap.
  //   nested_stride    : scenario sampling stride inside nested levels.
  //                      0 = bag-size default (2-peg 1, 3-peg 5, 4-peg 7);
  //                      >= 1 = that explicit stride.
  //   nested_emptier_ply_cap : endgame depth at a nested emptier (bag-empty)
  //                            leaf. > 0 = solve to exactly that many plies (a
  //                            FIXED depth, not capped down to the lookahead);
  //                            0 = tie the depth to the remaining lookahead, so
  //                            deeper nested emptiers get fewer plies.
  //   nested_max_depth : how many nested pegs deep before a non-emptier leaf
  //                      falls to a greedy rollout. 0 or unset = 1 (the
  //                      default; deeper recursion does not improve the
  //                      decision). A value >= the bag size nests "until
  //                      empty": the recursion bottoms out at the exact endgame
  //                      when the bag empties, so it is bounded by bag size
  //                      however large the cap is.
  // Exhaustive nesting = cand_cap 0, stride 1, ply_cap 0, max_depth >= bag size
  // with a large fidelity: a genuinely game-theoretic solve under the unseen
  // model.
  int nested_cand_cap;
  const int *nested_cand_caps; // per-level cap sequence; NULL = use flat cap
  int nested_n_cand_caps;      // length of nested_cand_caps
  int nested_stride;
  int nested_emptier_ply_cap;
  int nested_max_depth;

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

// Fills every PegArgs field from an explicit argument, so that adding a field
// to the struct breaks each call site until it is considered. That is the whole
// point of the parameter count: prefer this to assigning the struct directly,
// and add a parameter here rather than a default when a field is added.
// (sim_args_fill deliberately does not go this far; see the note there.) The
// tests build PegArgs literals instead, opting out of that check knowingly.
static inline void
peg_args_fill(const Game *game, ThreadControl *thread_control,
              const int num_threads, const double time_budget_seconds,
              const int max_stage, const bool greedy_seed_only,
              const int *stage_top_k, const int num_stages,
              const int inner_top_k, const PegOppModel opp_model,
              const int scenario_stride, const bool nested_enabled,
              const int nested_cand_cap, const int *nested_cand_caps,
              const int nested_n_cand_caps, const int nested_stride,
              const int nested_emptier_ply_cap, const int nested_max_depth,
              const MachineLetter *eval_bag_order, const int eval_bag_order_len,
              const Move *const *only_moves, const int n_only_moves,
              const Move *const *protect_moves, const int n_protect_moves,
              const bool include_per_scenario, PegOnStageStart on_stage_start,
              PegOnCandDone on_cand_done, PegOnScenarioDone on_scenario_done,
              void *user_data, PegPoll *poll, PegArgs *peg_args) {
  peg_args->game = game;
  peg_args->thread_control = thread_control;
  peg_args->num_threads = num_threads;
  peg_args->time_budget_seconds = time_budget_seconds;
  peg_args->max_stage = max_stage;
  peg_args->greedy_seed_only = greedy_seed_only;
  peg_args->stage_top_k = stage_top_k;
  peg_args->num_stages = num_stages;
  peg_args->inner_top_k = inner_top_k;
  peg_args->opp_model = opp_model;
  peg_args->scenario_stride = scenario_stride;
  peg_args->nested_enabled = nested_enabled;
  peg_args->nested_cand_cap = nested_cand_cap;
  peg_args->nested_cand_caps = nested_cand_caps;
  peg_args->nested_n_cand_caps = nested_n_cand_caps;
  peg_args->nested_stride = nested_stride;
  peg_args->nested_emptier_ply_cap = nested_emptier_ply_cap;
  peg_args->nested_max_depth = nested_max_depth;
  peg_args->eval_bag_order = eval_bag_order;
  peg_args->eval_bag_order_len = eval_bag_order_len;
  peg_args->only_moves = only_moves;
  peg_args->n_only_moves = n_only_moves;
  peg_args->protect_moves = protect_moves;
  peg_args->n_protect_moves = n_protect_moves;
  peg_args->include_per_scenario = include_per_scenario;
  peg_args->on_stage_start = on_stage_start;
  peg_args->on_cand_done = on_cand_done;
  peg_args->on_scenario_done = on_scenario_done;
  peg_args->user_data = user_data;
  peg_args->poll = poll;
}

// ----- Stage progress snapshot ------------------------------------------

// Per-stage progress snapshot. Completed stages have end_ns != 0; the current
// stage has end_ns == 0. best_win_pct is -1.0 until the first candidate done.
typedef struct PegStageSnapshot {
  int fidelity_plies;  // 0 = greedy; N = N-ply endgame
  int field_size;      // total candidates evaluated in this stage
  int cands_done;      // candidates that have finished so far
  int64_t start_ns;    // monotonic ns when this stage started
  int64_t end_ns;      // monotonic ns when this stage ended; 0 = still running
  double best_win_pct; // best mover win% seen; -1.0 if none yet
} PegStageSnapshot;

// ----- Solver outputs ---------------------------------------------------

typedef struct PegRankedCand {
  Move move;           // the cand
  double win_pct;      // 0..1
  double mean_spread;  // signed (mover - opp), in points
  int64_t weight_sum;  // labeled ordered-draw count = perm(unseen, bag_size),
                       // the "weighted orderings" denominator (constant across
                       // all plays in a position)
  int64_t win_count;   // labeled bag-orderings won (leaf value > 0)
  int64_t tie_count;   // labeled bag-orderings tied (leaf value == 0); losses
                       // are weight_sum - win_count - tie_count
  int n_scenarios;     // distinct (multiset, bag-ordering) scenarios evaluated
  double eval_seconds; // wall-clock time to score this cand at its deepest
                       // stage (live/CLI solves only; 0 otherwise)
} PegRankedCand;

typedef struct PegResult {
  // Best move = top of the last fully-completed stage.
  Move best_move;
  double best_win;
  double best_spread;

  // Index of the deepest stage reached (0 = greedy only; the final halving
  // stage = the deepest stage actually run). -1 while running or uninitialized.
  int last_completed_stage;

  // True when the deepest stage was cut off by the budget/interrupt after
  // scoring only some of its candidates (a partial tier), so it was reached but
  // not completed. False when every stage shown ran to completion.
  bool last_stage_partial;

  // Wall-clock timer: started at the top of peg_solve (is_running == true
  // while solving, false once done). ctimer_elapsed_seconds reads the live
  // elapsed time while running and the final elapsed time after completion.
  Timer timer;

  // Top-K cand list from the last completed stage, sorted descending by
  // (win_pct + 1e-4 * mean_spread). Caller owns/frees via peg_result_destroy.
  // NULL on failure.
  PegRankedCand *top_cands;
  int n_top_cands;

  // Graded final ranking: every candidate that entered a halving stage, each
  // tagged with the deepest endgame fidelity (ply count) it reached, captured
  // shallowest-tier first (graded_fidelity[i] is the ply count for
  // graded_cands[i]). A renderer can group by fidelity, showing the deepest
  // tier first with the rank continuing across tiers. Caller owns/frees via
  // peg_result_destroy. n_graded == 0 when no halving stage completed (then the
  // renderer falls back to the flat top_cands list).
  PegRankedCand *graded_cands;
  int *graded_fidelity;
  int n_graded;

  // Optional per-scenario breakdown for top_cands[0]. Only populated when
  // PegArgs.include_per_scenario is set. NULL otherwise.
  struct PegPerScenario *per_scenario;
  int n_per_scenario;

  // Optional per-candidate per-ordering outcomes, captured inline during the
  // halving stages (no re-evaluation) when PegArgs.include_per_scenario is set.
  // One entry per candidate that reached a halving stage, each holding that
  // candidate's per-ordering W/L/T rows at the deepest fidelity it was scored
  // at. Looked up by move at render time. NULL / 0 otherwise. Caller owns/frees
  // via peg_result_destroy.
  struct PegCandOutcomes *cand_outcomes;
  int n_cand_outcomes;

  // Per-stage progress history, populated from the poll at the end of
  // peg_solve. Index i = stage i; n_stage_history grows as stages complete.
  int n_stage_history;
  PegStageSnapshot stage_history[PEG_POLL_MAX_STAGES];
} PegResult;

// Per-scenario detail row.
typedef struct PegPerScenario {
  int scenario_idx;
  char drawn[16];     // mover's drawn tile letters (e.g., "EI")
  char remaining[16]; // bag-remaining letters
  int64_t weight;
  int32_t mover_total;
} PegPerScenario;

// Per-candidate per-ordering outcomes (see PegResult.cand_outcomes). rows is a
// PegPerScenario list for the single candidate `move`, captured at `fidelity`
// plies (0 = greedy; N = N-ply endgame). Caller owns rows via
// peg_result_destroy.
typedef struct PegCandOutcomes {
  Move move;
  PegPerScenario *rows;
  int n_rows;
  int fidelity;
} PegCandOutcomes;

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
// fidelity / field-size metadata. The deep halving stages track per-candidate
// completion via cands_done in the stage history. `done` flips true when the
// solve finishes.

// Compact per-candidate record kept for each completed non-greedy stage so the
// status display can show one time column per stage.
typedef struct {
  Move move;
  double eval_seconds;
} PegHistoryCand;

typedef struct PegPollSnapshot {
  int stage;          // current stage (0 = greedy seed); -1 before stage 0
  int fidelity_plies; // current stage's fidelity
  int field_size;     // candidates being evaluated in the current stage
  bool done;          // solve finished
  uint64_t version;   // bumped on every update (skip redundant redraws)
  int n_entries;      // populated leaderboard rows below
  PegRankedCand entries[PEG_POLL_MAX_ENTRIES]; // current top-K, sorted desc
  // Moves of every candidate that will be scored in the current stage, recorded
  // when the stage begins. Lets a live renderer size the move column to the
  // whole stage up front so it doesn't grow as candidates finish.
  int n_stage_moves;
  Move stage_moves[PEG_POLL_MAX_ENTRIES];
  // Per-stage history: index i = stage i. Grows as stages start. The last
  // entry is the current (possibly still-running) stage.
  int n_stage_history;
  PegStageSnapshot stage_history[PEG_POLL_MAX_STAGES];
  // Snapshot of the previous completed stage's ranking, saved when a new
  // halving stage begins (via peg_poll_clear_entries). Lets status_peg build a
  // cross-depth merged display that always shows the best available result for
  // every candidate, even when the new stage has not yet finished any of them.
  int n_baseline_entries;
  int baseline_fidelity;
  PegRankedCand baseline_entries[PEG_POLL_MAX_ENTRIES];
  // Index into stage_moves[] of the candidate currently being evaluated at the
  // current depth; -1 when none is in flight. eval_start_ns is the monotonic
  // timestamp when that evaluation began, used by the status display to show a
  // live elapsed time and a '*' marker on the depth label.
  int currently_evaluating_move_idx;
  int64_t eval_start_ns;
  // Per-candidate timing history for completed non-greedy stages, accumulated
  // by peg_poll_clear_entries. Greedy (fidelity 0) is excluded — it is always
  // instant and the user does not need a greedy time column. Each slot records
  // the move and eval_seconds for every candidate that completed in that stage.
  // Matched at render time by move identity.
  int n_history_stages;
  int history_fidelities[PEG_POLL_MAX_HISTORY_STAGES];
  int history_n_cands[PEG_POLL_MAX_HISTORY_STAGES];
  PegHistoryCand history_cands[PEG_POLL_MAX_HISTORY_STAGES]
                              [PEG_POLL_MAX_ENTRIES];
} PegPollSnapshot;

PegPoll *peg_poll_create(void);
void peg_poll_destroy(PegPoll *poll);
// Clear the live view back to its just-created state. Call before reusing one
// poll across multiple peg_solve calls so per-solve progress (stage history,
// cands_done) does not accumulate from prior solves.
void peg_poll_reset(PegPoll *poll);
// Copy a consistent snapshot of the current live view into *out (under lock).
void peg_poll_read(PegPoll *poll, PegPollSnapshot *out);

// Replace the poll's live per-candidate outcomes with a deep copy of src[0..n)
// (under lock). Call at each stage boundary so a status poll can render the
// per-multiset W/L/T column; the values change as candidates deepen. src ==
// NULL clears them.
void peg_poll_set_outcomes(PegPoll *poll, const PegCandOutcomes *src, int n);
// Deep-copy the poll's current outcomes into a fresh array (caller owns; free
// with peg_cand_outcomes_destroy_array). *out is NULL when there are none.
void peg_poll_copy_outcomes(PegPoll *poll, PegCandOutcomes **out, int *n_out);
// Free an array of PegCandOutcomes (each rows[] then the array).
void peg_cand_outcomes_destroy_array(PegCandOutcomes *arr, int n);

// ----- Entry points -----------------------------------------------------

// Solve PEG analysis for args->game. Fills *out. Caller owns out->top_cands
// and out->per_scenario; free via peg_result_destroy. Pushes onto error_stack
// on failure (game out of PEG range, alloc fail, etc.).
void peg_solve(const PegArgs *args, PegResult *out, ErrorStack *error_stack);

// Free PegResult dynamically-allocated members. Idempotent.
void peg_result_destroy(PegResult *r);

#endif // PEG_H
