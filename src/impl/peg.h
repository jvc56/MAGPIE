#ifndef PEG_H
#define PEG_H

#include "../compat/ctime.h"
#include "../def/letter_distribution_defs.h"
#include "../def/peg_defs.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/thread_control.h"
#include "../ent/transposition_table.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// Unified pre-endgame (PEG) solver
//
// Solves positions with PEG_MIN_BAG..PEG_MAX_BAG tiles in the bag; larger
// positions are midgame and are rejected, unless every candidate move is
// guaranteed to leave at most PEG_MAX_BAG tiles in it (an only_moves set, or
// a limit_tiles_left of at most PEG_MAX_BAG, with any protected moves
// leaving that few too), in which case a bag up to RACK_SIZE is also
// accepted: each candidate then hands the opponent a pre-endgame the solver
// handles.
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
// view can just append its one row. When a cutoff stage solves candidates at
// once (see early_cutoff), it fires from pool threads, one call at a time.
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
typedef struct PegCheckpoint PegCheckpoint;

PegCheckpoint *peg_checkpoint_create(void);
void peg_checkpoint_destroy(PegCheckpoint *checkpoint);

typedef struct PegArgs {
  // Required: PEG position to analyze. Must have bag size in [PEG_MIN_BAG,
  // PEG_MAX_BAG], except bag sizes up to RACK_SIZE are also accepted when
  // only_moves is non-empty and every move in it is guaranteed to empty the
  // bag (see peg_move_empties_bag); this exception can only be satisfied via
  // only_moves; the full root move list always includes a pass, which never
  // empties the bag. Caller retains ownership.
  const Game *game;

  // Required: thread control for movegen / endgame coordination.
  ThreadControl *thread_control;

  // Required: number of worker threads (>= 1). The solver parallelizes both
  // across cands and across scenarios within a cand.
  int num_threads;

  // Browser-only aggregate TT budget as a fraction of
  // get_search_memory_budget_base(). Zero retains platform defaults; browser
  // callers supply the selected budget. Ignored on native builds.
  double tt_fraction_of_mem;

  // The endgame depth of an exhaustive solve, whose one stage keeps every
  // candidate (stage_top_k {INT_MAX}): each emptier endgame is searched this
  // many plies deep, judging a line still going there by the leaf estimate. 0
  // (or PEG_EXHAUSTIVE_PLIES or more) searches to the end of the game, so its
  // values are proven. peg_args_fill sets 0; the cascade's stages ignore it.
  int exhaustive_plies;

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
  // 0 (the CLI default) = full enumeration, the same as 1.
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
  // Optional per-bag-size override of the nested stride: element b is the
  // stride an inner peg with a bag of b tiles samples at (element 0 is
  // unused). An element > 0 takes precedence over nested_stride and over the
  // bag-dependent default; NULL / 0 = no override. Lets a caller state every
  // stride rather than leave one to a built-in default that can change between
  // builds (birdtest's contribute requests do this).
  const int *nested_strides_by_bag;
  int nested_n_strides_by_bag; // length of nested_strides_by_bag
  int nested_emptier_ply_cap;
  int nested_max_depth;
  // How a nested player's choice is valued. False (a zero-initialized
  // PegArgs) values the chosen play by its average over every scenario that
  // player could be in, which is not the result on the scenario at hand. True
  // chooses by that average, then plays the choice on the scenario's actual
  // racks and bag and values what follows.
  bool nested_score_actual;
  // With PEG_OPP_PESSIMISTIC and nesting on: at every nested node where the
  // opponent of the root mover is on turn, it tries every reply (or its
  // inner_top_k highest-equity ones) on the scenario, as if it could see every
  // rack and the bag order, and plays the one worst for the mover; under
  // first-win it stops at the first reply that wins for it. False (a
  // zero-initialized PegArgs) uses the pessimistic model only where a rollout
  // is used, so with nesting on only at the greedy seed.
  bool nested_pessimistic;

  // EndgameArgs.disable_best_reply_shortcuts for every endgame solve the PEG
  // runs. False (a zero-initialized PegArgs) leaves the shortcuts on.
  bool disable_best_reply_shortcuts;

  // Solve every endgame leaf, outer and nested, first-win: a [-1, +1] window
  // on the final spread, which answers win, draw or loss and not by how much.
  // Win% is unchanged and the solves are much faster, but the mean spread of a
  // candidate scored past the greedy seed is unknown
  // (PegRankedCand.spread_unknown), and nested values become -1, 0 or +1, so a
  // nested player ranks its plays by wins plus half the draws. False (a
  // zero-initialized PegArgs) computes exact spreads.
  bool first_win;

  // In an exhaustive solve (a single uncapped stage, PegArgs.stage_top_k =
  // {INT_MAX}), with first-win endgames: score candidates separately, and
  // stop solving one, and mark it PegRankedCand.cut, once its losses and
  // draws so far prove it cannot reach the best finished candidate's win%. It
  // is exact because first-win verdicts are exact, so the best move and its
  // win% are those of the uncut solve. Protected moves are never cut. Its
  // scenarios are solved likeliest losses first (by a greedy playout), so a
  // losing candidate is cut sooner. With more than one thread, the seed's
  // best candidate is solved first, every thread on its scenarios, and then
  // the rest at once, one per thread, each against the best finished so far;
  // which candidates are cut, and the bounds they show, can then vary from
  // run to run. False (a zero-initialized PegArgs) solves every candidate in
  // full.
  bool early_cutoff;

  // The tiles on the opponent's rack in `game` are tiles the mover knows it
  // holds: given in the position, or returned to its rack after a
  // challenged-off phony. Every scenario keeps them on its rack, and only the
  // rest of the unseen tiles are dealt to the bag and to the rest of its
  // rack. Only the root solve uses them; a nested player deals from all it
  // cannot see. False (a zero-initialized PegArgs) deals every unseen tile,
  // as when `game` holds the opponent's actual rack (autoplay).
  bool opp_rack_known;

  // Timed-cascade refinements, each false (today's cascade) in a
  // zero-initialized PegArgs:
  // - first_win_intermediate: the halving stages before the last solve their
  //   endgames first-win, as a complete solve does (their cuts depend on win%
  //   almost always), keeping every candidate tied at a cut line; the last
  //   stage keeps a full window, so the answer's spreads are exact.
  // - skip_noncutting_stages: a stage that would keep every live candidate
  //   (fewer of them than its count), other than the last, is skipped: its
  //   ranking decides nothing.
  // - final_stage_cutoff: the last stage scores its candidates separately
  //   and stops solving one once its losses and draws prove it cannot reach
  //   the best finished one at that depth (PegRankedCand.cut), as
  //   early_cutoff does for a complete solve, threads included. Protected
  //   moves are never cut.
  bool first_win_intermediate;
  bool skip_noncutting_stages;
  bool final_stage_cutoff;

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

  // Optional limit on the generated candidates (when n_only_moves is 0): with
  // limit_tiles_left, only moves that leave at most max_tiles_left tiles in
  // the bag (peg_move_bag_after) are candidates, so 0 is every move that
  // empties the bag. A pass leaves the whole bag. False (a zero-initialized
  // PegArgs) generates every move.
  bool limit_tiles_left;
  int max_tiles_left;

  // Optional "only solve" set: a fixed list of root candidate moves to
  // evaluate instead of generating the full move list. When n_only_moves > 0,
  // the solver skips move generation and uses exactly these moves as the
  // stage-0 candidates (still ranked and cascaded normally). Each must be a
  // legal play for the mover on the root board; the pointed-to Move objects
  // must outlive the solve. NULL / 0 = generate all moves (default).
  const Move *const *only_moves;
  int n_only_moves;

  // Optional "protect from pruning" set (like the simmer's snoprune): moves
  // that are always candidates -- added to the set when the limit above leaves
  // them out -- and that must survive every stage's top-K cut even when their
  // win% rank falls below it. Each stage advances its top-K unprotected moves
  // plus every protected move, carrying them to the deepest fidelity. A
  // protected move never takes one of the K slots, so protecting a move that
  // already ranks in the top-K still carries K unprotected moves alongside it.
  // Matched against the candidate set by move-similarity key. The pointed-to
  // Move objects must outlive the solve. NULL / 0 = no protection (default).
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
  // Optional caller-owned endgame transposition table every leaf solve uses,
  // in place of the table peg_solve otherwise allocates (sized from RAM) and
  // shares among its workers on every call. Transposition tables are safe to
  // share between threads, and between solves of different positions.
  // Autoplay passes one table for the whole run, so concurrent games each
  // solving a pre-endgame do not each claim a share of RAM. NULL = allocate
  // one per solve on first use (default).
  TranspositionTable *shared_endgame_tt;
  // Optional caller-owned session checkpoint. Reuses complete candidate
  // evaluations only when position and search settings match. Time budget and
  // thread count may change. NULL keeps the non-interactive path unchanged.
  PegCheckpoint *checkpoint;
  bool resume;
} PegArgs;

