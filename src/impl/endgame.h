#ifndef ENDGAME_H
#define ENDGAME_H

// Note: The endgame solver currently only supports RACK_SIZE <= 7 and
// BOARD_DIM <= 16. The rack-size limitation comes from SmallMove, which
// encodes up to 7 tiles in a 64-bit integer for memory efficiency during
// search. The board-size limitation comes from MoveUndo's 16-bit
// tiles_placed_mask.

#include "../ent/endgame_results.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/kwg.h"
#include "../ent/move.h"
#include "../ent/move_undo.h"
#include "../ent/small_move_arena.h"
#include "../ent/thread_control.h"
#include "../ent/transposition_table.h"

enum {
  DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE = 1024 * 1024,
};

typedef struct EndgameSolver EndgameSolver;
typedef struct EndgameSolverWorker EndgameSolverWorker;

// Callback for per-ply PV reporting during iterative deepening
// Parameters: depth, value (spread delta), pv_line, game,
//             ranked_pvs (top-K PVLines with TT-extended variations),
//             num_ranked_pvs, user_data
typedef void (*EndgamePerPlyCallback)(int depth, int32_t value,
                                      const struct PVLine *pv_line,
                                      const struct Game *game,
                                      const struct PVLine *ranked_pvs,
                                      int num_ranked_pvs, void *user_data);

typedef struct EndgameArgs {
  ThreadControl *thread_control;
  const Game *game;
  double tt_fraction_of_mem;
  int plies;
  int initial_small_move_arena_size;
  int num_threads;
  // Enable stuck-tile heuristics and greedy leaf playout (default: true)
  bool use_heuristics;
  // Number of top moves to return with full PVs (default 1)
  int num_top_moves;
  EndgamePerPlyCallback per_ply_callback;
  void *per_ply_callback_data;
  dual_lexicon_mode_t dual_lexicon_mode;
  // If true, play forced passes without consuming a depth ply (default: false)
  bool forced_pass_bypass;
  // IDS time management (0 = no limit, rely on external timer only):
  // After each completed depth, if elapsed > soft_time_limit, stop.
  // If elapsed < soft_time_limit, estimate next depth time via EBF.
  // If estimated completion > hard_time_limit, stop to bank remaining time.
  double soft_time_limit;
  double hard_time_limit;
  // If true, skip word pruning (KWG build) during reset. Move generation will
  // use the full KWG. Useful when the solver is reused for many positions where
  // rebuilding the pruned KWG per call would be prohibitively expensive.
  bool skip_word_pruning;
  // If true, allow the bag to be non-empty when endgame_solve is called.
  // Used by the PEG solver for pass-candidate scenarios where the bag tile
  // was not drawn by the mover (pass = no draw) and must remain in the bag
  // for the opponent to draw normally.
  bool allow_nonempty_bag;
  // If non-NULL, the solver uses this TT instead of creating/destroying its
  // own.  The caller is responsible for the lifetime of the shared TT.
  // tt_fraction_of_mem is ignored when shared_tt is set.
  TranspositionTable *shared_tt;
  // Offset added to worker thread indices. When multiple endgame_solve calls
  // run concurrently (e.g. from PEG), each must use a distinct range to avoid
  // collisions on the global per-thread MoveGen cache.
  int thread_index_offset;
  // When true, search with α=-1, β=1 to determine win/loss without finding
  // the exact best spread. Much faster due to narrow-window cutoffs.
  bool first_win_optim;
} EndgameArgs;

EndgameSolver *endgame_solver_create(void);
void endgame_solver_reset(EndgameSolver *es, const EndgameArgs *endgame_args);
void endgame_solve(EndgameSolver *solver, const EndgameArgs *endgame_args,
                   EndgameResults *results, ErrorStack *error_stack);
void endgame_solver_destroy(EndgameSolver *es);
const TranspositionTable *
endgame_solver_get_transposition_table(const EndgameSolver *es);
void endgame_solver_get_progress(const EndgameSolver *es, int *current_depth,
                                 int *root_moves_completed,
                                 int *root_moves_total,
                                 int *ply2_moves_completed,
                                 int *ply2_moves_total);

// PEG (pre-endgame) solver interface: exposes worker lifecycle and the greedy
// leaf playout so the PEG solver can reuse the endgame solver infrastructure
// without running the full iterative-deepening loop.
EndgameSolverWorker *endgame_solver_create_worker(EndgameSolver *solver,
                                                  int worker_index,
                                                  uint64_t base_seed);
void endgame_solver_worker_destroy(EndgameSolverWorker *worker);
Game *endgame_solver_worker_get_game(EndgameSolverWorker *worker);

// Greedy playout from the current state of worker->game_copy. Undo slots used:
// worker->move_undos[plies .. plies+depth-1] where plies =
// worker->solver->requested_plies. Cross-sets must be valid before calling.
// Returns score from on_turn_idx's perspective (positive = good for on_turn).
int32_t negamax_greedy_leaf_playout(EndgameSolverWorker *worker,
                                    uint64_t node_key, int on_turn_idx,
                                    int32_t on_turn_spread, PVLine *pv,
                                    float opp_stuck_frac);

// Accessors used by the PEG solver to temporarily adjust solver state.
// requested_plies controls which move_undo slots the greedy playout uses:
// it starts at move_undos[requested_plies] to avoid clobbering parent undo data.
int endgame_solver_worker_get_requested_plies(const EndgameSolverWorker *worker);
void endgame_solver_worker_set_requested_plies(EndgameSolverWorker *worker,
                                               int plies);
MoveUndo *endgame_solver_worker_get_move_undo(EndgameSolverWorker *worker,
                                              int slot);

#endif
