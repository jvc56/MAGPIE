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
  // use the full KWG (or any override KWGs set by the caller on the game).
  // Useful when the caller builds pruned KWGs once and reuses them across many
  // endgame solves.
  bool skip_word_pruning;
  // If true, allow the bag to be non-empty when endgame_solve is called.
  bool allow_nonempty_bag;
  // If non-NULL, the solver uses this TT instead of creating/destroying its
  // own. The caller is responsible for the lifetime of the shared TT.
  // tt_fraction_of_mem is ignored when shared_tt is set.
  TranspositionTable *shared_tt;
  // Offset added to worker thread indices. When multiple endgame_solve calls
  // run concurrently, each must use a distinct range to avoid collisions on
  // the global per-thread MoveGen cache.
  int thread_index_offset;
} EndgameArgs;

void endgame_ctx_destroy(EndgameCtx *ctx);
void endgame_solve(EndgameCtx **ctx, const EndgameArgs *endgame_args,
                   EndgameResults *results, ErrorStack *error_stack);
// Single-threaded endgame solve that runs in the calling thread (no
// cpthread_create). Safe for use from concurrent PEG decomp threads
// when each thread uses a distinct thread_index_offset in EndgameArgs.
void endgame_solve_inline(EndgameCtx **ctx, const EndgameArgs *endgame_args,
                          EndgameResults *results);
const TranspositionTable *
endgame_ctx_get_transposition_table(const EndgameCtx *ctx);
void endgame_ctx_get_progress(const EndgameCtx *ctx, int *current_depth,
                              int *root_moves_completed, int *root_moves_total,
                              int *ply2_moves_completed, int *ply2_moves_total);

#endif