// Fills every PegArgs field from an explicit argument, so that adding a field
// to the struct breaks each call site until it is considered. That is the whole
// point of the parameter count: prefer this to assigning the struct directly,
// and add a parameter here rather than a default when a field is added.
// (sim_args_fill deliberately does not go this far; see the note there.) The
// tests build PegArgs literals instead, opting out of that check knowingly.
static inline void peg_args_fill(
    const Game *game, ThreadControl *thread_control, const int num_threads,
    const double time_budget_seconds, const int max_stage,
    const bool greedy_seed_only, const int *stage_top_k, const int num_stages,
    const int inner_top_k, const PegOppModel opp_model,
    const int scenario_stride, const bool nested_enabled,
    const int nested_cand_cap, const int *nested_cand_caps,
    const int nested_n_cand_caps, const int nested_stride,
    const int *nested_strides_by_bag, const int nested_n_strides_by_bag,
    const int nested_emptier_ply_cap, const int nested_max_depth,
    const bool nested_score_actual, const bool nested_pessimistic,
    const bool disable_best_reply_shortcuts, const bool first_win,
    const bool early_cutoff, const bool opp_rack_known,
    const bool first_win_intermediate, const bool skip_noncutting_stages,
    const bool final_stage_cutoff, const MachineLetter *eval_bag_order,
    const int eval_bag_order_len, const bool limit_tiles_left,
    const int max_tiles_left, const Move *const *only_moves,
    const int n_only_moves, const Move *const *protect_moves,
    const int n_protect_moves, const bool include_per_scenario,
    PegOnStageStart on_stage_start, PegOnCandDone on_cand_done,
    PegOnScenarioDone on_scenario_done, void *user_data, PegPoll *poll,
    TranspositionTable *shared_endgame_tt, PegCheckpoint *checkpoint,
    bool resume, PegArgs *peg_args) {
  peg_args->game = game;
  peg_args->thread_control = thread_control;
  peg_args->num_threads = num_threads;
  peg_args->tt_fraction_of_mem = 0;
  peg_args->exhaustive_plies = 0;
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
  peg_args->nested_strides_by_bag = nested_strides_by_bag;
  peg_args->nested_n_strides_by_bag = nested_n_strides_by_bag;
  peg_args->nested_emptier_ply_cap = nested_emptier_ply_cap;
  peg_args->nested_max_depth = nested_max_depth;
  peg_args->nested_score_actual = nested_score_actual;
  peg_args->nested_pessimistic = nested_pessimistic;
  peg_args->disable_best_reply_shortcuts = disable_best_reply_shortcuts;
  peg_args->first_win = first_win;
  peg_args->early_cutoff = early_cutoff;
  peg_args->opp_rack_known = opp_rack_known;
  peg_args->first_win_intermediate = first_win_intermediate;
  peg_args->skip_noncutting_stages = skip_noncutting_stages;
  peg_args->final_stage_cutoff = final_stage_cutoff;
  peg_args->eval_bag_order = eval_bag_order;
  peg_args->eval_bag_order_len = eval_bag_order_len;
  peg_args->limit_tiles_left = limit_tiles_left;
  peg_args->max_tiles_left = max_tiles_left;
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
  peg_args->shared_endgame_tt = shared_endgame_tt;
  peg_args->checkpoint = checkpoint;
  peg_args->resume = resume;
}

