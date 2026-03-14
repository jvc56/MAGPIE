#ifndef PEG_H
#define PEG_H

#include "../def/game_defs.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/thread_control.h"
#include "endgame.h"

// Maximum number of refinement passes (greedy + up to 15 endgame depths).
enum { PEG_MAX_PASSES = 16 };

// Default pass candidate limits: progressively fewer candidates per deeper
// pass.
enum {
  PEG_DEFAULT_PASS0_LIMIT = 64,
  PEG_DEFAULT_PASS1_LIMIT = 32,
  PEG_DEFAULT_PASS2_LIMIT = 16,
  PEG_DEFAULT_PASS3_LIMIT = 8,
  PEG_DEFAULT_PASS4_LIMIT = 4,
};

// Called after each PEG evaluation pass completes and candidates are sorted.
typedef void (*PegPerPassCallback)(int pass, int num_evaluated,
                                   const SmallMove *top_moves,
                                   const double *top_values,
                                   const double *top_win_pcts, int num_top,
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

  // Candidate limits after each pass:
  //   pass_candidate_limits[0]: top-K to promote from greedy -> 1-ply endgame
  //   pass_candidate_limits[1]: top-K to promote from 1-ply  -> 2-ply endgame
  // num_passes: number of endgame passes (0 = greedy only).
  int pass_candidate_limits[PEG_MAX_PASSES];
  int num_passes;

  // Starting ply offset for endgame passes. Default 0 means passes start at
  // 1-ply. Set to N to skip the first N ply levels (e.g., start_pass=1 with
  // num_passes=1 runs a single 2-ply pass, skipping 1-ply).
  int start_pass;

  // Optional progress callback (NULL to disable).
  PegPerPassCallback per_pass_callback;
  void *per_pass_callback_data;
  // How many top candidates to pass to the callback (0 = default 5).
  int per_pass_num_top;

  // Internal flag: set by peg_eval_pass_recursive to skip the pass candidate
  // in the inner recursive call, preventing infinite mutual recursion.
  // Callers should leave this at 0.
  int skip_pass;

  // If non-NULL, all endgame solves share this TT instead of creating their
  // own.  The caller owns the lifetime.
  TranspositionTable *shared_tt;

  // Base offset for movegen thread indices. Each thread in peg_solve uses
  // thread_index_offset + ti to index into the global cached movegen array.
  // Callers should leave this at 0. Set internally when peg_solve is called
  // recursively (e.g., from pass evaluation) to avoid index collisions.
  int thread_index_offset;
} PegArgs;

typedef struct PegResult {
  SmallMove best_move;
  // Win fraction in [0,1] averaged over all bag-tile scenarios.
  double best_win_pct;
  // Expected spread from mover's perspective, averaged over bag-tile scenarios.
  double best_expected_spread;
  // How many endgame passes completed (0 = greedy only).
  int passes_completed;
  // Candidates remaining after the last completed pass.
  int candidates_remaining;
} PegResult;

typedef struct PegSolver PegSolver;

PegSolver *peg_solver_create(void);
// Solve the pre-endgame position. game must have exactly 1 tile in the bag.
void peg_solve(PegSolver *solver, const PegArgs *args, PegResult *result,
               ErrorStack *error_stack);
void peg_solver_destroy(PegSolver *solver);

#endif
