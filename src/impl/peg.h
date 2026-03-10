#ifndef PEG_H
#define PEG_H

#include "../def/game_defs.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/thread_control.h"
#include "endgame.h"

// Maximum number of stages (stage 0 = greedy, stages 1+ = endgame).
enum { PEG_MAX_STAGES = 16 };

// First-win optimization modes for PEG endgame stages.
//   NEVER:  status quo — full spread search on every stage.
//   PRUNE_ONLY:  first pass with first_win to prune losers, then re-eval
//                survivors with full spread search.
//   WIN_PCT_THEN_SPREAD:  use first_win for win/loss on all stages; only
//                compute spread on the final stage for candidates with tied
//                win% (mode 3a) or for all survivors (mode 3b).
//   WIN_PCT_THEN_SPREAD_ALL:  first_win on all stages except the last, which
//                gets full spread for every surviving candidate.
//   WIN_PCT_ONLY:  first_win on all stages, never compute spread. Fastest.
typedef enum {
  PEG_FIRST_WIN_NEVER = 0,
  PEG_FIRST_WIN_PRUNE_ONLY = 1,
  PEG_FIRST_WIN_WIN_PCT_THEN_SPREAD = 2,
  PEG_FIRST_WIN_WIN_PCT_THEN_SPREAD_ALL = 3,
  PEG_FIRST_WIN_WIN_PCT_ONLY = 4,
} peg_first_win_mode_t;

// Default stage candidate limits: progressively fewer candidates per deeper
// stage. stage_candidate_limits[i] is the top-K to promote from stage i to
// stage i+1.
enum {
  PEG_DEFAULT_STAGE_LIMIT_0 = 64,
  PEG_DEFAULT_STAGE_LIMIT_1 = 32,
  PEG_DEFAULT_STAGE_LIMIT_2 = 16,
  PEG_DEFAULT_STAGE_LIMIT_3 = 8,
  PEG_DEFAULT_STAGE_LIMIT_4 = 4,
};

// Called after each PEG evaluation pass completes and candidates are sorted.
//   pass          - 0 = greedy, 1..N = N-ply endgame
//   num_evaluated - number of candidates evaluated in this pass
//   top_moves     - best candidates after this pass, sorted best-first
//   top_values    - their expected spreads from mover's perspective
//   top_win_pcts  - their win fractions in [0,1] (1=win, 0.5=tie, 0=loss)
//   num_top       - length of the top_* arrays
//   game          - the original input game (for move formatting)
//   elapsed       - seconds elapsed since peg_solve started (cumulative)
//   stage_seconds - seconds spent on this pass only
//   top_pruned    - true if candidate was pruned (win_pct is upper bound)
//   user_data     - caller-supplied pointer
typedef void (*PegPerPassCallback)(int pass, int num_evaluated,
                                   const Move *top_moves,
                                   const double *top_values,
                                   const double *top_win_pcts,
                                   const bool *top_pruned,
                                   const bool *top_spread_known,
                                   int num_top,
                                   const Game *game, double elapsed,
                                   double stage_seconds, void *user_data);

typedef struct PegArgs {
  // Game must have exactly 1 tile in the bag.
  const Game *game;
  ThreadControl *thread_control;
  // Total time budget for peg_solve (seconds). 0 = no limit (run all passes).
  double time_budget_seconds;
  int num_threads;
  // Fraction of memory for the transposition table in endgame passes.
  // Use 0 to disable the TT (faster for very shallow searches).
  double tt_fraction_of_mem;
  dual_lexicon_mode_t dual_lexicon_mode;

  // Number of stages (1 = greedy only, 2 = greedy + 1-ply endgame, etc.).
  // stage_candidate_limits has num_stages-1 entries:
  //   stage_candidate_limits[0]: top-K to promote from greedy -> 1-ply endgame
  //   stage_candidate_limits[1]: top-K to promote from 1-ply  -> 2-ply endgame
  //   ...
  int num_stages;
  int stage_candidate_limits[PEG_MAX_STAGES];

  // Optional progress callback (NULL to disable).
  PegPerPassCallback per_pass_callback;
  void *per_pass_callback_data;
  // How many top candidates to pass to the callback (0 = default 5).
  int per_pass_num_top;

  // When true, skip remaining bag-tile scenarios for a candidate once it
  // cannot possibly reach the K-th best win_pct among completed candidates.
  // Pruned candidates are reported with an upper-bound win_pct.
  bool early_cutoff;

  // For 2-bag non-emptying candidates: number of opp responses to evaluate.
  // multi_tile: plays with >=2 tiles (empty the 1-tile bag). 0 = default 8.
  // one_tile: plays with 1 tile (opp keeps full rack for bingo fishing). 0 = default 8.
  int inner_opp_multi_tile_limit;
  int inner_opp_one_tile_limit;

  // Maximum number of non-emptying (1-tile) candidates to evaluate in Phase 2.
  // Negative or 0 = no limit (evaluate all). Sorted by equity descending.
  int max_non_emptying;

  // When true, skip Phase 1b (full-rack bag-emptying candidates).
  bool skip_phase_1b;

  // When true, do not evaluate the pass candidate at the root level.
  // Useful for 2-in-bag positions where pass is never competitive.
  bool skip_root_pass;

  // Internal flag: set by peg_eval_pass_recursive to skip the pass candidate
  // in the inner recursive call, preventing infinite mutual recursion.
  // Callers should leave this at 0.
  int skip_pass;

  // Base offset for worker thread indices. Ensures that concurrent PEG
  // evaluations (e.g. inner recursive calls from different greedy threads)
  // use non-overlapping thread indices for the global MoveGen cache.
  // Callers should leave this at 0.
  int thread_index_base;

  // If non-NULL, all endgame solves share this TT instead of creating their
  // own.  The caller owns the lifetime.  Internal recursive calls propagate
  // this pointer automatically.
  TranspositionTable *shared_tt;

  // Optional allowlist of candidate move strings (e.g. {"10I X(I)", "7L
  // S(NO)T"}).  When non-NULL, only candidates whose formatted move string
  // matches an entry are kept; all others are discarded before evaluation.
  // Useful for fast, targeted tests of specific plays.
  const char **candidate_allowlist;
  int candidate_allowlist_count;

  // First-win optimization mode for endgame stages (default: NEVER).
  // When non-NEVER, some or all endgame stages use α=-1,β=1 narrow-window
  // search for fast win/loss detection instead of full-spread search.
  peg_first_win_mode_t first_win_mode;
  // For WIN_PCT_THEN_SPREAD: when true (mode 3b), compute spread for all
  // final-stage survivors, not just those tied on win%. Default false (3a).
  bool first_win_spread_all_final;
} PegArgs;

typedef struct PegResult {
  Move best_move;
  // Win fraction in [0,1] averaged over all bag-tile scenarios (primary sort).
  double best_win_pct;
  // Expected spread from mover's perspective, averaged over bag-tile scenarios.
  double best_expected_spread;
  // How many stages completed (1 = greedy only, 2 = greedy + 1-ply, etc.).
  int stages_completed;
  // Candidates remaining after the last completed pass.
  int candidates_remaining;
  // False when the best_expected_spread is unknown (first_win modes that skip
  // spread computation). Spread is only meaningful when this is true.
  bool spread_known;
} PegResult;

typedef struct PegSolver PegSolver;

PegSolver *peg_solver_create(void);
// Solve the pre-endgame position. game must have exactly 1 tile in the bag.
void peg_solve(PegSolver *solver, const PegArgs *args, PegResult *result,
               ErrorStack *error_stack);
void peg_solver_destroy(PegSolver *solver);

#endif
