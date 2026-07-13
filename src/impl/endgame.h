#ifndef ENDGAME_H
#define ENDGAME_H

// Note: The endgame solver currently only supports RACK_SIZE <= 7 and
// BOARD_DIM <= 16. The rack-size limitation comes from SmallMove, which
// encodes up to 7 tiles in a 64-bit integer for memory efficiency during
// search. The board-size limitation comes from MoveUndo's 16-bit
// tiles_placed_mask.

#include "../def/game_defs.h"
#include "../ent/endgame_results.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/kwg.h"
#include "../ent/move.h"
#include "../ent/small_move_arena.h"
#include "../ent/thread_control.h"
#include "../ent/transposition_table.h"
#include <stdint.h>

enum {
  DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE = 1024 * 1024,
  MAX_ENDGAME_DISPLAY_PVS = 100,
};

typedef struct EndgameCtx EndgameCtx;

// ===========================================================================
// Live analysis interface
// ===========================================================================
//
// The solver exposes its in-progress state so a UI (or any observer) can
// render a live view of the search. There are two complementary ways to
// observe; a consumer may use either or both:
//
//   * Push — the callbacks on EndgameArgs (before_search / per_root_move /
//     per_ply). They fire synchronously, on the main worker (ordinal 0), at
//     well-defined moments (search start, each root completion, each completed
//     depth). They deliver exact, in-order state, but they run inside the
//     search: a handler must be cheap, must not block, and must copy out
//     anything it keeps (pointer arguments are valid only for the call).
//     Note the main worker is not necessarily a dedicated thread: endgame_solve
//     runs it on a spawned worker thread, but endgame_solve_inline (the entry
//     point the PEG solver drives) runs it on the *calling* thread — so a
//     handler may execute on the caller's own thread.
//
//   * Pull — the endgame_ctx_get_* accessors below. A separate thread polls
//     these at its own cadence (e.g. a render loop) while endgame_solve runs
//     on another thread. They return lock-free, internally-consistent
//     snapshots (seqlock / release-acquire) and never block the search. A
//     snapshot may be a few nodes stale, which is fine for a heartbeat or a
//     leaderboard.
//
// Everything here is observation only: nothing feeds back into the search, and
// whether or not it is read does not change the result.
//
// Conventions shared by the whole interface:
//
//   * Values are spreads in WHOLE POINTS — not the millipoint Equity used
//     elsewhere in MAGPIE — from the solving player's perspective: positive
//     means the endgame nets points for the solving player. This matches
//     PVLine.score.
//     "Spread-adjusted" below means the root's initial spread has been
//     subtracted from the raw negamax value (which is itself the projected
//     final spread), so the reported number is the delta — the net point
//     swing over the endgame from the current position, not the projected
//     final spread. (The per-ply callback documents this same value as the
//     "spread delta".)
//
//   * Moves are returned as `tiny_move` codes: the compact 64-bit SmallMove
//     encoding (schema in move.h). A code of 0 is a pass. To render one, wrap
//     it in a SmallMove and decode it against the board:
//         SmallMove sm = {0};
//         sm.tiny_move = code;
//         Move move;
//         small_move_to_move(&move, &sm, board);
//     The push callbacks hand back SmallMove pointers directly; the pull
//     accessors hand back raw codes so each snapshot stays a flat, copy-safe
//     value with no engine-owned pointers in it.
//
//   * `worker_index` selects which worker to observe (an index into the
//     solver's worker pool, not an OS thread id). Index 0 is the main worker:
//     it owns root ordering, the progress counters, and the live PV /
//     leaderboard, so a simple UI passes 0 for the canonical view. Higher
//     indices are the parallel ABDADA helper workers and are rarely needed.
// ===========================================================================

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
// leaderboard before any depth completes. Fires from the main worker
// (ordinal 0; see the overview above for which thread that is) after its
// initial-move generation finishes (sub-ms after the solve starts) and
// before any negamax call. The polled
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

// Callback fired each time a root move completes its negamax evaluation at
// the current iterative-deepening depth, from the main worker (ordinal 0)
// only. Lets clients re-rank the leaderboard live (per-root resolution rather
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
// Fires from inside abdada_negamax. Multiple per-root callbacks may happen
// back-to-back at the same depth as roots complete in scan order. Only the
// main worker calls, but it may be any OS thread (see the overview), so a
// handler must not assume a fixed calling thread.
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
  // Generalization of first_win: search every iterative-deepening pass
  // with the fixed window [initial_alpha, initial_beta] (in final-spread
  // units for the solving player, i.e. PVLine score plus the initial
  // spread) and disable aspiration windows. The solve then answers how
  // the exact value relates to the window instead of computing it: a
  // returned value >= initial_beta is a lower bound and a value <=
  // initial_alpha is an upper bound. A null window [x, x+1] proves
  // whether the exact value exceeds x. first_win is equivalent to a
  // [-1, 1] window.
  bool use_initial_window;
  int32_t initial_alpha;
  int32_t initial_beta;
  // Absolute monotonic-ns deadline (ctimer_monotonic_ns()-compatible). If
  // non-zero, workers bail out mid-search once now > deadline. Lets a
  // caller (e.g. PEG) impose a wall-clock budget that propagates through
  // alpha-beta, not just between IDS depth iterations. 0 = no deadline.
  int64_t external_deadline_ns;
  // If non-NULL, the solver additionally computes an exact (non-pruned)
  // value for this specific move in the same search that finds the best
  // move, instead of requiring a second endgame_solve call. Must be a legal
  // move for the position (game's player-on-turn rack); matched against
  // generated root moves by board position + tiles (see
  // move_get_similarity_key), not by pointer identity. Read only during the
  // call; ownership stays with the caller. If the move can't be found among
  // the root moves, ENDGAME_RESULT_ACTUAL is left unset (see
  // endgame_results_get_actual_move_found). Kept at root index 0 through
  // every IDS depth so it always gets an unnarrowed [alpha, beta] window,
  // the same guarantee the root's first move always gets.
  const Move *actual_move;
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
// both entry points: endgame_solve (spawned main worker) and
// endgame_solve_inline (the calling thread is the main worker). It may be
// called from another thread (e.g. a pool lending an idle core) or from the
// solving thread itself — the injection test drives it from the main worker's
// per_ply_callback. The new worker gets the next free ordinal (> 0, never
// ordinal 0), its own ABDADA jitter from that ordinal, a game copy from the
// (read-only) root inputs, and a MoveGen cache slot assigned on demand by
// get_movegen(); it cooperates with the running workers purely through the
// shared TT and self-exits when the search completes. Returns true if a worker
// was spawned, false if the injection window is shut or the max_workers ceiling
// is reached. Only valid against a ctx whose current solve was launched with
// max_workers enabling growth (max_workers > effective thread count after reset
// normalization).
bool endgame_add_worker(EndgameCtx *ctx);

