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
  MAX_ENDGAME_DISPLAY_PVS = 100,
};

typedef struct EndgameCtx EndgameCtx;

// Callback for per-ply PV reporting during iterative deepening
// Parameters: depth, value (spread delta), pv_line, game,
//             ranked_pvs (top-K PVLines with TT-extended variations),
//             num_ranked_pvs, user_data
typedef void (*EndgamePerPlyCallback)(int depth, int32_t value,
                                      const struct PVLine *pv_line,
                                      const struct Game *game,
                                      const struct PVLine *ranked_pvs,
                                      int num_ranked_pvs, void *user_data);

// Callback fired once before iterative deepening begins. Provides the
// d=0 root-move list (sorted descending by static estimate from
// assign_estimates_and_sort), letting the caller render an initial
// leaderboard before any depth completes. Fires from worker thread 0
// after its initial-move generation finishes (sub-ms after
// endgame_solve start) and before any negamax call. The polled
// atomics (endgame_ctx_get_progress) are guaranteed to show
// root_moves_total == initial_move_count and root_moves_completed == 0
// at the moment this callback fires; current_depth is still 0.
//
// game is the worker's game copy (already in endgame_solving_mode);
// initial_moves points into the worker's small_move_arena and is
// valid only for the duration of the callback. Copy out anything
// needed past callback return.
typedef void (*EndgameBeforeSearchCallback)(
    const struct Game *game, const struct SmallMove *initial_moves,
    int initial_move_count, int initial_spread, int solving_player,
    void *user_data);

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
  EndgameBeforeSearchCallback before_search_callback;
  void *before_search_callback_data;
  dual_lexicon_mode_t dual_lexicon_mode;
  // If true, play forced passes without consuming a depth ply (default: false)
  bool forced_pass_bypass;
  // If true, run iterative deepening on thread 0 (depth 1, then 2, ..., up
  // to plies). If false, thread 0 jumps directly to plies (other threads
  // still use depth jitter regardless). Callers should normally set this to
  // true; the EndgameArgs zero-initializer leaves it false intentionally so
  // designated initializers don't silently enable an optimization the test
  // wasn't designed for.
  bool enable_iterative_deepening;
  bool enable_pv_display; // Whether to prepare PVLine data for display
                          // (default: false)
  // IDS time management (0 = no limit, rely on external timer only):
  // After each completed depth, if elapsed > soft_time_limit, stop.
  // If elapsed < soft_time_limit, estimate next depth time via EBF.
  // If estimated completion > hard_time_limit, stop to bank remaining time.
  double soft_time_limit;
  double hard_time_limit;
  uint64_t seed;
} EndgameArgs;

// Selects the movegen cache slot to avoid races between concurrent callers.
// RESULT_DISPLAY (slot 0) and SOLVER (slot 1) can run simultaneously during
// a solve; WORKER threads use slot thread_index + 2.
typedef enum {
  ENDGAME_MOVEGEN_RESULT_DISPLAY,
  ENDGAME_MOVEGEN_SOLVER,
  ENDGAME_MOVEGEN_WORKER,
} endgame_movegen_caller_t;

void pvline_extend_from_tt(PVLine *pv_line, Game *game_copy,
                           TranspositionTable *tt, int solving_player,
                           int max_depth, int thread_index,
                           endgame_movegen_caller_t caller);
void endgame_ctx_destroy(EndgameCtx *ctx);
void endgame_solve(EndgameCtx **ctx, const EndgameArgs *endgame_args,
                   EndgameResults *results, ErrorStack *error_stack);
const TranspositionTable *
endgame_ctx_get_transposition_table(const EndgameCtx *ctx);
void endgame_ctx_get_progress(const EndgameCtx *ctx, int *current_depth,
                              int *root_moves_completed, int *root_moves_total,
                              int *ply2_moves_completed, int *ply2_moves_total);

#endif