// ----- Stage progress snapshot ------------------------------------------

// Per-stage progress snapshot. Completed stages have end_ns != 0; the current
// stage has end_ns == 0. best_win_pct is -1.0 until the first candidate done.
typedef struct PegStageSnapshot {
  int fidelity_plies;  // 0 = greedy; N = N-ply endgame
  int field_size;      // total candidates evaluated in this stage
  int cands_done;      // candidates that have finished so far
  int cands_cut;       // of those, the ones an early cutoff stopped
  int64_t start_ns;    // monotonic ns when this stage started
  int64_t end_ns;      // monotonic ns when this stage ended; 0 = still running
  double best_win_pct; // best mover win% seen; -1.0 if none yet
  // The candidate in progress, when one is evaluated at a time: its scenario
  // jobs (mover draws) solved, and their total. 0/0 otherwise.
  int scenarios_done;
  int scenarios_total;
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
  bool spread_unknown; // scored with first-win endgames (PegArgs.first_win),
                       // so mean_spread means nothing
  bool cut;            // stopped by PegArgs.early_cutoff once it could not
                       // reach the best: win_pct is the most it could have
                       // reached, and the counts are of the orderings solved
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
  bool can_resume;

  // Wall-clock timer: started at the top of peg_solve (is_running == true
  // while solving, false once done). ctimer_elapsed_seconds reads the live
  // elapsed time while running and the final elapsed time after completion.
  Timer timer;

  // Top-K cand list from the last completed stage, sorted by win% (wins plus
  // half the draws), with mean spread breaking only exact ties. Caller
  // owns/frees via peg_result_destroy.
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
  // Endgame continuation for bag emptiers; NULL for non-emptiers/greedy.
  char *pv;
} PegPerScenario;

