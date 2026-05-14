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

// Callback fired each time a root move completes its negamax evaluation
// at the current iterative-deepening depth, from worker thread 0 only.
// Lets clients re-rank the leaderboard live (per-root resolution rather
// than per-completed-depth) and watch values swing during long depths.
//
// Parameters:
//   depth      - the IDS depth this completion is at (>= 1)
//   root_index - index of the root move within the (currently sorted)
//                root_moves array, 0..root_moves_total-1
//   move       - the root move that just completed (caller must copy if
//                it needs the value past callback return)
//   value      - the negamax value of this root, in spread units
//                (already adjusted by initial_spread; same sign convention
//                as PVLine.score)
//   user_data  - opaque
//
// Fires from inside abdada_negamax. Multiple per-root callbacks may
// happen back-to-back at the same depth as roots complete in scan order.
// Implementations must be thread-safe even though only thread 0 calls.
typedef void (*EndgamePerRootMoveCallback)(int depth, int root_index,
                                           const struct SmallMove *move,
                                           int32_t value, void *user_data);

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
  EndgamePerRootMoveCallback per_root_move_callback;
  void *per_root_move_callback_data;
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
// Reset (zero) the transposition table contents but keep the allocation.
// Useful between unrelated solves on the same ctx when the caller wants
// each solve to start from a cold cache without paying the realloc cost
// (the cost is the same — a memset of the full TT — but no malloc/free).
// No-op if the ctx has no TT (tt_fraction_of_mem == 0).
void endgame_ctx_clear_transposition_table(EndgameCtx *ctx);
void endgame_ctx_get_progress(const EndgameCtx *ctx, int *current_depth,
                              int *root_moves_completed, int *root_moves_total,
                              int *ply2_moves_completed, int *ply2_moves_total);

// Sum of nodes searched across all worker threads, lagging by up to
// DEPTH_DEADLINE_CHECK_INTERVAL nodes per thread (the period at which
// each worker flushes its non-atomic local counter to the shared
// atomic). Cheap to call from any thread; intended for "is the engine
// alive / nodes per second" UI heartbeats during long single-root
// subtree evaluations.
uint64_t endgame_ctx_get_nodes_searched(const EndgameCtx *ctx);

// Snapshot of the line currently being explored by worker thread
// `thread_index`. Writes up to `max_len` `tiny_move` entries into
// `out_line` and returns the number written (0..max_len). The line
// reads thread-local state without locking; the reader uses
// release/acquire on the length so the prefix length is always
// consistent, but individual entries past the length may still be
// torn if the worker is concurrently swapping into a sibling subtree.
// For display only; never used to drive search decisions.
int endgame_ctx_get_current_line(const EndgameCtx *ctx, int thread_index,
                                 uint64_t *out_line, int max_len);

// Live ETA data for callers that want to render a wall-clock progress
// bar for the in-flight IDS depth.  All values are CLOCK_MONOTONIC ns.
//
//   *current_depth_started_ns  - timestamp when the engine entered the
//                                current IDS depth. 0 if no depth has
//                                begun yet (callable before search).
//   *prev_completed_depth_ns   - duration of the most recently completed
//                                IDS depth. 0 if no depth has completed.
//   *prev_prev_completed_depth_ns - duration of the depth before that
//                                (so callers can compute a 2-ply EBF
//                                = prev / prev_prev, per-ply EBF
//                                = sqrt of that).
//
// Suggested use: predicted_total_for_current_depth =
// prev_completed_depth_ns * sqrt(prev / prev_prev). Fraction done =
// (now - current_depth_started_ns) / predicted_total. Time-based
// rather than node-based to avoid TT cache effects (an engine that
// hits a hot TT runs faster per-node but doesn't get more wall-clock
// done — the node count would mislead).
void endgame_ctx_get_eta_data(const EndgameCtx *ctx,
                              int64_t *current_depth_started_ns,
                              int64_t *prev_completed_depth_ns,
                              int64_t *prev_prev_completed_depth_ns);

// Convenience: returns an estimated 0..1 fraction of the current IDS
// depth's wall-clock done. Returns -1.0 if no estimate is possible
// (first depth, or no depth has begun yet). Capped at 1.0 if the
// search has overshot the prediction. Uses 2-ply EBF when available,
// falls back to a fixed assumption otherwise.
double endgame_ctx_get_current_depth_eta_fraction(const EndgameCtx *ctx);

// Snapshot of the live best PV from worker `thread_index`. Updated by
// the engine each time best_value improves at the root (so during a
// long depth the reader can see the engine's best line refine as
// successive root moves get evaluated).
//
// Writes up to `max_len` `tiny_move` entries into `out_moves` and
// returns the number written (0..max_len). `*out_value` is set to
// the spread-adjusted PV value (same sign convention as
// PVLine.score).
//
// Uses a seqlock so the moves array, length, and value all come
// from the same writer update — never from a half-overwritten
// state. If a consistent snapshot can't be obtained after a few
// retries (writer continuously updating) returns 0 and *out_value=0;
// callers should treat that as "no fresh PV this frame".
int endgame_ctx_get_live_pv(const EndgameCtx *ctx, int thread_index,
                            uint64_t *out_moves, int max_len,
                            int32_t *out_value);

// One slot of the live multi-PV top-K leaderboard. root_tiny is the
// first move (a SmallMove tiny_move); value is the spread-adjusted
// negamax value (same sign convention as PVLine.score); continuation
// is the rest of the line (depth - 1 moves at most), with
// continuation_len in [0, MAX_SEARCH_DEPTH].
typedef struct EndgameLivePvSnapshot {
  uint64_t root_tiny;
  int32_t value;
  int continuation_len;
  uint64_t continuation_tiny[MAX_SEARCH_DEPTH];
} EndgameLivePvSnapshot;

// Snapshot of the live multi-PV top-K leaderboard from worker
// `thread_index`. Each slot in `out` is filled with one root move,
// its current value, and the continuation line found at this depth.
// Slots are sorted descending by value; the leaderboard is reset at
// the start of each IDS depth and fills in as roots complete (so the
// reader may see fewer than max_k entries early in a depth).
//
// Writes up to min(max_k, MAX_ENDGAME_DISPLAY_PVS, K_currently_filled)
// entries and returns the number written. Uses a seqlock so a
// snapshot is consistent (every entry came from the same writer
// update). Returns 0 if a consistent snapshot can't be obtained
// after a few retries.
int endgame_ctx_get_live_top_k_pvs(const EndgameCtx *ctx, int thread_index,
                                   EndgameLivePvSnapshot *out, int max_k);

#endif