// Number of worker threads currently live in this solve (main worker + injected
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
// Reset (zero) the transposition table contents but keep the allocation.
// Useful between unrelated solves on the same ctx when the caller wants
// each solve to start from a cold cache without paying the realloc cost
// (the cost is the same — a memset of the full TT — but no malloc/free).
// No-op if the ctx has no TT (tt_fraction_of_mem == 0).
void endgame_ctx_clear_transposition_table(EndgameCtx *ctx);
// Coarse progress counters for a progress bar (main worker's view). Writes
// the current iterative-deepening depth, and how many of this depth's root
// moves have finished (completed/total). The ply2_* out-params are an optional
// finer-grained sub-progress — how many children of the first root move have
// finished — for a UI that wants a second bar during a long first root; a
// simple consumer can pass them and ignore the values. All are plain atomic
// reads, cheap from any thread.
void endgame_ctx_get_progress(const EndgameCtx *ctx, int *current_depth,
                              int *root_moves_completed, int *root_moves_total,
                              int *ply2_moves_completed, int *ply2_moves_total);

// Sum of nodes searched across all worker threads. While the search is
// running this lags by up to DEPTH_DEADLINE_CHECK_INTERVAL nodes per thread
// (the period at which each worker flushes its non-atomic local counter to
// the shared atomic); each worker does a final exact flush when it stops, so
// once the solve completes the total is exact. Cheap to call from any thread;
// intended for "is the engine alive / nodes per second" UI heartbeats during
// long single-root subtree evaluations.
uint64_t endgame_ctx_get_nodes_searched(const EndgameCtx *ctx);

// Snapshot of the line currently being explored by worker `worker_index`
// (0 = main worker; see conventions above). Writes up to `max_len` `tiny_move`
// entries into `out_line` and returns the number written (0..max_len). The
// line reads per-worker state without locking: the slots are relaxed atomics
// and the reader uses release/acquire on the length, so the prefix length is
// always consistent and each slot read is well-defined (no torn reads). A slot
// the worker is concurrently overwriting may simply be stale — it can reflect a
// different sibling subtree than the reported length — which is fine for a
// display heartbeat. For display only; never used to drive search decisions.
int endgame_ctx_get_current_line(const EndgameCtx *ctx, int worker_index,
                                 uint64_t *out_line, int max_len);

// Snapshot of the live best PV from worker `worker_index` (0 = main worker).
// Updated by the engine each time best_value improves at the root (so during a
// long depth the reader can see the engine's best line refine as
// successive root moves get evaluated).
//
// Writes up to `max_len` `tiny_move` entries into `out_moves` and
// returns the number written (0..max_len). `*out_value` is set to the
// spread-adjusted value (whole points; see conventions above).
//
// Uses a seqlock so the moves array, length, and value all come
// from the same writer update — never from a half-overwritten
// state. If a consistent snapshot can't be obtained after a few
// retries (writer continuously updating) returns 0 and *out_value=0;
// callers should treat that as "no fresh PV this frame".
int endgame_ctx_get_live_pv(const EndgameCtx *ctx, int worker_index,
                            uint64_t *out_moves, int max_len,
                            int32_t *out_value);

// One slot of the live multi-PV top-K leaderboard: a root move, its current
// spread-adjusted value (whole points; see conventions above), and the
// continuation line found after it (up to depth - 1 more moves,
// continuation_len in [0, MAX_SEARCH_DEPTH]). Moves are tiny_move codes.
//
// This is a flat, copy-safe value type, deliberately distinct from PVLine:
// PVLine carries a Game* and is sized for the search, whereas a poller wants
// to memcpy a leaderboard slice out under the seqlock and keep using it after
// the engine has moved on. The snapshot owns no engine pointers.
typedef struct EndgameLivePvSnapshot {
  uint64_t root_tiny;
  int32_t value;
  int continuation_len;
  uint64_t continuation_tiny[MAX_SEARCH_DEPTH];
} EndgameLivePvSnapshot;

// Snapshot of the live multi-PV top-K leaderboard from worker `worker_index`
// (0 = main worker). Each slot in `out` is filled with one root move,
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
int endgame_ctx_get_live_top_k_pvs(const EndgameCtx *ctx, int worker_index,
                                   EndgameLivePvSnapshot *out, int max_k);

#endif