void peg_scenario_rows_destroy(PegPerScenario *rows, int count);

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

// Where one mover draw of the candidate in progress stands, when candidates are
// solved one at a time (PegStageSnapshot.scenarios_total > 0). A draw is one
// scenario job: the candidate with those tiles drawn, over every ordering of
// the rest of the bag.
typedef enum {
  PEG_DRAW_QUEUED,
  PEG_DRAW_RUNNING,
  PEG_DRAW_DONE,
  // Not solved, or not all its orderings: the candidate was cut, or the
  // deadline or a user interrupt came first.
  PEG_DRAW_STOPPED,
} peg_draw_state_t;

typedef struct PegDrawStatus {
  // The mover's draw, and the tiles left in the bag, whose orderings the draw
  // runs through. A pass or an exchange draws nothing, so its draws differ
  // only in the bag.
  MachineLetter drawn[PEG_SCENARIO_ARRAY_CAP];
  int n_drawn;
  MachineLetter bag[PEG_SCENARIO_ARRAY_CAP];
  int n_bag;
  peg_draw_state_t state;
  int worker_idx;   // the worker solving it, while running
  int64_t start_ns; // monotonic ns when it started; 0 while queued
  int64_t end_ns;   // monotonic ns when it finished; 0 until then
  // Done: the orderings it won and tied of all it had (weight_sum), and its
  // mean spread over them, which means nothing when spread_unknown (a
  // first-win solve).
  int64_t weight_sum;
  int64_t win_count;
  int64_t tie_count;
  double mean_spread;
  bool spread_unknown;
  // Running: the endgame its worker is searching, read when the poll is read.
  // in_endgame is false between endgames, and with one thread, whose
  // endgames take no helpers and so do not open the window this reads.
  bool in_endgame;
  int depth;
  int root_moves_done;
  int root_moves_total;
  uint64_t nodes;
  int helpers; // idle threads lent to its endgame
} PegDrawStatus;

