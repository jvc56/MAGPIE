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
#include "../ent/small_move_arena.h"
#include "../ent/thread_control.h"
#include "../ent/transposition_table.h"

enum {
  DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE = 1024 * 1024,
};

typedef struct EndgameSolver EndgameSolver;

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
  // If true, skip using pruned KWGs for cross-set computation (benchmark only)
  bool skip_pruned_cross_sets;
  // If true, play forced passes without consuming a depth ply (default: false)
  bool forced_pass_bypass;
  // Base thread index offset for endgame solver workers (default: 0).
  // Used to avoid thread_index collisions when endgame solver runs inside
  // sim worker threads. Each endgame worker i gets thread_index_base + i.
  int thread_index_base;
} EndgameArgs;

EndgameSolver *endgame_solver_create(void);
void endgame_solve(EndgameSolver *solver, const EndgameArgs *endgame_args,
                   EndgameResults *results, ErrorStack *error_stack);
void endgame_solver_destroy(EndgameSolver *es);

// Lightweight endgame solver for use inside sim plies.
// Pre-allocates all resources once; each solve call reuses them.
typedef struct EndgamePlyCtx EndgamePlyCtx;

EndgamePlyCtx *endgame_ply_ctx_create(const Game *game, int thread_index);
void endgame_ply_ctx_destroy(EndgamePlyCtx *ctx);

// Quick 2-ply endgame solve. Game must have an empty bag.
// Returns the best first move as a SmallMove.
// The result is written into *best_move_out. Returns true on success.
bool endgame_ply_solve_quick(EndgamePlyCtx *ctx, const Game *game, int plies,
                             SmallMove *best_move_out);

#endif
