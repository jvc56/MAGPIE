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
  bool enable_pv_display; // Whether to prepare PVLine data for display
                          // (default: false)
  // IDS time management (0 = no limit, rely on external timer only):
  // After each completed depth, if elapsed > soft_time_limit, stop.
  // If elapsed < soft_time_limit, estimate next depth time via EBF.
  // If estimated completion > hard_time_limit, stop to bank remaining time.
  double soft_time_limit;
  double hard_time_limit;
  uint64_t seed;
  // If true, skip word pruning (KWG build) during reset. Move generation will
  // use the full KWG (or any override KWGs set by the caller on the game).
  // Useful when the caller builds pruned KWGs once and reuses them across many
  // endgame solves.
  bool skip_word_pruning;
  // If non-NULL, the solver uses this TT instead of creating/destroying its
  // own. The caller is responsible for the lifetime of the shared TT.
  // tt_fraction_of_mem is ignored when shared_tt is set.
  TranspositionTable *shared_tt;
  // Ceiling on the worker count for dynamic injection. When > num_threads the
  // solve starts with num_threads workers but endgame_add_worker may grow it
  // up to max_workers mid-search (e.g. a pool lending cores as they free up).
  // The workers[]/worker_ids[] arrays are sized to this ceiling so growth
  // never reallocates them out from under the running threads. 0 (or <=
  // num_threads) disables growth — the solve runs with exactly num_threads.
  int max_workers;
  // First-win optimization: search a narrow [-1, +1] window so the solver
  // returns only win/loss/draw rather than exact spread. Faster (more
  // alpha-beta cutoffs) but exact spread is unknown when set.
  bool first_win;
  // Cap on the depth-0 interrupt fallback when first_win is set. The fallback
  // greedy-evaluates root candidates so an interrupted solve still returns a
  // best move; for first_win that sweep (often 1000+ moves at a wide endgame
  // root) is almost pure overhead. 0 = built-in default (12), <0 = skip the
  // sweep entirely, >0 = evaluate at most this many top moves. Ignored unless
  // first_win is set.
  int first_win_fallback_moves;
  // Absolute monotonic-ns deadline (ctimer_monotonic_ns()-compatible). If
  // non-zero, workers bail out mid-search once now > deadline. Lets a
  // caller (e.g. PEG) impose a wall-clock budget that propagates through
  // alpha-beta, not just between IDS depth iterations. 0 = no deadline.
  int64_t external_deadline_ns;
} EndgameArgs;

void pvline_extend_from_tt(PVLine *pv_line, Game *game_copy,
                           TranspositionTable *tt, int solving_player,
                           int max_depth);
// Allocate an empty endgame context (reused across solves). Returns a single
// EndgameCtx; pre-creating it lets a caller hold a stable pointer before any
// concurrent observer (e.g. an injection monitor) reads it.
EndgameCtx *endgame_ctx_create(void);
void endgame_ctx_destroy(EndgameCtx *ctx);
void endgame_solve(EndgameCtx **ctx, const EndgameArgs *endgame_args,
                   EndgameResults *results, ErrorStack *error_stack);
// Inject one additional ABDADA worker into an in-flight solve. Works against
// both entry points: endgame_solve (spawned master) and endgame_solve_inline
// (the calling thread is the master). It may be called from another thread
// (e.g. a pool lending an idle core) or from the solving thread itself — the
// injection test drives it from the master's per_ply_callback. The new worker
// gets the next free ordinal (> 0, never the root master), its own ABDADA
// jitter from that ordinal, a game copy from the (read-only) root inputs, and a
// MoveGen cache slot assigned on demand by get_movegen(); it cooperates with
// the running workers purely through the shared TT and self-exits when the
// search completes. Returns true if a worker was spawned, false if the
// injection window is shut or the max_workers ceiling is reached. Only valid
// against a ctx whose current solve was launched with max_workers enabling
// growth (max_workers > effective thread count after reset normalization).
bool endgame_add_worker(EndgameCtx *ctx);

// Number of worker threads currently live in this solve (master + injected
// helpers). Lets an injection monitor cap total threads near the core count.
int endgame_live_workers(const EndgameCtx *ctx);
// True while the solve is accepting injected workers (window open).
bool endgame_injecting(const EndgameCtx *ctx);
// Monotonic-ns timestamp when the injection window last opened. Lets a monitor
// inject only into endgames that have run long enough to be worth helping
// (skipping the many sub-millisecond leaf solves).
int64_t endgame_window_open_ns(const EndgameCtx *ctx);
// Single-threaded endgame solve that runs in the calling thread (no
// cpthread_create). Safe for use from concurrent PEG decomp threads, which
// cooperate through the per-thread MoveGen pool (get_movegen auto-assigns a
// distinct slot per pthread). `results` is required (must be non-NULL); the
// iterative-deepening loop writes into it on every depth.
void endgame_solve_inline(EndgameCtx **ctx, const EndgameArgs *endgame_args,
                          EndgameResults *results);
const TranspositionTable *
endgame_ctx_get_transposition_table(const EndgameCtx *ctx);
void endgame_ctx_get_progress(const EndgameCtx *ctx, int *current_depth,
                              int *root_moves_completed, int *root_moves_total,
                              int *ply2_moves_completed, int *ply2_moves_total);

#endif