// A candidate solved at once with others, one per thread: the candidate, when
// it started, how many of its draws it has solved of all it has, and the draw
// it is solving (state PEG_DRAW_RUNNING), or none between draws
// (PEG_DRAW_QUEUED).
typedef struct PegCandStatus {
  Move move;
  int64_t start_ns;
  int draws_done;
  int draws_total;
  PegDrawStatus draw;
} PegCandStatus;

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
  bool can_resume;    // stopped with reusable candidate evaluations
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
  // That candidate, which stage_moves holds only while it is among the
  // stage's first PEG_POLL_MAX_ENTRIES.
  Move evaluating_move;
  // How many candidates are being solved at once, one per thread, as a
  // cutoff stage solves all but its first; 0 otherwise. Up to
  // PEG_POLL_MAX_CAND_ROWS of them are listed, longest running first. Filled
  // by peg_poll_read.
  int cands_running;
  int n_cand_rows;
  PegCandStatus cand_rows[PEG_POLL_MAX_CAND_ROWS];
  // The best PEG_POLL_MAX_SOLVED_ROWS solved (uncut) candidates of the current
  // endgame stage, best first, each with its rank among all the stage's
  // finished candidates (1 = best). The top entries can miss them: cut
  // bounds sort above a solved play's lower win%. Filled by peg_poll_read;
  // none in the greedy seed.
  int n_solved_rows;
  PegRankedCand solved_rows[PEG_POLL_MAX_SOLVED_ROWS];
  int solved_ranks[PEG_POLL_MAX_SOLVED_ROWS];
  // The mover draws of that candidate, when candidates are solved one at a
  // time: how many are in each state, the orderings the done ones won and
  // tied of all they had, and up to PEG_POLL_MAX_DRAW_ROWS of the draws
  // themselves -- every running one, then finished ones -- in the order they
  // are solved. Filled by peg_poll_read; all zero otherwise.
  int draws_queued;
  int draws_running;
  int draws_done;
  int draws_stopped;
  int64_t draws_done_weight;
  int64_t draws_done_wins;
  int64_t draws_done_ties;
  int n_draw_rows;
  PegDrawStatus draw_rows[PEG_POLL_MAX_DRAW_ROWS];
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

// The effective PEG bag size for `game`: the real remaining bag tiles plus
// any opponent tiles unknown to the mover (see peg.c for the derivation).
// Exposed so a caller that pre-filters candidate moves (e.g. the pegonly
// "empty" positional value) can match exactly the bag size peg_solve itself
// will compute, rather than the raw (unadjusted) bag_get_letters count.
int peg_compute_bag_size(const Game *game);

// How many of a `bag_size`-tile bag's tiles `move` leaves in it: a tile
// placement draws as many as it plays, while a pass leaves the whole bag, and
// so does an exchange, which returns what it draws.
int peg_move_bag_after(const Move *move, int bag_size);

// True when playing `move` is guaranteed to empty a bag of `bag_size` tiles:
// a tile placement that plays at least bag_size tiles draws the whole bag on
// replenishment. A pass never touches the bag, and an exchange draws
// replacements but then returns the exchanged tiles to the bag, so neither
// ever empties it regardless of tiles played. Exposed so a caller filtering
// candidate moves (e.g. the pegonly "empty" positional value) uses the same
// predicate peg_solve itself checks.
bool peg_move_empties_bag(const Move *move, int bag_size);

// ----- Entry points -----------------------------------------------------

// Solve PEG analysis for args->game. Fills *out. Caller owns out->top_cands
// and out->per_scenario; free via peg_result_destroy. Pushes onto error_stack
// on failure (game out of PEG range, alloc fail, etc.).
void peg_solve(const PegArgs *args, PegResult *out, ErrorStack *error_stack);

// Free PegResult dynamically-allocated members. Idempotent.
void peg_result_destroy(PegResult *r);

#endif // PEG_H
