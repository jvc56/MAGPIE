#include "bai_peg.h"

#include "../compat/cpthread.h"
#include "../compat/ctime.h"
#include "../def/board_defs.h"
#include "../def/cpthread_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/kwg_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/dictionary_word.h"
#include "../ent/endgame_results.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/thread_control.h"
#include "../ent/transposition_table.h"
#include "../str/move_string.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "endgame.h"
#include "gameplay.h"
#include "kwg_maker.h"
#include "move_gen.h"
#include "word_prune.h"
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  BAI_PEG_DEFAULT_TOP_K = 64,
  BAI_PEG_MOVELIST_CAPACITY = 250000,
  BAI_PEG_DEFAULT_PROGRESS_TOP = 5,
  BP_EXEC_QUEUE_INIT_CAP = 1024,
};

// ---------------------------------------------------------------------------
// Work-stealing executor
// ---------------------------------------------------------------------------
//
// Persistent N-worker pool. Workers loop on a shared FIFO queue protected
// by a mutex + condition variable. Callers submit a "batch" of work items
// (each is a function pointer + opaque arg) and then call _wait_batch to
// block until every item in the batch has run. While blocked, the waiter
// drains queue items itself (help-while-waiting) so deeply nested
// submissions can't deadlock.
//
// Queue grows on demand (push doubles capacity if full). Items carry a
// pointer back to their batch's pending counter + completion CV.

typedef struct BpExecBatch {
  atomic_int pending; // remaining items in this batch
  cpthread_mutex_t mutex;
  cpthread_cond_t cv; // signaled when pending hits 0
} BpExecBatch;

typedef void (*BpExecFn)(void *arg, int worker_idx);

typedef struct BpExecItem {
  BpExecFn fn;
  void *arg;
  BpExecBatch *batch; // may be NULL for fire-and-forget items (unused today)
} BpExecItem;

typedef struct BpExecWorkerCtx {
  struct BaiPegExecutor *executor;
  int worker_idx;
} BpExecWorkerCtx;

struct BaiPegExecutor {
  int num_workers;
  int thread_index_offset;
  cpthread_t *threads;
  BpExecWorkerCtx *worker_ctxs;
  // Ring-ish queue. Simpler: linear buffer with head/tail; grow on overflow.
  BpExecItem *queue;
  int q_head;  // next pop index
  int q_count; // items currently queued
  int q_cap;   // allocated capacity
  cpthread_mutex_t q_mutex;
  cpthread_cond_t q_cv_nonempty;
  bool shutdown;
};

static void bp_exec_batch_init(BpExecBatch *b, int n) {
  atomic_init(&b->pending, n);
  cpthread_mutex_init(&b->mutex);
  cpthread_cond_init(&b->cv);
}

// Push one item onto the queue. Caller must NOT hold q_mutex.
static void bp_exec_push(BaiPegExecutor *e, BpExecItem item) {
  cpthread_mutex_lock(&e->q_mutex);
  if (e->q_count == e->q_cap) {
    // Grow: copy items into a new linearized buffer.
    int new_cap = e->q_cap * 2;
    BpExecItem *new_q = malloc_or_die((size_t)new_cap * sizeof(BpExecItem));
    for (int i = 0; i < e->q_count; i++) {
      new_q[i] = e->queue[(e->q_head + i) % e->q_cap];
    }
    free(e->queue);
    e->queue = new_q;
    e->q_cap = new_cap;
    e->q_head = 0;
  }
  int tail = (e->q_head + e->q_count) % e->q_cap;
  e->queue[tail] = item;
  e->q_count++;
  cpthread_cond_signal(&e->q_cv_nonempty);
  cpthread_mutex_unlock(&e->q_mutex);
}

// Try to pop one item without blocking. Returns true if popped.
static bool bp_exec_try_pop(BaiPegExecutor *e, BpExecItem *out) {
  cpthread_mutex_lock(&e->q_mutex);
  if (e->q_count == 0) {
    cpthread_mutex_unlock(&e->q_mutex);
    return false;
  }
  *out = e->queue[e->q_head];
  e->q_head = (e->q_head + 1) % e->q_cap;
  e->q_count--;
  cpthread_mutex_unlock(&e->q_mutex);
  return true;
}

// Block until an item is available or shutdown is set. Returns false on
// shutdown with no item.
static bool bp_exec_pop_or_shutdown(BaiPegExecutor *e, BpExecItem *out) {
  cpthread_mutex_lock(&e->q_mutex);
  while (e->q_count == 0 && !e->shutdown) {
    cpthread_cond_wait(&e->q_cv_nonempty, &e->q_mutex);
  }
  if (e->q_count == 0) {
    cpthread_mutex_unlock(&e->q_mutex);
    return false;
  }
  *out = e->queue[e->q_head];
  e->q_head = (e->q_head + 1) % e->q_cap;
  e->q_count--;
  cpthread_mutex_unlock(&e->q_mutex);
  return true;
}

// Run an item and decrement its batch's pending counter, signaling the
// batch's CV when the last one drains.
static void bp_exec_run_item(BpExecItem *item, int worker_idx) {
  item->fn(item->arg, worker_idx);
  if (item->batch) {
    int prev = atomic_fetch_sub(&item->batch->pending, 1);
    if (prev == 1) {
      cpthread_mutex_lock(&item->batch->mutex);
      cpthread_cond_broadcast(&item->batch->cv);
      cpthread_mutex_unlock(&item->batch->mutex);
    }
  }
}

static void *bp_exec_worker_main(void *arg) {
  BpExecWorkerCtx *ctx = (BpExecWorkerCtx *)arg;
  while (true) {
    BpExecItem item;
    if (!bp_exec_pop_or_shutdown(ctx->executor, &item)) {
      break;
    }
    bp_exec_run_item(&item, ctx->worker_idx);
  }
  return NULL;
}

// Submit a contiguous array of items as one batch and wait for all to
// complete. The calling thread helps drain the queue while waiting so
// nested submissions (e.g. pass-cand inner work submitted from a worker
// already running outer work) don't deadlock. `helper_worker_idx` is the
// thread index used for cache keying when the helper runs items; pass
// the calling worker's idx if you're inside a worker, else any idx in
// [thread_index_offset, thread_index_offset + num_workers) range that
// won't collide with concurrent activity.
static void bp_exec_submit_and_wait(BaiPegExecutor *e, BpExecItem *items, int n,
                                    int helper_worker_idx) {
  BpExecBatch batch;
  bp_exec_batch_init(&batch, n);
  for (int i = 0; i < n; i++) {
    items[i].batch = &batch;
    bp_exec_push(e, items[i]);
  }
  // Help-while-waiting.
  while (atomic_load(&batch.pending) > 0) {
    BpExecItem item;
    if (bp_exec_try_pop(e, &item)) {
      bp_exec_run_item(&item, helper_worker_idx);
    } else {
      cpthread_mutex_lock(&batch.mutex);
      // Re-check under lock: another helper may have completed it
      // between try_pop returning false and us acquiring the lock.
      if (atomic_load(&batch.pending) > 0) {
        cpthread_cond_wait(&batch.cv, &batch.mutex);
      }
      cpthread_mutex_unlock(&batch.mutex);
    }
  }
}

BaiPegExecutor *bai_peg_executor_create(int num_workers,
                                        int thread_index_offset) {
  if (num_workers < 1) {
    num_workers = 1;
  }
  BaiPegExecutor *e = malloc_or_die(sizeof(*e));
  e->num_workers = num_workers;
  e->thread_index_offset = thread_index_offset;
  e->q_cap = BP_EXEC_QUEUE_INIT_CAP;
  e->queue = malloc_or_die((size_t)e->q_cap * sizeof(BpExecItem));
  e->q_head = 0;
  e->q_count = 0;
  e->shutdown = false;
  cpthread_mutex_init(&e->q_mutex);
  cpthread_cond_init(&e->q_cv_nonempty);
  e->threads = malloc_or_die((size_t)num_workers * sizeof(cpthread_t));
  e->worker_ctxs = malloc_or_die((size_t)num_workers * sizeof(BpExecWorkerCtx));
  for (int i = 0; i < num_workers; i++) {
    e->worker_ctxs[i].executor = e;
    e->worker_ctxs[i].worker_idx = thread_index_offset + i;
    cpthread_create(&e->threads[i], bp_exec_worker_main, &e->worker_ctxs[i]);
  }
  return e;
}

void bai_peg_executor_destroy(BaiPegExecutor *e) {
  if (!e) {
    return;
  }
  cpthread_mutex_lock(&e->q_mutex);
  e->shutdown = true;
  cpthread_cond_broadcast(&e->q_cv_nonempty);
  cpthread_mutex_unlock(&e->q_mutex);
  for (int i = 0; i < e->num_workers; i++) {
    cpthread_join(e->threads[i]);
  }
  free(e->threads);
  free(e->worker_ctxs);
  free(e->queue);
  free(e);
}

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

static int bp_compute_unseen(const Game *game, int mover_idx,
                             uint8_t unseen[MAX_ALPHABET_SIZE]) {
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);
  memset(unseen, 0, sizeof(uint8_t) * MAX_ALPHABET_SIZE);
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] = (uint8_t)ld_get_dist(ld, ml);
  }
  const Rack *mover_rack = player_get_rack(game_get_player(game, mover_idx));
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] -= (uint8_t)rack_get_letter(mover_rack, ml);
  }
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      if (board_is_empty(board, row, col)) {
        continue;
      }
      MachineLetter ml = board_get_letter(board, row, col);
      if (get_is_blanked(ml)) {
        if (unseen[BLANK_MACHINE_LETTER] > 0) {
          unseen[BLANK_MACHINE_LETTER]--;
        }
      } else {
        if (unseen[ml] > 0) {
          unseen[ml]--;
        }
      }
    }
  }
  int total = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    total += unseen[ml];
  }
  return total;
}

static void bp_set_opp_rack(Rack *opp_rack,
                            const uint8_t unseen[MAX_ALPHABET_SIZE],
                            int ld_size, MachineLetter bag_tile) {
  rack_reset(opp_rack);
  for (int ml = 0; ml < ld_size; ml++) {
    int cnt = (int)unseen[ml] - (ml == bag_tile ? 1 : 0);
    for (int i = 0; i < cnt; i++) {
      rack_add_letter(opp_rack, (MachineLetter)ml);
    }
  }
}

static void bp_clear_false_game_end(Game *game) {
  game_set_game_end_reason(game, GAME_END_REASON_NONE);
}

// Pre-bias the scoreless-turn counter so the next scoreless turn ends
// the game. The optA branch in bp_evaluate_pass directly computes the
// "both pass → game ends with end-rack penalties" outcome — but with
// the real-Scrabble default of max_scoreless_turns=6 and count=0 from a
// CGP load, opp's pass after mover's pass is only the 2nd scoreless
// turn (not the 6th), so the game wouldn't actually end and optA's
// terminal assumption is invalid. Setting count=max-1 here makes any
// pass strictly terminal — which matches the optA assumption and also
// propagates correctly through inner sub-PEG game duplications. Plays
// (count→0) reset normally so non-pass cands are unaffected.
static void bp_set_pre_terminal_scoreless(Game *game) {
  const int max = game_get_max_scoreless_turns(game);
  if (max > 0) {
    game_set_consecutive_scoreless_turns(game, max - 1);
  }
}

// ---------------------------------------------------------------------------
// Per-candidate state
// ---------------------------------------------------------------------------

typedef struct BaiCand {
  SmallMove move;
  Move move_full;
  int static_score;        // Move score (used for sort + display).
  Equity equity_for_prior; // Prior softmax basis = score + KLV leave value
                           // (movegen's pre-computed equity field).
  Game *post_cand_game;    // Post-move game state (board + cross-sets), shared.
  double prior;            // Softmax-normalized prior in [0, 1].
  int depth_evaluated;     // 0 = no endgame eval yet; N = N-ply endgame done.
  int visits;              // Number of (depth) evaluations performed.
  double time_paid;        // Cumulative wall time spent evaluating this cand.
  double last_eval_seconds; // Wall time of the most recent evaluation.
  double cost_estimate;     // Predicted seconds for the next evaluation.
  double q_mean_spread;     // Mean spread at deepest evaluated depth (0 init).
  double q_win_pct;         // Win pct at deepest evaluated depth.
  // Snapshots of two prior signals captured exactly once each, before they
  // get overwritten by deeper evaluations. Used downstream to study which
  // signal predicts the eventual real-Q better (regression analysis).
  double playout_q_mean_spread; // From Phase 0 (initial_playout) only.
  double playout_q_win_pct;
  bool playout_q_set;
  double neighbor_q_mean_spread; // Snapshot of cand[rank-1]'s q at admission.
  double neighbor_q_win_pct;
  bool neighbor_q_set;
  bool fully_explored; // True once depth_evaluated >= max_depth.
  bool is_pass;        // Pass candidate; evaluated via recursive
                       // bai_peg_solve on the post-pass game from opp's
                       // perspective (one level of recursion only).
  double sort_utility; // Transient: populated by bp_populate_sort_utility
                       // immediately before each qsort, read by the
                       // qsort comparator. Stale outside that window.
  // For pass cand only: per-scenario coupled inner sessions, lazily
  // initialized on first pass-eval, deepened incrementally on each
  // mover-pass-revisit so opp's PUCT state persists. Length matches the
  // outer solver's num_tile_types and is stored in coupled_count.
  // NULL on non-pass cands.
  struct BpInnerSession *coupled_inner_sessions;
  int coupled_count;
  // Pass-cand only: KWGs pruned against the unplayed pool (board-derived,
  // identical across all bag-tile scenarios). Built once at the first
  // pass-eval and shared across every scenario's inner session, replacing
  // N redundant prunes per pass eval. NULL slot = use the player's full
  // KWG. Slot 1 stays NULL in DUAL_LEXICON_MODE_IGNORANT. Owned by this
  // cand; freed in the cleanup loop in bai_peg_solve.
  KWG *cached_pruned_kwgs[2];
} BaiCand;

// Artificial prior basis for the pass candidate. The mover's rack has 7
// tiles; for each of the 7 possible 6-tile leaves (one per dropped tile,
// counting duplicates with their multiplicity), look up KLV's value of
// keeping that 6-leave. Average over the 7 slots. Returns the average
// in Equity units (millipoints) so it composes with movegen's equity
// field for the rest of the prior pipeline. Returns 0 if klv is NULL.
static Equity bp_pass_artificial_prior(const KLV *klv, const Rack *full_rack,
                                       int ld_size) {
  if (klv == NULL) {
    return 0;
  }
  double total_eq = 0.0;
  int count = 0;
  Rack leave;
  rack_set_dist_size_and_reset(&leave, ld_size);
  for (int ml = 0; ml < ld_size; ml++) {
    int n = (int)rack_get_letter(full_rack, ml);
    if (n == 0) {
      continue;
    }
    rack_copy(&leave, full_rack);
    rack_take_letter(&leave, (MachineLetter)ml);
    Equity lv = klv_get_leave_value(klv, &leave);
    // The same 6-leave is reused for each duplicate of this letter; weight
    // by multiplicity so the average is over rack slots, not distinct types.
    total_eq += equity_to_double(lv) * (double)n;
    count += n;
  }
  if (count == 0) {
    return 0;
  }
  return double_to_equity(total_eq / (double)count);
}

// Structural prior on the cost of evaluating one (cand, depth) eval before
// any measurements exist. After the first eval the running measurement
// replaces this. Captures: (a) cheaper as candidate consumes more rack
// tiles (smaller post-move endgame), (b) PASS/EXCHANGE keeps full rack
// (most expensive), (c) roughly doubles per ply.
static double bp_initial_cost_estimate(const Move *move, int depth) {
  // Tiles played by this candidate. PASS/EXCHANGE leaves full rack on board.
  int tiles_played = (move->move_type == GAME_EVENT_TILE_PLACEMENT_MOVE)
                         ? move_get_tiles_played(move)
                         : 0;
  int rack_remaining = RACK_SIZE - tiles_played; // 0..7
  const double base = 0.01;          // ~10ms baseline at depth 1, empty rack
  const double growth_per_ply = 2.0; // rough endgame branching factor
  // (rack² + 1) so an emptied-rack candidate isn't predicted ~free.
  double rack_factor = (double)(rack_remaining * rack_remaining) + 1.0;
  return base * rack_factor * pow(growth_per_ply, (double)depth - 1);
}

// ---------------------------------------------------------------------------
// Parallel scenario evaluation: for one (cand, depth), each worker thread
// pulls a tile_idx atomically and runs an endgame solve on that scenario.
// ---------------------------------------------------------------------------

typedef struct BpEvalThreadArgs {
  BaiCand *cand;
  int depth;
  int mover_idx;
  int opp_idx;
  int thread_index;
  const uint8_t *unseen;
  int ld_size;
  const MachineLetter *tile_types;
  const int *tile_counts;
  int num_tile_types;
  EndgameCtx **endgame_ctx;        // Per-thread, lazily initialized.
  EndgameResults *endgame_results; // Per-thread.
  ThreadControl *thread_control;
  TranspositionTable *shared_tt;
  dual_lexicon_mode_t dual_lexicon_mode;
  double endgame_time_per_solve;
  int64_t external_deadline_ns;
  // Shared work-stealing counter (only inner-loop atomic).
  atomic_int *next_tile;
  // Per-thread output accumulators (no atomics; merged after join).
  int64_t local_spread_sum;
  int64_t local_wins_x2;
  int64_t local_weight_sum;
} BpEvalThreadArgs;

static void *bp_eval_thread(void *arg) {
  BpEvalThreadArgs *a = (BpEvalThreadArgs *)arg;
  int64_t local_spread_sum = 0;
  int64_t local_wins_x2 = 0;
  int64_t local_weight_sum = 0;
  while (true) {
    int ti = atomic_fetch_add(a->next_tile, 1);
    if (ti >= a->num_tile_types) {
      break;
    }
    MachineLetter tile = a->tile_types[ti];
    int tcnt = a->tile_counts[ti];

    Game *scenario = game_duplicate(a->cand->post_cand_game);
    Rack *opp_rack = player_get_rack(game_get_player(scenario, a->opp_idx));
    bp_set_opp_rack(opp_rack, a->unseen, a->ld_size, tile);
    Rack *mover_rack = player_get_rack(game_get_player(scenario, a->mover_idx));
    rack_add_letter(mover_rack, tile);

    int32_t mover_lead =
        equity_to_int(
            player_get_score(game_get_player(scenario, a->mover_idx))) -
        equity_to_int(player_get_score(game_get_player(scenario, a->opp_idx)));

    EndgameArgs ea = {
        .thread_control = a->thread_control,
        .game = scenario,
        .plies = a->depth,
        .shared_tt = a->shared_tt,
        .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
        .num_threads = 1,
        .use_heuristics = true,
        .num_top_moves = 1,
        .dual_lexicon_mode = a->dual_lexicon_mode,
        .skip_word_pruning = true,
        .thread_index_offset = a->thread_index,
        .soft_time_limit = a->endgame_time_per_solve,
        .hard_time_limit = a->endgame_time_per_solve,
        .external_deadline_ns = a->external_deadline_ns,
    };

    endgame_results_reset(a->endgame_results);
    endgame_solve_inline(a->endgame_ctx, &ea, a->endgame_results);
    int eg_val =
        endgame_results_get_value(a->endgame_results, ENDGAME_RESULT_BEST);
    int32_t mover_total = mover_lead - eg_val;

    local_spread_sum += (int64_t)mover_total * tcnt;
    int win_contrib;
    if (mover_total > 0) {
      win_contrib = 2 * tcnt;
    } else if (mover_total == 0) {
      win_contrib = tcnt;
    } else {
      win_contrib = 0;
    }
    local_wins_x2 += win_contrib;
    local_weight_sum += tcnt;

    game_destroy(scenario);
  }
  a->local_spread_sum = local_spread_sum;
  a->local_wins_x2 = local_wins_x2;
  a->local_weight_sum = local_weight_sum;
  return NULL;
}

// ---------------------------------------------------------------------------
// Flattened batch evaluation: a single shared (cand, scenario) work queue
// across all worker threads. One spawn/join cycle for all num_cands ×
// num_tile_types scenarios — vs. the per-cand bp_evaluate_cand which pays
// cpthread_create/join for every candidate. Used by Phase 0 (initial_playout)
// where 256+ cheap depth-0 evaluations would otherwise spend most of their
// time on thread ceremony.
// ---------------------------------------------------------------------------

typedef struct BpBatchWorkItem {
  int cand_idx;
  int tile_idx;
} BpBatchWorkItem;

typedef struct BpBatchThreadArgs {
  atomic_int *next_item;
  const BpBatchWorkItem *items;
  int total_items;
  BaiCand *cands; // indexed by item->cand_idx
  int mover_idx;
  int opp_idx;
  const uint8_t *unseen;
  int ld_size;
  const MachineLetter *tile_types;
  const int *tile_counts;
  int thread_index;
  EndgameCtx **endgame_ctx;
  EndgameResults *endgame_results;
  // Per-thread move list reused across all (cand, scenario) work items
  // when pure_playout is set. NULL when going through endgame_solve_inline.
  MoveList *move_list;
  ThreadControl *thread_control;
  TranspositionTable *shared_tt;
  dual_lexicon_mode_t dual_lexicon_mode;
  int depth;
  double endgame_time_per_solve;
  int64_t external_deadline_ns;
  // If true, skip endgame_solve_inline plumbing and run a flat greedy
  // playout (movegen + play loop, no TT, no alpha-beta).
  bool pure_playout;
  // Per-thread, per-cand accumulators (length = num_cands; this thread's row
  // in the flat all_spread/all_wins/all_weight arrays).
  int64_t *local_spread;
  int64_t *local_wins_x2;
  int64_t *local_weight;
} BpBatchThreadArgs;

// Streamlined greedy playout: from `game`'s current position (post-cand
// move played, opp rack and bag-tile placed), play highest-scoring moves
// alternately until end-of-game or a safety cap. Returns final spread
// from `solving_player`'s perspective in equity-int units.
//
// No TT, no alpha-beta, no endgame_solve infrastructure. Just movegen +
// play. Standard end-of-game scoring is handled by play_move; if we hit
// the safety cap without natural end, apply rack penalties manually.
// Same as bp_greedy_playout but uses MOVE_RECORD_ALL + take move[0]. The
// MOVE_RECORD_BEST path runs compare_moves(allow_duplicates=false) and
// trips on byte-identical moves under the inner sub-PEG's state (pruned
// KWG + post-cand cross-sets). Used by the inner greedy rerank only.
static int32_t bp_inner_greedy_playout(Game *game, int solving_player,
                                       MoveList *move_list, int thread_index) {
  for (int turn = 0; turn < MAX_SEARCH_DEPTH; turn++) {
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      break;
    }
    const MoveGenArgs ga = {
        .game = game,
        .move_list = move_list,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_SCORE,
        .override_kwg = NULL,
        .thread_index = thread_index,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&ga);
    if (move_list_get_count(move_list) == 0) {
      break;
    }
    const Move *best = move_list_get_move(move_list, 0);
    play_move(best, game, NULL);
  }
  const Player *me = game_get_player(game, solving_player);
  const Player *op = game_get_player(game, 1 - solving_player);
  int32_t spread = equity_to_int(player_get_score(me) - player_get_score(op));
  if (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    const LetterDistribution *ld = game_get_ld(game);
    spread -= (int32_t)equity_to_int(rack_get_score(ld, player_get_rack(me)));
    spread += (int32_t)equity_to_int(rack_get_score(ld, player_get_rack(op)));
  }
  return spread;
}

static int32_t bp_greedy_playout(Game *game, int solving_player,
                                 MoveList *move_list, int thread_index) {
  for (int turn = 0; turn < MAX_SEARCH_DEPTH; turn++) {
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      break;
    }
    const MoveGenArgs ga = {
        .game = game,
        .move_list = move_list,
        .move_record_type = MOVE_RECORD_BEST,
        .move_sort_type = MOVE_SORT_SCORE,
        .override_kwg = NULL,
        .thread_index = thread_index,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&ga);
    if (move_list_get_count(move_list) == 0) {
      break;
    }
    const Move *best = move_list_get_move(move_list, 0);
    play_move(best, game, NULL);
  }
  const Player *me = game_get_player(game, solving_player);
  const Player *op = game_get_player(game, 1 - solving_player);
  int32_t spread = equity_to_int(player_get_score(me) - player_get_score(op));
  // If we hit the cap before the game ended naturally, apply standard
  // rack-value adjustments.
  if (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    const LetterDistribution *ld = game_get_ld(game);
    spread -= (int32_t)equity_to_int(rack_get_score(ld, player_get_rack(me)));
    spread += (int32_t)equity_to_int(rack_get_score(ld, player_get_rack(op)));
  }
  return spread;
}

static void *bp_batch_thread(void *arg) {
  BpBatchThreadArgs *a = (BpBatchThreadArgs *)arg;
  while (true) {
    int idx = atomic_fetch_add(a->next_item, 1);
    if (idx >= a->total_items) {
      break;
    }
    int cand_idx = a->items[idx].cand_idx;
    int tile_idx = a->items[idx].tile_idx;
    const BaiCand *cand = &a->cands[cand_idx];
    MachineLetter tile = a->tile_types[tile_idx];
    int tcnt = a->tile_counts[tile_idx];

    Game *scenario = game_duplicate(cand->post_cand_game);
    Rack *opp_rack = player_get_rack(game_get_player(scenario, a->opp_idx));
    bp_set_opp_rack(opp_rack, a->unseen, a->ld_size, tile);
    Rack *mover_rack = player_get_rack(game_get_player(scenario, a->mover_idx));
    rack_add_letter(mover_rack, tile);

    int32_t mover_total;
    if (a->pure_playout) {
      // Streamlined: no endgame_solve, no TT, no alpha-beta. Just play
      // greedy moves to end-of-game and read off the spread.
      mover_total = bp_greedy_playout(scenario, a->mover_idx, a->move_list,
                                      a->thread_index);
    } else {
      int32_t mover_lead =
          equity_to_int(
              player_get_score(game_get_player(scenario, a->mover_idx))) -
          equity_to_int(
              player_get_score(game_get_player(scenario, a->opp_idx)));
      EndgameArgs ea = {
          .thread_control = a->thread_control,
          .game = scenario,
          .plies = a->depth,
          .shared_tt = a->shared_tt,
          .initial_small_move_arena_size =
              DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
          .num_threads = 1,
          .use_heuristics = true,
          .num_top_moves = 1,
          .dual_lexicon_mode = a->dual_lexicon_mode,
          .skip_word_pruning = true,
          .thread_index_offset = a->thread_index,
          .soft_time_limit = a->endgame_time_per_solve,
          .hard_time_limit = a->endgame_time_per_solve,
          .external_deadline_ns = a->external_deadline_ns,
      };
      endgame_results_reset(a->endgame_results);
      endgame_solve_inline(a->endgame_ctx, &ea, a->endgame_results);
      int eg_val =
          endgame_results_get_value(a->endgame_results, ENDGAME_RESULT_BEST);
      mover_total = mover_lead - eg_val;
    }

    a->local_spread[cand_idx] += (int64_t)mover_total * tcnt;
    int win_contrib;
    if (mover_total > 0) {
      win_contrib = 2 * tcnt;
    } else if (mover_total == 0) {
      win_contrib = tcnt;
    } else {
      win_contrib = 0;
    }
    a->local_wins_x2[cand_idx] += win_contrib;
    a->local_weight[cand_idx] += tcnt;

    game_destroy(scenario);
  }
  return NULL;
}

// Evaluate every candidate at a given depth in a single parallel pass.
// Updates each cand->q_win_pct, q_mean_spread, depth_evaluated, visits.
// One thread spawn/join cycle for all num_cands*num_tile_types scenarios.
static void bp_playout_batch(
    BaiCand *cands, int num_cands, int depth, int mover_idx, int opp_idx,
    const uint8_t *unseen, int ld_size, const MachineLetter *tile_types,
    const int *tile_counts, int num_tile_types, ThreadControl *thread_control,
    TranspositionTable **per_thread_tts, dual_lexicon_mode_t dlm,
    double endgame_time_per_solve, int num_threads, int thread_index_offset,
    int64_t external_deadline_ns, bool pure_playout) {
  int total_items = num_cands * num_tile_types;
  BpBatchWorkItem *items = malloc_or_die(total_items * sizeof(BpBatchWorkItem));
  int k = 0;
  for (int ci = 0; ci < num_cands; ci++) {
    for (int ti = 0; ti < num_tile_types; ti++) {
      items[k].cand_idx = ci;
      items[k].tile_idx = ti;
      k++;
    }
  }

  atomic_int next_item;
  atomic_init(&next_item, 0);

  EndgameCtx **ctxs = calloc_or_die(num_threads, sizeof(EndgameCtx *));
  EndgameResults **results =
      malloc_or_die(num_threads * sizeof(EndgameResults *));
  for (int ti = 0; ti < num_threads; ti++) {
    results[ti] = endgame_results_create();
  }
  // Per-thread MoveLists used by bp_greedy_playout when pure_playout is on.
  // We only need top-1 per movegen.
  MoveList **move_lists = NULL;
  if (pure_playout) {
    move_lists = malloc_or_die(num_threads * sizeof(MoveList *));
    for (int ti = 0; ti < num_threads; ti++) {
      move_lists[ti] = move_list_create(1);
    }
  }

  // Per-thread per-cand accumulators stored in flat arrays so we can free
  // them in one shot.
  int64_t *all_spread =
      calloc_or_die((size_t)num_threads * (size_t)num_cands, sizeof(int64_t));
  int64_t *all_wins =
      calloc_or_die((size_t)num_threads * (size_t)num_cands, sizeof(int64_t));
  int64_t *all_weight =
      calloc_or_die((size_t)num_threads * (size_t)num_cands, sizeof(int64_t));

  BpBatchThreadArgs *targs =
      malloc_or_die(num_threads * sizeof(BpBatchThreadArgs));
  cpthread_t *threads = malloc_or_die(num_threads * sizeof(cpthread_t));

  for (int ti = 0; ti < num_threads; ti++) {
    targs[ti] = (BpBatchThreadArgs){
        .next_item = &next_item,
        .items = items,
        .total_items = total_items,
        .cands = cands,
        .mover_idx = mover_idx,
        .opp_idx = opp_idx,
        .unseen = unseen,
        .ld_size = ld_size,
        .tile_types = tile_types,
        .tile_counts = tile_counts,
        .thread_index = thread_index_offset + ti,
        .endgame_ctx = &ctxs[ti],
        .endgame_results = results[ti],
        .move_list = move_lists ? move_lists[ti] : NULL,
        .thread_control = thread_control,
        .shared_tt = per_thread_tts ? per_thread_tts[ti] : NULL,
        .dual_lexicon_mode = dlm,
        .depth = depth,
        .endgame_time_per_solve = endgame_time_per_solve,
        .external_deadline_ns = external_deadline_ns,
        .pure_playout = pure_playout,
        .local_spread = &all_spread[(size_t)ti * num_cands],
        .local_wins_x2 = &all_wins[(size_t)ti * num_cands],
        .local_weight = &all_weight[(size_t)ti * num_cands],
    };
    cpthread_create(&threads[ti], bp_batch_thread, &targs[ti]);
  }
  for (int ti = 0; ti < num_threads; ti++) {
    cpthread_join(threads[ti]);
  }

  // Reduce per-thread accumulators into per-cand Q values.
  for (int ci = 0; ci < num_cands; ci++) {
    int64_t spread_sum = 0;
    int64_t wins_sum = 0;
    int64_t weight_sum = 0;
    for (int ti = 0; ti < num_threads; ti++) {
      spread_sum += all_spread[((size_t)ti * num_cands) + ci];
      wins_sum += all_wins[((size_t)ti * num_cands) + ci];
      weight_sum += all_weight[((size_t)ti * num_cands) + ci];
    }
    if (weight_sum > 0) {
      cands[ci].q_mean_spread = (double)spread_sum / (double)weight_sum;
      cands[ci].q_win_pct = (double)wins_sum / (2.0 * (double)weight_sum);
    }
    cands[ci].depth_evaluated = depth;
    cands[ci].visits++;
  }

  for (int ti = 0; ti < num_threads; ti++) {
    endgame_ctx_destroy(ctxs[ti]);
    endgame_results_destroy(results[ti]);
  }
  if (move_lists) {
    for (int ti = 0; ti < num_threads; ti++) {
      move_list_destroy(move_lists[ti]);
    }
    free(move_lists);
  }
  free(ctxs);
  free(results);
  free(targs);
  free(threads);
  free(items);
  free(all_spread);
  free(all_wins);
  free(all_weight);
}

// Per (cand, scenario_tile) work item for executor-driven evaluation.
typedef struct BpCandTileWork {
  // Inputs.
  BaiCand *cand;
  int depth;
  int mover_idx;
  int opp_idx;
  const uint8_t *unseen;
  int ld_size;
  MachineLetter tile;
  int tcnt;
  ThreadControl *thread_control;
  TranspositionTable *shared_tt;
  dual_lexicon_mode_t dlm;
  double endgame_time_per_solve;
  int64_t external_deadline_ns;
  // Outputs.
  int64_t spread_contrib;
  int64_t wins_x2_contrib;
  int64_t weight_contrib;
} BpCandTileWork;

static void bp_cand_tile_eval(BpCandTileWork *w, int worker_idx) {
  Game *scenario = game_duplicate(w->cand->post_cand_game);
  Rack *opp_rack = player_get_rack(game_get_player(scenario, w->opp_idx));
  bp_set_opp_rack(opp_rack, w->unseen, w->ld_size, w->tile);
  Rack *mover_rack = player_get_rack(game_get_player(scenario, w->mover_idx));
  rack_add_letter(mover_rack, w->tile);

  int32_t mover_lead =
      equity_to_int(player_get_score(game_get_player(scenario, w->mover_idx))) -
      equity_to_int(player_get_score(game_get_player(scenario, w->opp_idx)));

  EndgameCtx *eg_ctx = NULL;
  EndgameResults *eg_results = endgame_results_create();
  EndgameArgs ea = {
      .thread_control = w->thread_control,
      .game = scenario,
      .plies = w->depth,
      .shared_tt = w->shared_tt,
      .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      .num_threads = 1,
      .use_heuristics = true,
      .num_top_moves = 1,
      .dual_lexicon_mode = w->dlm,
      .skip_word_pruning = true,
      .thread_index_offset = worker_idx,
      .soft_time_limit = w->endgame_time_per_solve,
      .hard_time_limit = w->endgame_time_per_solve,
      .external_deadline_ns = w->external_deadline_ns,
  };
  endgame_solve_inline(&eg_ctx, &ea, eg_results);
  int eg_val = endgame_results_get_value(eg_results, ENDGAME_RESULT_BEST);
  int32_t mover_total = mover_lead - eg_val;

  w->spread_contrib = (int64_t)mover_total * w->tcnt;
  if (mover_total > 0) {
    w->wins_x2_contrib = 2 * (int64_t)w->tcnt;
  } else if (mover_total == 0) {
    w->wins_x2_contrib = w->tcnt;
  } else {
    w->wins_x2_contrib = 0;
  }
  w->weight_contrib = w->tcnt;

  endgame_ctx_destroy(eg_ctx);
  endgame_results_destroy(eg_results);
  game_destroy(scenario);
}

static void bp_cand_tile_work_fn(void *arg, int worker_idx) {
  bp_cand_tile_eval((BpCandTileWork *)arg, worker_idx);
}

// One (cand, inner_tile) unit of work for the inner sub-PEG's greedy
// rerank. Submitted via bp_exec_submit_and_wait so each item runs on a
// pool worker and uses that worker's thread-local MoveGen — keeping the
// playout's many generate_moves calls inside the executor's
// num_workers-MoveGen pool rather than running inline within init.
typedef struct BpInnerGreedyWork {
  Game *post_cand_game; // owned by inner_session->cands[ci]
  const uint8_t *unseen;
  int ld_size;
  int inner_mover_idx;
  int inner_opp_idx;
  MachineLetter tile;
  int weight; // tile count, for caller-side aggregation
  int32_t out_mover_total;
} BpInnerGreedyWork;

static void bp_inner_greedy_work_fn(void *arg, int worker_idx) {
  BpInnerGreedyWork *w = (BpInnerGreedyWork *)arg;
  Game *scenario = game_duplicate(w->post_cand_game);
  Rack *opp_rack =
      player_get_rack(game_get_player(scenario, w->inner_opp_idx));
  bp_set_opp_rack(opp_rack, w->unseen, w->ld_size, w->tile);
  Rack *mover_rack =
      player_get_rack(game_get_player(scenario, w->inner_mover_idx));
  rack_add_letter(mover_rack, w->tile);
  MoveList *playout_ml = move_list_create(4096);
  w->out_mover_total = bp_inner_greedy_playout(
      scenario, w->inner_mover_idx, playout_ml, worker_idx);
  move_list_destroy(playout_ml);
  game_destroy(scenario);
}

// Evaluate one candidate at a given depth across all bag-tile scenarios in
// parallel. Updates cand->q_mean_spread, q_win_pct, depth_evaluated, visits.
static void bp_evaluate_cand(
    BaiCand *cand, int depth, int mover_idx, int opp_idx, const uint8_t *unseen,
    int ld_size, const MachineLetter *tile_types, const int *tile_counts,
    int num_tile_types, ThreadControl *thread_control,
    TranspositionTable **per_thread_tts, dual_lexicon_mode_t dlm,
    double endgame_time_per_solve, int num_threads, int thread_index_offset,
    int64_t external_deadline_ns, BaiPegExecutor *executor) {
  if (executor) {
    BpCandTileWork *works =
        calloc_or_die((size_t)num_tile_types, sizeof(BpCandTileWork));
    BpExecItem *items =
        malloc_or_die((size_t)num_tile_types * sizeof(BpExecItem));
    for (int ti = 0; ti < num_tile_types; ti++) {
      works[ti].cand = cand;
      works[ti].depth = depth;
      works[ti].mover_idx = mover_idx;
      works[ti].opp_idx = opp_idx;
      works[ti].unseen = unseen;
      works[ti].ld_size = ld_size;
      works[ti].tile = tile_types[ti];
      works[ti].tcnt = tile_counts[ti];
      works[ti].thread_control = thread_control;
      // Per-thread TTs aren't naturally addressable by an executor worker
      // (the worker pool's ids are independent of the parent solve's
      // num_threads). Skip TT in executor mode for now — endgame_solve will
      // create an ephemeral one per item. Future: per-worker TTs in the
      // executor itself.
      works[ti].shared_tt = NULL;
      works[ti].dlm = dlm;
      works[ti].endgame_time_per_solve = endgame_time_per_solve;
      works[ti].external_deadline_ns = external_deadline_ns;
      items[ti].fn = bp_cand_tile_work_fn;
      items[ti].arg = &works[ti];
      items[ti].batch = NULL;
    }
    bp_exec_submit_and_wait(executor, items, num_tile_types,
                            thread_index_offset);

    int64_t spread_sum = 0;
    int64_t wins_x2 = 0;
    int64_t weight_sum = 0;
    for (int ti = 0; ti < num_tile_types; ti++) {
      spread_sum += works[ti].spread_contrib;
      wins_x2 += works[ti].wins_x2_contrib;
      weight_sum += works[ti].weight_contrib;
    }
    free(works);
    free(items);
    if (weight_sum > 0) {
      cand->q_mean_spread = (double)spread_sum / (double)weight_sum;
      cand->q_win_pct = (double)wins_x2 / (2.0 * (double)weight_sum);
    }
    cand->depth_evaluated = depth;
    cand->visits++;
    (void)per_thread_tts;
    (void)num_threads;
    return;
  }
  atomic_int next_tile;
  atomic_init(&next_tile, 0);

  EndgameCtx **ctxs = calloc_or_die(num_threads, sizeof(EndgameCtx *));
  EndgameResults **results =
      malloc_or_die(num_threads * sizeof(EndgameResults *));
  for (int ti = 0; ti < num_threads; ti++) {
    results[ti] = endgame_results_create();
  }

  BpEvalThreadArgs *targs =
      malloc_or_die(num_threads * sizeof(BpEvalThreadArgs));
  cpthread_t *threads = malloc_or_die(num_threads * sizeof(cpthread_t));

  for (int ti = 0; ti < num_threads; ti++) {
    targs[ti] = (BpEvalThreadArgs){
        .cand = cand,
        .depth = depth,
        .mover_idx = mover_idx,
        .opp_idx = opp_idx,
        .thread_index = thread_index_offset + ti,
        .unseen = unseen,
        .ld_size = ld_size,
        .tile_types = tile_types,
        .tile_counts = tile_counts,
        .num_tile_types = num_tile_types,
        .endgame_ctx = &ctxs[ti],
        .endgame_results = results[ti],
        .thread_control = thread_control,
        .shared_tt = per_thread_tts ? per_thread_tts[ti] : NULL,
        .dual_lexicon_mode = dlm,
        .endgame_time_per_solve = endgame_time_per_solve,
        .external_deadline_ns = external_deadline_ns,
        .next_tile = &next_tile,
        .local_spread_sum = 0,
        .local_wins_x2 = 0,
        .local_weight_sum = 0,
    };
    cpthread_create(&threads[ti], bp_eval_thread, &targs[ti]);
  }
  for (int ti = 0; ti < num_threads; ti++) {
    cpthread_join(threads[ti]);
  }

  int64_t spread_sum = 0;
  int64_t wins_x2 = 0;
  int64_t weight_sum = 0;
  for (int ti = 0; ti < num_threads; ti++) {
    spread_sum += targs[ti].local_spread_sum;
    wins_x2 += targs[ti].local_wins_x2;
    weight_sum += targs[ti].local_weight_sum;
  }

  for (int ti = 0; ti < num_threads; ti++) {
    endgame_ctx_destroy(ctxs[ti]);
    endgame_results_destroy(results[ti]);
  }
  free(ctxs);
  free(results);
  free(targs);
  free(threads);

  if (weight_sum > 0) {
    cand->q_mean_spread = (double)spread_sum / (double)weight_sum;
    cand->q_win_pct = (double)wins_x2 / (2.0 * (double)weight_sum);
  }
  cand->depth_evaluated = depth;
  cand->visits++;
}

// ---------------------------------------------------------------------------
// Ranking helpers
// ---------------------------------------------------------------------------

// Sort cands by their prior basis (descending). Movegen already produces
// moves in equity-descending order; this comparator exists as a safety
// net after the pass filter so the cands array's basis ordering is an
// invariant the rest of the file can rely on.
static int bp_compare_cands_by_prior(const void *a, const void *b) {
  const BaiCand *ca = (const BaiCand *)a;
  const BaiCand *cb = (const BaiCand *)b;
  if (cb->equity_for_prior > ca->equity_for_prior) {
    return 1;
  }
  if (cb->equity_for_prior < ca->equity_for_prior) {
    return -1;
  }
  return 0;
}

// Stateless utility helper. Callers pass alpha explicitly; the qsort
// comparator below relies on bp_populate_sort_utility caching the result
// onto each cand's sort_utility field rather than reading a shared
// global, so concurrent bai_peg_solve calls do not race on alpha.
static double bp_compute_utility(const BaiCand *c, double alpha) {
  return c->q_win_pct + (alpha * c->q_mean_spread);
}

// Populate cand->sort_utility for every cand in the array. Must be called
// immediately before any qsort that uses bp_compare_cand_ptrs_by_q; the
// comparator reads only sort_utility and never alpha.
static void bp_populate_sort_utility(BaiCand *cands, int num_candidates,
                                     double alpha) {
  for (int ci = 0; ci < num_candidates; ci++) {
    cands[ci].sort_utility = bp_compute_utility(&cands[ci], alpha);
  }
}

// Sort by sort_utility unconditionally — used by the inner-session greedy
// rerank where every cand has a freshly computed greedy q but visits is
// still 0 (negamax hasn't run yet). The general PUCT comparator below
// gates on visits>0 first, which would silently fall back to static_score
// during the rerank phase.
static int bp_compare_cand_ptrs_by_sort_utility(const void *a, const void *b) {
  const BaiCand *const *pa = (const BaiCand *const *)a;
  const BaiCand *const *pb = (const BaiCand *const *)b;
  if ((*pa)->sort_utility > (*pb)->sort_utility) {
    return -1;
  }
  if ((*pa)->sort_utility < (*pb)->sort_utility) {
    return 1;
  }
  if ((*pa)->q_mean_spread > (*pb)->q_mean_spread) {
    return -1;
  }
  if ((*pa)->q_mean_spread < (*pb)->q_mean_spread) {
    return 1;
  }
  return 0;
}

static int bp_compare_cand_ptrs_by_q(const void *a, const void *b) {
  const BaiCand *const *pa = (const BaiCand *const *)a;
  const BaiCand *const *pb = (const BaiCand *const *)b;
  // Visited cands always rank above unvisited. Without this, in a losing
  // position a visited cand (q_win=0, spread=-50) ranks BELOW unvisited
  // cands (q_win=0, spread=0), and qsort instability bubbles a never-
  // explored move (often pass, the lowest static-score arm) to ranking[0].
  const bool va = (*pa)->visits > 0;
  const bool vb = (*pb)->visits > 0;
  if (va != vb) {
    return va ? -1 : 1;
  }
  if (!va) {
    // Both unvisited: fall back to static score (matches greedy ranking).
    return (*pb)->static_score - (*pa)->static_score;
  }
  // Both visited: cached utility (win_pct + alpha * mean_spread), descending.
  double ua = (*pa)->sort_utility;
  double ub = (*pb)->sort_utility;
  if (ua > ub) {
    return -1;
  }
  if (ua < ub) {
    return 1;
  }
  // Tiebreak: raw mean spread descending (matters when alpha == 0).
  if ((*pa)->q_mean_spread > (*pb)->q_mean_spread) {
    return -1;
  }
  if ((*pa)->q_mean_spread < (*pb)->q_mean_spread) {
    return 1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Coupled inner session: one BpInnerSession per (pass-cand, scenario_tile).
// Holds a long-lived inner sub-solve state that persists across mover's
// pass-revisits. On each visit we deepen each cand from its current
// depth_evaluated to the requested target_depth, skipping work already done.
// ---------------------------------------------------------------------------

typedef struct BpInnerSession {
  bool initialized;
  // Inner mover/opp indices match the OUTER solver's: opp_idx is who owns
  // the scenario rack (= outer opp_idx), mover_idx is who plays endgame
  // after the pre-endgame move (= outer mover_idx).
  int inner_mover_idx; // outer's opp_idx (the player we model picking M_opp)
  int inner_opp_idx;   // outer's mover_idx
  int ld_size;
  Game *base_game; // post-staging (mover-on-turn flipped, override_kwgs set)
  bool kwgs_owned;
  KWG *pruned_kwgs[2];
  dual_lexicon_mode_t dlm;
  // Inner-cand pool. Each cand has its own post_cand_game; depth_evaluated
  // is updated by bp_evaluate_cand and persists across advance() calls.
  BaiCand *cands;
  int num_cands;
  // Inner scenario distribution (= unseen as seen from inner mover).
  uint8_t inner_unseen[MAX_ALPHABET_SIZE];
  MachineLetter inner_tile_types[MAX_ALPHABET_SIZE];
  int inner_tile_counts[MAX_ALPHABET_SIZE];
  int inner_num_tile_types;
} BpInnerSession;

// Build the persistent state once. After this returns, the session is ready
// for repeated advance() calls. Mirrors the relevant subset of
// bai_peg_solve's setup (movegen, KWG prune, cand build, post-cand-game
// pre-staging) — without TT, progressive widening, or pass cands.
static void bp_inner_session_init(BpInnerSession *s, const BaiPegArgs *outer,
                                  const Game *outer_game, int outer_mover_idx,
                                  const uint8_t outer_unseen[MAX_ALPHABET_SIZE],
                                  MachineLetter scenario_bag_tile, int ld_size,
                                  int initial_top_k, int worker_idx,
                                  KWG *const shared_pruned_kwgs[2]) {
  s->inner_mover_idx = 1 - outer_mover_idx; // outer opp == inner mover
  s->inner_opp_idx = outer_mover_idx;
  s->ld_size = ld_size;
  s->dlm = outer->dual_lexicon_mode;

  // Build base sub-game: opp_rack (= inner mover's rack) = unseen - {bag_tile},
  // bag = {bag_tile}, inner mover on turn.
  s->base_game = game_duplicate(outer_game);
  {
    Bag *sub_bag = game_get_bag(s->base_game);
    Rack *inner_mover_rack =
        player_get_rack(game_get_player(s->base_game, s->inner_mover_idx));
    for (int ml = 0; ml < ld_size; ml++) {
      while (bag_get_letter(sub_bag, (MachineLetter)ml) > 0) {
        (void)bag_draw_letter(sub_bag, (MachineLetter)ml, 0);
      }
    }
    rack_reset(inner_mover_rack);
    for (int ml = 0; ml < ld_size; ml++) {
      int n = (int)outer_unseen[ml];
      if (ml == scenario_bag_tile) {
        n -= 1;
      }
      for (int i = 0; i < n; i++) {
        rack_add_letter(inner_mover_rack, (MachineLetter)ml);
      }
    }
    bag_add_letter(sub_bag, scenario_bag_tile, 0);
    game_set_player_on_turn_index(s->base_game, s->inner_mover_idx);
    bp_clear_false_game_end(s->base_game);
  }
  // Drop WMP on both players (inner solve uses pruned KWG path).
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    player_set_wmp(game_get_player(s->base_game, player_idx), NULL);
  }

  // Use the caller-provided pruned KWGs when available — the unplayed
  // pool is board-derived (same across all bag-tile scenarios), so the
  // pass cand pre-builds these once and shares across every scenario's
  // session. Otherwise build local ones (legacy / standalone path).
  bool shared_kwg =
      game_get_data_is_shared(s->base_game, PLAYERS_DATA_TYPE_KWG);
  if (s->dlm == DUAL_LEXICON_MODE_INFORMED && shared_kwg) {
    s->dlm = DUAL_LEXICON_MODE_IGNORANT;
  }
  if (shared_pruned_kwgs && shared_pruned_kwgs[0]) {
    s->pruned_kwgs[0] = shared_pruned_kwgs[0];
    s->pruned_kwgs[1] = shared_pruned_kwgs[1];
    s->kwgs_owned = false;
  } else {
    bool create_separate_kwgs =
        (s->dlm == DUAL_LEXICON_MODE_INFORMED) && !shared_kwg;
    s->pruned_kwgs[0] = NULL;
    s->pruned_kwgs[1] = NULL;
    for (int player_idx = 0; player_idx < (create_separate_kwgs ? 2 : 1);
         player_idx++) {
      const KWG *full_kwg =
          player_get_kwg(game_get_player(s->base_game, player_idx));
      DictionaryWordList *word_list = dictionary_word_list_create();
      generate_possible_words(s->base_game, full_kwg, word_list);
      s->pruned_kwgs[player_idx] = make_kwg_from_words_small(
          word_list, KWG_MAKER_OUTPUT_GADDAG, KWG_MAKER_MERGE_EXACT);
      dictionary_word_list_destroy(word_list);
    }
    s->kwgs_owned = true;
  }
  game_set_override_kwgs(s->base_game, s->pruned_kwgs[0], s->pruned_kwgs[1],
                         s->dlm);
  game_gen_all_cross_sets(s->base_game);

  // Inner scenario distribution: from inner mover's perspective, the unseen
  // tiles are the inner opp's full rack (= outer mover's rack) plus the bag
  // tile — same total pool as outer_unseen, but interpreted from the other
  // side. The distribution is structurally the same.
  for (int ml = 0; ml < ld_size; ml++) {
    s->inner_unseen[ml] = outer_unseen[ml];
  }
  s->inner_num_tile_types = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    if (s->inner_unseen[ml] > 0) {
      s->inner_tile_types[s->inner_num_tile_types] = (MachineLetter)ml;
      s->inner_tile_counts[s->inner_num_tile_types] = (int)s->inner_unseen[ml];
      s->inner_num_tile_types++;
    }
  }

  // Root movegen. Use the calling worker's idx for the per-thread movegen
  // cache key — concurrent workers must not share it.
  MoveList *initial_ml = move_list_create(BAI_PEG_MOVELIST_CAPACITY);
  const MoveGenArgs gen_args = {
      .game = s->base_game,
      .move_list = initial_ml,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .thread_index = worker_idx,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&gen_args);

  // Build cands: skip pass (disallow_pass behavior baked in).
  int n = initial_ml->count;
  s->cands = calloc_or_die((size_t)n, sizeof(BaiCand));
  int kept = 0;
  for (int ci = 0; ci < n; ci++) {
    const Move *m = initial_ml->moves[ci];
    if (move_get_type(m) == GAME_EVENT_PASS) {
      continue;
    }
    s->cands[kept].move_full = *m;
    const bool is_vert = (m->dir == BOARD_VERTICAL_DIRECTION);
    const int orig_row = is_vert ? m->col_start : m->row_start;
    const int orig_col = is_vert ? m->row_start : m->col_start;
    small_move_set_all(&s->cands[kept].move, m->tiles, 0, m->tiles_length - 1,
                       m->score, orig_row, orig_col, m->tiles_played, is_vert,
                       m->move_type);
    s->cands[kept].static_score = (int)equity_to_int(m->score);
    s->cands[kept].equity_for_prior = m->equity;
    kept++;
  }
  move_list_destroy(initial_ml);

  // Sort by movegen prior so the truncation below (when we don't run
  // greedy rerank) keeps the top-equity cands.
  qsort(s->cands, kept, sizeof(BaiCand), bp_compare_cands_by_prior);

  // Pre-stage post_cand_game for ALL kept cands so the greedy playout can
  // play from each. Anything pruned by the rerank gets its game freed.
  for (int ci = 0; ci < kept; ci++) {
    Game *g = game_duplicate(s->base_game);
    game_set_endgame_solving_mode(g);
    game_set_backup_mode(g, BACKUP_MODE_OFF);
    play_move_without_drawing_tiles(&s->cands[ci].move_full, g);
    bp_clear_false_game_end(g);
    s->cands[ci].post_cand_game = g;
  }

  // Greedy-playout rerank: for each (cand, inner_tile), draw the tile to
  // inner_opp's rack and run a flat highest-score playout to end-of-game.
  // The aggregated mover-perspective spread is a much better prior than
  // movegen equity, so the resulting top-`initial_top_k` is far more
  // likely to contain the depth-1-negamax winner than equity-sorted top-K
  // would be. q values written here are overwritten by the negamax pass
  // in bp_inner_session_advance(target_depth=1) — we only persist
  // depth_evaluated==0 so the advance loop runs.
  if (kept > 0 && s->inner_num_tile_types > 0) {
    // Greedy-playout rerank dispatched as work items to the outer
    // executor. Each (cand, inner_tile) item runs on a pool worker and
    // uses that worker's thread-local MoveGen, so the many
    // play_move + generate_moves iterations stay within the
    // num_workers-MoveGen pool rather than running inline in init.
    const int total_items = kept * s->inner_num_tile_types;
    BpInnerGreedyWork *works =
        calloc_or_die((size_t)total_items, sizeof(BpInnerGreedyWork));
    BpExecItem *items =
        malloc_or_die((size_t)total_items * sizeof(BpExecItem));
    int idx = 0;
    for (int ci = 0; ci < kept; ci++) {
      for (int ti = 0; ti < s->inner_num_tile_types; ti++) {
        works[idx].post_cand_game = s->cands[ci].post_cand_game;
        works[idx].unseen = s->inner_unseen;
        works[idx].ld_size = s->ld_size;
        works[idx].inner_mover_idx = s->inner_mover_idx;
        works[idx].inner_opp_idx = s->inner_opp_idx;
        works[idx].tile = s->inner_tile_types[ti];
        works[idx].weight = s->inner_tile_counts[ti];
        items[idx].fn = bp_inner_greedy_work_fn;
        items[idx].arg = &works[idx];
        items[idx].batch = NULL;
        idx++;
      }
    }
    if (outer->executor) {
      bp_exec_submit_and_wait(outer->executor, items, total_items, worker_idx);
    } else {
      for (int i = 0; i < total_items; i++) {
        bp_inner_greedy_work_fn(items[i].arg, worker_idx);
      }
    }
    // Aggregate per cand.
    for (int ci = 0; ci < kept; ci++) {
      int64_t spread_sum = 0;
      int64_t wins_x2 = 0;
      int64_t weight_sum = 0;
      for (int ti = 0; ti < s->inner_num_tile_types; ti++) {
        const int item_idx = ci * s->inner_num_tile_types + ti;
        const int tcnt = works[item_idx].weight;
        const int32_t mover_total = works[item_idx].out_mover_total;
        spread_sum += (int64_t)mover_total * tcnt;
        if (mover_total > 0) {
          wins_x2 += 2 * tcnt;
        } else if (mover_total == 0) {
          wins_x2 += tcnt;
        }
        weight_sum += tcnt;
      }
      if (weight_sum > 0) {
        s->cands[ci].q_mean_spread = (double)spread_sum / (double)weight_sum;
        s->cands[ci].q_win_pct = (double)wins_x2 / (2.0 * (double)weight_sum);
      }
      s->cands[ci].sort_utility =
          s->cands[ci].q_win_pct + 1e-4 * s->cands[ci].q_mean_spread;
    }
    free(works);
    free(items);

    // Sort by greedy utility, descending. Tiebreaker is raw spread.
    BaiCand **ptrs = malloc_or_die((size_t)kept * sizeof(BaiCand *));
    for (int ci = 0; ci < kept; ci++) {
      ptrs[ci] = &s->cands[ci];
    }
    qsort(ptrs, (size_t)kept, sizeof(BaiCand *),
          bp_compare_cand_ptrs_by_sort_utility);
    BaiCand *sorted = calloc_or_die((size_t)kept, sizeof(BaiCand));
    for (int ci = 0; ci < kept; ci++) {
      sorted[ci] = *ptrs[ci];
    }
    free(ptrs);
    free(s->cands);
    s->cands = sorted;
  }

  // Truncate to inner top_k. Free the post_cand_games of the dropped cands.
  if (initial_top_k > 0 && kept > initial_top_k) {
    for (int ci = initial_top_k; ci < kept; ci++) {
      if (s->cands[ci].post_cand_game) {
        game_destroy(s->cands[ci].post_cand_game);
        s->cands[ci].post_cand_game = NULL;
      }
    }
    kept = initial_top_k;
  }
  s->num_cands = kept;

  // Reset depth_evaluated/visits on retained cands so advance(1) still runs
  // negamax (otherwise the greedy q would mark them as already evaluated at
  // depth=1 and skip). q values are kept around as a fallback for any cand
  // that never gets a negamax visit due to budget exhaustion.
  for (int ci = 0; ci < s->num_cands; ci++) {
    s->cands[ci].depth_evaluated = 0;
    s->cands[ci].visits = 0;
  }

  s->initialized = true;
}

// Deepen every retained cand to depth >= target_depth, skipping work done in
// prior advance() calls. Each step calls bp_evaluate_cand at the next depth,
// which itself uses args->executor when set (recursive work-stealing).
static void bp_inner_session_advance(BpInnerSession *s, int target_depth,
                                     ThreadControl *thread_control,
                                     double endgame_time_per_solve,
                                     int worker_idx, BaiPegExecutor *executor,
                                     int64_t external_deadline_ns) {
  if (!s->initialized) {
    return;
  }
  if (target_depth > BAI_PEG_MAX_DEPTH) {
    target_depth = BAI_PEG_MAX_DEPTH;
  }
  for (int ci = 0; ci < s->num_cands; ci++) {
    while (s->cands[ci].depth_evaluated < target_depth) {
      int next_d = s->cands[ci].depth_evaluated + 1;
      bp_evaluate_cand(
          &s->cands[ci], next_d, s->inner_mover_idx, s->inner_opp_idx,
          s->inner_unseen, s->ld_size, s->inner_tile_types,
          s->inner_tile_counts, s->inner_num_tile_types, thread_control,
          /*per_thread_tts=*/NULL, s->dlm, endgame_time_per_solve,
          /*num_threads=*/1, worker_idx, external_deadline_ns, executor);
    }
  }
}

// Pick the inner cand with the highest utility. utility_alpha follows
// outer's preference for tiebreaking (small alpha = pure win% with spread
// tiebreak).
static int bp_inner_session_pick_best(const BpInnerSession *s,
                                      double utility_alpha) {
  int best = -1;
  double best_u = 0.0;
  int best_d = -1;
  for (int ci = 0; ci < s->num_cands; ci++) {
    if (s->cands[ci].depth_evaluated == 0) {
      continue;
    }
    double u =
        s->cands[ci].q_win_pct + utility_alpha * s->cands[ci].q_mean_spread;
    int d = s->cands[ci].depth_evaluated;
    if (best < 0 || d > best_d || (d == best_d && u > best_u)) {
      best = ci;
      best_u = u;
      best_d = d;
    }
  }
  return best;
}

static void bp_inner_session_destroy(BpInnerSession *s) {
  if (!s->initialized) {
    return;
  }
  for (int ci = 0; ci < s->num_cands; ci++) {
    if (s->cands[ci].post_cand_game) {
      game_destroy(s->cands[ci].post_cand_game);
    }
  }
  free(s->cands);
  if (s->kwgs_owned) {
    if (s->pruned_kwgs[0]) {
      kwg_destroy(s->pruned_kwgs[0]);
    }
    if (s->pruned_kwgs[1]) {
      kwg_destroy(s->pruned_kwgs[1]);
    }
  }
  game_destroy(s->base_game);
  s->initialized = false;
}

// ---------------------------------------------------------------------------
// Pass evaluation
// ---------------------------------------------------------------------------

// One per-scenario unit of work: build the sub-game, run opp's recursive
// PEG, decide opt-A vs opt-B, optionally apply M_opp + run mover's endgame,
// and store the resulting mover_total along with logging fields.
//
// Designed to be self-contained so a pool of executor workers can run them
// concurrently — each scenario allocates its own EndgameCtx / Game on
// demand and frees them before returning.
typedef struct BpPassScenario {
  // Inputs.
  const BaiPegArgs *args;
  int mover_idx;
  int opp_idx;
  const uint8_t *unseen;
  int ld_size;
  MachineLetter bag_tile;
  int weight;
  int depth;
  bool use_pinned_opp_depth;
  int pinned_opp_depth;
  double per_scenario_time;
  const LetterDistribution *ld;
  // Coupled inner session for THIS scenario_tile (lifetime owned by the
  // outer pass cand). NULL = no coupling (legacy path: build + tear down a
  // fresh inner sub-solve every visit). When set, deepen incrementally.
  BpInnerSession *inner_session;
  int initial_top_k; // Used when initializing the coupled session.
  // Pre-built pruned KWGs from the pass cand (board-derived, identical
  // across all scenarios). Inner session uses these instead of pruning
  // its own; NULL slot 0 means inner session prunes itself.
  KWG *shared_pruned_kwgs[2];
  // Outputs.
  bool sub_ok;
  bool found_tile_play;
  bool opp_chose_pass;
  int32_t opp_pass_spread;
  double opp_pass_win;
  double opp_play_win;
  double opp_play_spread;
  int32_t mover_total;
  int opp_tiles_played;
  int opp_evals;
  double opp_seconds;
  char move_str[64]; // formatted opp move text for logging
} BpPassScenario;

// Resolve M_opp + opp's perceived stats either via a coupled inner session
// (incremental deepening, persistent across mover-pass-revisits) or by
// running a fresh bai_peg_solve sub-call (legacy path). On return:
//   *sub_game_out   — points to a Game* the caller MAY own (see
//                     *owns_sub_game_out). Always valid for reads.
//   *m_opp_small_out — opp's chosen small move (set as pass on no result).
// Other s->* output fields are populated.
static void bp_pass_scenario_resolve_m_opp(BpPassScenario *s, int worker_idx,
                                           Game **sub_game_out,
                                           bool *owns_sub_game_out,
                                           SmallMove *m_opp_small_out) {
  const BaiPegArgs *args = s->args;
  if (s->inner_session) {
    BpInnerSession *isess = s->inner_session;
    Timer t;
    ctimer_start(&t);
    if (!isess->initialized) {
      Timer init_t;
      ctimer_start(&init_t);
      int top_k =
          s->initial_top_k > 0 ? s->initial_top_k : BAI_PEG_DEFAULT_TOP_K;
      bp_inner_session_init(isess, args, args->game, s->mover_idx, s->unseen,
                            s->bag_tile, s->ld_size, top_k, worker_idx,
                            s->shared_pruned_kwgs);
      double init_secs = ctimer_elapsed_seconds(&init_t);
      if (args->log_solve_details) {
        fprintf(stderr,
                "      [bp.scen.init] tile=%d cands=%d worker=%d %.2fs\n",
                (int)s->bag_tile, isess->num_cands, worker_idx, init_secs);
        fflush(stderr);
      }
    }
    // In coupled mode opp's depth tracks mover's PUCT eval depth — the
    // pinned diagnostic knob is intentionally ignored here so each
    // mover-pass-visit deepens opp's tree exactly one ply at a time.
    int target_depth = s->depth;
    if (target_depth > BAI_PEG_MAX_DEPTH) {
      target_depth = BAI_PEG_MAX_DEPTH;
    }
    Timer adv_t;
    ctimer_start(&adv_t);
    bp_inner_session_advance(isess, target_depth, args->thread_control,
                             args->endgame_time_per_solve, worker_idx,
                             args->executor,
                             /*external_deadline_ns=*/0);
    if (args->log_solve_details) {
      double adv_secs = ctimer_elapsed_seconds(&adv_t);
      fprintf(stderr, "      [bp.scen.advance] tile=%d depth=%d %.2fs\n",
              (int)s->bag_tile, target_depth, adv_secs);
      fflush(stderr);
    }
    s->opp_seconds = ctimer_elapsed_seconds(&t);
    int evals = 0;
    for (int ci = 0; ci < isess->num_cands; ci++) {
      evals += isess->cands[ci].depth_evaluated;
    }
    s->opp_evals = evals;
    s->sub_ok = isess->num_cands > 0;
    int best_idx = bp_inner_session_pick_best(isess, args->utility_alpha);
    if (best_idx >= 0) {
      *m_opp_small_out = isess->cands[best_idx].move;
      s->opp_play_win = isess->cands[best_idx].q_win_pct;
      s->opp_play_spread = isess->cands[best_idx].q_mean_spread;
    } else {
      small_move_set_as_pass(m_opp_small_out);
      s->opp_play_win = 0.0;
      s->opp_play_spread = 0.0;
    }
    s->found_tile_play = !small_move_is_pass(m_opp_small_out);
    *sub_game_out = isess->base_game;
    *owns_sub_game_out = false;
    return;
  }

  // Legacy path: build sub-game + run bai_peg_solve to completion.
  Game *sub_game = game_duplicate(args->game);
  {
    Bag *sub_bag = game_get_bag(sub_game);
    Rack *sub_opp_rack = player_get_rack(game_get_player(sub_game, s->opp_idx));
    for (int ml = 0; ml < s->ld_size; ml++) {
      while (bag_get_letter(sub_bag, (MachineLetter)ml) > 0) {
        (void)bag_draw_letter(sub_bag, (MachineLetter)ml, 0);
      }
    }
    rack_reset(sub_opp_rack);
    for (int ml = 0; ml < s->ld_size; ml++) {
      int n = (int)s->unseen[ml];
      if (ml == s->bag_tile) {
        n -= 1;
      }
      for (int i = 0; i < n; i++) {
        rack_add_letter(sub_opp_rack, (MachineLetter)ml);
      }
    }
    bag_add_letter(sub_bag, s->bag_tile, 0);
    game_set_player_on_turn_index(sub_game, s->opp_idx);
    bp_clear_false_game_end(sub_game);
  }

  BaiPegArgs sub = *args;
  sub.game = sub_game;
  sub.include_pass = false;
  sub.disallow_pass = true;
  sub.max_depth = s->use_pinned_opp_depth ? s->pinned_opp_depth : s->depth;
  sub.time_budget_seconds = s->use_pinned_opp_depth
                                ? args->pass_opp_time_per_scenario
                                : s->per_scenario_time;
  sub.log_solve_details = false;
  sub.thread_index_offset = worker_idx;
  sub.shared_tt = NULL;
  sub.shared_tt_per_thread = NULL;
  sub.tt_fraction_of_mem = 0.0;
  sub.request_cand_stats = false;
  sub.progress_callback = NULL;
  sub.progress_callback_data = NULL;
  sub.sweep_max_depth = 0;
  sub.pass_opp_max_depth = 0;
  sub.num_threads = 1;
  sub.executor = args->executor;

  BaiPegResult sub_result;
  ErrorStack *sub_es = error_stack_create();
  bai_peg_solve(&sub, &sub_result, sub_es);
  s->sub_ok = error_stack_is_empty(sub_es);
  s->opp_evals = s->sub_ok ? sub_result.evaluations_done : 0;
  s->opp_seconds = s->sub_ok ? sub_result.seconds_elapsed : 0.0;
  if (s->sub_ok) {
    *m_opp_small_out = sub_result.best_move;
    s->opp_play_win = sub_result.best_win_pct;
    s->opp_play_spread = sub_result.best_mean_spread;
  } else {
    small_move_set_as_pass(m_opp_small_out);
    s->opp_play_win = 0.0;
    s->opp_play_spread = 0.0;
    error_stack_reset(sub_es);
  }
  s->found_tile_play = !small_move_is_pass(m_opp_small_out);
  bai_cand_stats_free(sub_result.cand_stats);
  error_stack_destroy(sub_es);
  *sub_game_out = sub_game;
  *owns_sub_game_out = true;
}

static void bp_pass_scenario_eval(BpPassScenario *s, int worker_idx) {
  const BaiPegArgs *args = s->args;

  Game *sub_game = NULL;
  bool owns_sub_game = false;
  SmallMove m_opp_small;
  bp_pass_scenario_resolve_m_opp(s, worker_idx, &sub_game, &owns_sub_game,
                                 &m_opp_small);

  // Option A: both pass → 2-pass terminal with rack penalties. Computed
  // from sub_game (which has racks/scores in the right state regardless of
  // path: both legacy build and inner-session base_game keep mover's rack
  // and opp's rack as 7 each, opp on turn).
  int32_t cur_spread =
      equity_to_int(player_get_score(game_get_player(sub_game, s->mover_idx))) -
      equity_to_int(player_get_score(game_get_player(sub_game, s->opp_idx)));
  int32_t mover_rack_pts = (int32_t)equity_to_int(rack_get_score(
      s->ld, player_get_rack(game_get_player(sub_game, s->mover_idx))));
  int32_t opp_rack_pts = (int32_t)equity_to_int(rack_get_score(
      s->ld, player_get_rack(game_get_player(sub_game, s->opp_idx))));
  int32_t mover_total_if_both_pass = cur_spread - mover_rack_pts + opp_rack_pts;
  s->opp_pass_spread = -mover_total_if_both_pass;
  if (s->opp_pass_spread > 0) {
    s->opp_pass_win = 1.0;
  } else if (s->opp_pass_spread == 0) {
    s->opp_pass_win = 0.5;
  } else {
    s->opp_pass_win = 0.0;
  }

  Move m_opp_full;
  small_move_to_move(&m_opp_full, &m_opp_small, game_get_board(sub_game));

  // Format move text for the log line — capture now while sub_game is live.
  s->move_str[0] = '\0';
  if (args->log_solve_details) {
    StringBuilder *sb = string_builder_create();
    if (s->found_tile_play) {
      string_builder_add_move(sb, game_get_board(sub_game), &m_opp_full, s->ld,
                              /*add_score=*/true);
    } else {
      string_builder_add_string(sb, s->sub_ok ? "(Pass)" : "(error)");
    }
    const char *peek = string_builder_peek(sb);
    size_t len = strlen(peek);
    if (len >= sizeof(s->move_str)) {
      len = sizeof(s->move_str) - 1;
    }
    memcpy(s->move_str, peek, len);
    s->move_str[len] = '\0';
    string_builder_destroy(sb);
  }

  // Mirror old peg.c's tiebreaker: prefer higher win%, then higher spread.
  s->opp_chose_pass = !s->found_tile_play ||
                      s->opp_pass_win > s->opp_play_win + 1e-9 ||
                      (s->opp_pass_win >= s->opp_play_win - 1e-9 &&
                       (double)s->opp_pass_spread > s->opp_play_spread);

  s->opp_tiles_played = 0;
  if (s->opp_chose_pass) {
    s->mover_total = mover_total_if_both_pass;
  } else {
    Game *post_opp = game_duplicate(sub_game);
    game_set_endgame_solving_mode(post_opp);
    game_set_backup_mode(post_opp, BACKUP_MODE_OFF);
    s->opp_tiles_played = move_get_tiles_played(&m_opp_full);
    play_move(&m_opp_full, post_opp, NULL);
    bp_clear_false_game_end(post_opp);

    int32_t mover_lead =
        equity_to_int(
            player_get_score(game_get_player(post_opp, s->mover_idx))) -
        equity_to_int(player_get_score(game_get_player(post_opp, s->opp_idx)));

    if (game_get_game_end_reason(post_opp) != GAME_END_REASON_NONE) {
      s->mover_total = mover_lead;
    } else {
      double mover_eg_cap = args->endgame_time_per_solve > 0.0
                                ? args->endgame_time_per_solve
                                : 0.0;
      // endgame_solve_inline runs in the calling thread and does not
      // spawn the external timer thread, so soft/hard time limits go
      // unenforced mid-search. external_deadline_ns is checked by
      // abdada_negamax's depth-deadline path and gives us a real
      // wall-clock cap. Without this, post-bingo "stuck" endgames
      // (mover with 1 tile vs opp with 7 + unplayable letters) can
      // run for many minutes per scenario.
      int64_t deadline_ns =
          mover_eg_cap > 0.0
              ? ctimer_monotonic_ns() + (int64_t)(mover_eg_cap * 1.0e9)
              : 0;
      EndgameCtx *eg_ctx = NULL;
      EndgameResults *eg_results = endgame_results_create();
      Timer eg_t;
      ctimer_start(&eg_t);
      EndgameArgs ea = {
          .thread_control = args->thread_control,
          .game = post_opp,
          .plies = BAI_PEG_MAX_DEPTH,
          .initial_small_move_arena_size =
              DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
          .num_threads = 1,
          .use_heuristics = true,
          .num_top_moves = 1,
          .dual_lexicon_mode = args->dual_lexicon_mode,
          .skip_word_pruning = true,
          .thread_index_offset = worker_idx,
          .soft_time_limit = mover_eg_cap,
          .hard_time_limit = mover_eg_cap,
          .external_deadline_ns = deadline_ns,
      };
      endgame_solve_inline(&eg_ctx, &ea, eg_results);
      int eg_val = endgame_results_get_value(eg_results, ENDGAME_RESULT_BEST);
      int eg_depth = endgame_results_get_depth(eg_results, ENDGAME_RESULT_BEST);
      s->mover_total = mover_lead + eg_val;
      double eg_secs = ctimer_elapsed_seconds(&eg_t);
      if (args->log_solve_details) {
        fprintf(stderr,
                "      [bp.scen.eg] tile=%d eg_depth=%d eg_secs=%.2fs "
                "mover_total=%+d\n",
                (int)s->bag_tile, eg_depth, eg_secs, (int)s->mover_total);
        fflush(stderr);
      }
      endgame_results_destroy(eg_results);
      endgame_ctx_destroy(eg_ctx);
    }
    game_destroy(post_opp);
  }

  if (owns_sub_game) {
    game_destroy(sub_game);
  }
}

static void bp_pass_scenario_work_fn(void *arg, int worker_idx) {
  bp_pass_scenario_eval((BpPassScenario *)arg, worker_idx);
}

// Evaluate the pass candidate at depth N. For each scenario (one per
// unseen tile type, weighted by multiplicity) we model the world as we
// know it: bag = {bag_tile}, opp_rack = unseen - {bag_tile}, mover keeps
// its actual rack. We then:
//
//   1. Run a one-level-deep recursive bai_peg_solve from opp's POV
//      (opp's perceived 1peg under their bag-tile uncertainty) to pick
//      M_opp. The sub-call has disallow_pass=true so opp's pool excludes
//      pass and we never recurse further.
//   2. Apply M_opp in OUR scenario (with the known bag tile). If M_opp
//      is pass, both sides have passed in a row → standard 2-pass
//      terminal with rack penalties on both. Otherwise opp plays K≥1
//      tiles, draws the lone bag tile, and the position becomes a
//      true endgame on mover's turn.
//   3. Solve mover's endgame to convergence (plies = MAX_SEARCH_DEPTH)
//      and read the actual outcome.
//
// The result is the average of these actual outcomes, weighted by tile
// multiplicity — NOT the negation of opp's perceived expected value.
static void bp_evaluate_pass(BaiCand *cand, int depth, const BaiPegArgs *args,
                             int mover_idx, int opp_idx, const uint8_t *unseen,
                             int ld_size, const MachineLetter *tile_types,
                             const int *tile_counts, int num_tile_types,
                             double per_solve_time) {
  int total_weight = 0;
  for (int ti = 0; ti < num_tile_types; ti++) {
    total_weight += tile_counts[ti];
  }
  if (total_weight == 0) {
    return;
  }

  // Lazy-build the pass cand's pruned KWGs once. The board determines the
  // unplayed pool, so this is identical across all scenario_bag_tile values
  // — every coupled inner session reuses these pointers, replacing what
  // used to be N independent prunes per pass eval.
  if (!cand->cached_pruned_kwgs[0]) {
    bool shared_kwg =
        game_get_data_is_shared(args->game, PLAYERS_DATA_TYPE_KWG);
    dual_lexicon_mode_t dlm = args->dual_lexicon_mode;
    if (dlm == DUAL_LEXICON_MODE_INFORMED && shared_kwg) {
      dlm = DUAL_LEXICON_MODE_IGNORANT;
    }
    bool create_separate_kwgs =
        (dlm == DUAL_LEXICON_MODE_INFORMED) && !shared_kwg;
    for (int player_idx = 0; player_idx < (create_separate_kwgs ? 2 : 1);
         player_idx++) {
      const KWG *full_kwg =
          player_get_kwg(game_get_player(args->game, player_idx));
      DictionaryWordList *word_list = dictionary_word_list_create();
      generate_possible_words(args->game, full_kwg, word_list);
      cand->cached_pruned_kwgs[player_idx] = make_kwg_from_words_small(
          word_list, KWG_MAKER_OUTPUT_GADDAG, KWG_MAKER_MERGE_EXACT);
      dictionary_word_list_destroy(word_list);
    }
  }

  int64_t spread_sum = 0;
  int64_t wins_x2 = 0;
  int64_t weight_sum = 0;
  // When pass_opp_max_depth is set, depth gates the inner sub-solve and
  // there is no time cap (opp gets to actually reach the requested depth).
  // Otherwise we slice per_solve_time across scenarios as a cheap warmup.
  bool use_pinned_opp_depth = args->pass_opp_max_depth > 0;
  int pinned_opp_depth = args->pass_opp_max_depth;
  double per_scenario_time = 0.0;
  if (!use_pinned_opp_depth) {
    per_scenario_time =
        per_solve_time > 0.0 ? per_solve_time / num_tile_types : 0.0;
  }

  const LetterDistribution *ld = game_get_ld(args->game);

  // Lazy-allocate the cand's coupled inner sessions array on first visit.
  // The array length matches num_tile_types and indexes match `tile_types`.
  // Caller (bai_peg_solve cleanup at end) frees them when the cand is torn
  // down.
  if (!cand->coupled_inner_sessions) {
    cand->coupled_inner_sessions =
        calloc_or_die((size_t)num_tile_types, sizeof(BpInnerSession));
    cand->coupled_count = num_tile_types;
  }

  // Per-scenario work (one entry per distinct unseen tile type). Either
  // run sequentially (legacy / no executor) or submitted as a batch to the
  // executor. Aggregation happens after all entries are filled.
  BpPassScenario *scenarios =
      calloc_or_die((size_t)num_tile_types, sizeof(BpPassScenario));
  for (int ti = 0; ti < num_tile_types; ti++) {
    BpPassScenario *s = &scenarios[ti];
    s->args = args;
    s->mover_idx = mover_idx;
    s->opp_idx = opp_idx;
    s->unseen = unseen;
    s->ld_size = ld_size;
    s->bag_tile = tile_types[ti];
    s->weight = tile_counts[ti];
    s->depth = depth;
    s->use_pinned_opp_depth = use_pinned_opp_depth;
    s->pinned_opp_depth = pinned_opp_depth;
    s->per_scenario_time = per_scenario_time;
    s->ld = ld;
    s->inner_session = &cand->coupled_inner_sessions[ti];
    s->initial_top_k = args->inner_initial_top_k > 0
                           ? args->inner_initial_top_k
                           : args->initial_top_k;
    s->shared_pruned_kwgs[0] = cand->cached_pruned_kwgs[0];
    s->shared_pruned_kwgs[1] = cand->cached_pruned_kwgs[1];
  }

  if (args->executor) {
    BpExecItem *items =
        malloc_or_die((size_t)num_tile_types * sizeof(BpExecItem));
    for (int ti = 0; ti < num_tile_types; ti++) {
      items[ti].fn = bp_pass_scenario_work_fn;
      items[ti].arg = &scenarios[ti];
      items[ti].batch = NULL; // bp_exec_submit_and_wait sets this.
    }
    bp_exec_submit_and_wait(args->executor, items, num_tile_types,
                            args->thread_index_offset);
    free(items);
  } else {
    for (int ti = 0; ti < num_tile_types; ti++) {
      bp_pass_scenario_eval(&scenarios[ti], args->thread_index_offset);
    }
  }

  // Aggregate results and log.
  for (int ti = 0; ti < num_tile_types; ti++) {
    const BpPassScenario *s = &scenarios[ti];
    spread_sum += (int64_t)s->mover_total * s->weight;
    int win_contrib;
    if (s->mover_total > 0) {
      win_contrib = 2 * s->weight;
    } else if (s->mover_total == 0) {
      win_contrib = s->weight;
    } else {
      win_contrib = 0;
    }
    wins_x2 += win_contrib;
    weight_sum += s->weight;

    if (args->log_solve_details) {
      fprintf(stderr,
              "    [bp.pass] bag=%s w=%d depth=%d "
              "optA(both_pass): opp_spread=%+d opp_win=%.2f | "
              "optB(play): %s opp_win=%.4f opp_spread=%+0.2f K=%d "
              "[opp_evals=%d t=%.2fs] | "
              "opp_chose=%s -> mover_total=%+d mover_win=%s\n",
              ld->ld_ml_to_hl[s->bag_tile], s->weight, s->depth,
              (int)s->opp_pass_spread, s->opp_pass_win, s->move_str,
              s->opp_play_win, s->opp_play_spread, s->opp_tiles_played,
              s->opp_evals, s->opp_seconds, s->opp_chose_pass ? "PASS" : "PLAY",
              (int)s->mover_total,
              s->mover_total > 0 ? "yes"
                                 : (s->mover_total == 0 ? "tie" : "no"));
      fflush(stderr);
    }
  }

  free(scenarios);

  if (weight_sum > 0) {
    cand->q_mean_spread = (double)spread_sum / (double)weight_sum;
    cand->q_win_pct = (double)wins_x2 / (2.0 * (double)weight_sum);
  }
  cand->depth_evaluated = depth;
  cand->visits++;
}

// ---------------------------------------------------------------------------
// Main solver
// ---------------------------------------------------------------------------

void bai_peg_solve(const BaiPegArgs *args, BaiPegResult *result,
                   ErrorStack *error_stack) {
  memset(result, 0, sizeof(*result));

  const LetterDistribution *ld = game_get_ld(args->game);
  int ld_size = ld_get_size(ld);
  int mover_idx = game_get_player_on_turn_index(args->game);
  int opp_idx = 1 - mover_idx;
  int num_threads = args->num_threads > 0 ? args->num_threads : 1;
  int max_depth = args->max_depth > 0 ? args->max_depth : BAI_PEG_MAX_DEPTH;
  if (max_depth > BAI_PEG_MAX_DEPTH) {
    max_depth = BAI_PEG_MAX_DEPTH;
  }
  int initial_top_k =
      args->initial_top_k > 0 ? args->initial_top_k : BAI_PEG_DEFAULT_TOP_K;
  double puct_c = args->puct_c > 0.0 ? args->puct_c : 1.0;
  const double alpha = args->utility_alpha;

  // Accept any CGP that, from the mover's perspective, has exactly
  // RACK_SIZE+1 unseen tiles and a full mover rack: this is the canonical
  // 1-in-bag PEG state regardless of how the CGP partitions the unseen
  // tiles between opp's rack and the bag (CGPs from analyzers commonly
  // report opp_rack as empty since the opponent's tiles aren't knowable).
  // The inner scenario loop will redistribute the unseen pool itself.
  const Rack *mover_rack_check =
      player_get_rack(game_get_player(args->game, mover_idx));
  if (rack_get_total_letters(mover_rack_check) != RACK_SIZE) {
    error_stack_push(
        error_stack, ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY,
        get_formatted_string(
            "bai_peg_solve requires a full mover rack (%d tiles); got %d",
            RACK_SIZE, (int)rack_get_total_letters(mover_rack_check)));
    return;
  }
  uint8_t unseen[MAX_ALPHABET_SIZE];
  int total_unseen = bp_compute_unseen(args->game, mover_idx, unseen);
  if (total_unseen != RACK_SIZE + 1) {
    error_stack_push(
        error_stack, ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY,
        get_formatted_string("bai_peg_solve requires %d unseen tiles "
                             "(mover full rack + 1 in bag); got %d",
                             RACK_SIZE + 1, total_unseen));
    return;
  }

  // Build base game with empty bag and WMP disabled. generate_possible_words
  // derives its unplayed pool from the board alone, so we don't need to
  // pre-stage racks or the bag in any particular way before pruning. We
  // do drain the bag here (transferring tiles to a side that we'll
  // immediately reset) so the post-cand endgame search runs on a true
  // bag-empty position.
  Game *base_game = game_duplicate(args->game);
  {
    Bag *base_bag = game_get_bag(base_game);
    for (int ml = 0; ml < ld_size; ml++) {
      while (bag_get_letter(base_bag, ml) > 0) {
        bag_draw_letter(base_bag, (MachineLetter)ml, opp_idx);
      }
    }
    Rack *opp_rack = player_get_rack(game_get_player(base_game, opp_idx));
    rack_reset(opp_rack);
  }
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    player_set_wmp(game_get_player(base_game, player_idx), NULL);
  }

  // Build pruned KWGs (single ignorant copy) and set as override.
  bool shared_kwg = game_get_data_is_shared(args->game, PLAYERS_DATA_TYPE_KWG);
  dual_lexicon_mode_t dlm = args->dual_lexicon_mode;
  if (dlm == DUAL_LEXICON_MODE_INFORMED && shared_kwg) {
    dlm = DUAL_LEXICON_MODE_IGNORANT;
  }
  bool create_separate_kwgs =
      (dlm == DUAL_LEXICON_MODE_INFORMED) && !shared_kwg;
  KWG *pruned_kwgs[2] = {NULL, NULL};
  for (int player_idx = 0; player_idx < (create_separate_kwgs ? 2 : 1);
       player_idx++) {
    const KWG *full_kwg =
        player_get_kwg(game_get_player(base_game, player_idx));
    DictionaryWordList *word_list = dictionary_word_list_create();
    generate_possible_words(base_game, full_kwg, word_list);
    pruned_kwgs[player_idx] = make_kwg_from_words_small(
        word_list, KWG_MAKER_OUTPUT_GADDAG, KWG_MAKER_MERGE_EXACT);
    dictionary_word_list_destroy(word_list);
  }
  game_set_override_kwgs(base_game, pruned_kwgs[0], pruned_kwgs[1], dlm);
  game_gen_all_cross_sets(base_game);

  // Root movegen uses full Move records (not SmallMove) and MOVE_SORT_EQUITY
  // so each cand carries movegen's pre-computed equity (= score + KLV leave
  // value). We use that equity as the prior softmax basis, which empirically
  // beats score-only by ~0.4pp EmpW% / +0.37 spread on a 1000-position 1s
  // benchmark (95 vs 73 H2H wins among 174 differing picks; see commit
  // message for sweep details).
  MoveList *initial_ml = move_list_create(BAI_PEG_MOVELIST_CAPACITY);
  {
    const MoveGenArgs gen_args = {
        .game = base_game,
        .move_list = initial_ml,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .thread_index = args->thread_index_offset,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&gen_args);
  }

  int num_candidates = initial_ml->count;
  if (num_candidates == 0) {
    // movegen normally returns at least a pass; defensive fallback if it
    // somehow doesn't. Same handling as the "no scoring plays" case below.
    move_list_destroy(initial_ml);
    if (pruned_kwgs[0]) {
      kwg_destroy(pruned_kwgs[0]);
    }
    if (pruned_kwgs[1]) {
      kwg_destroy(pruned_kwgs[1]);
    }
    game_destroy(base_game);
    small_move_set_as_pass(&result->best_move);
    result->best_win_pct = 0.0;
    result->best_mean_spread = 0.0;
    result->best_depth_evaluated = 0;
    result->candidates_considered = 0;
    return;
  }

  // Build candidate array. Pass is added (with an artificial prior basis)
  // when args->include_pass is set and args->disallow_pass is not — pass
  // is then evaluated via a one-level recursive bai_peg_solve from opp's
  // perspective. Without include_pass, pass is filtered out and the
  // solver picks among the tile-play moves only.
  // Reserve one extra slot for pass.
  BaiCand *cands = calloc_or_die(num_candidates + 1, sizeof(BaiCand));
  int kept = 0;
  bool pass_added = false;
  for (int ci = 0; ci < num_candidates; ci++) {
    const Move *m = initial_ml->moves[ci];
    if (move_get_type(m) == GAME_EVENT_PASS) {
      if (args->include_pass && !args->disallow_pass && !pass_added) {
        // Add pass as a real candidate with the artificial prior basis.
        const KLV *klv = player_get_klv(game_get_player(args->game, mover_idx));
        const Rack *mover_rack_full =
            player_get_rack(game_get_player(args->game, mover_idx));
        Equity pass_basis =
            bp_pass_artificial_prior(klv, mover_rack_full, ld_size);
        cands[kept].move_full = *m;
        small_move_set_as_pass(&cands[kept].move);
        cands[kept].static_score = 0;
        cands[kept].equity_for_prior = pass_basis;
        cands[kept].is_pass = true;
        if (args->log_solve_details) {
          fprintf(stderr,
                  "  [bp] pass cand prior basis (avg 6-leave KLV): %.4f\n",
                  equity_to_double(pass_basis));
          fflush(stderr);
        }
        pass_added = true;
        kept++;
      }
      continue;
    }
    cands[kept].move_full = *m;
    // Build SmallMove from the full Move so callers (result.best_move,
    // progress callback, cand_stats) get the compact form. Movegen
    // pre-swaps row_start/col_start for vertical moves to match the
    // SmallMove encoding's own swap (set_play_for_record at
    // move_gen.c:210-213); we have to undo that swap before calling
    // small_move_set_all (which will re-apply it).
    const bool is_vert = (m->dir == BOARD_VERTICAL_DIRECTION);
    const int orig_row = is_vert ? m->col_start : m->row_start;
    const int orig_col = is_vert ? m->row_start : m->col_start;
    small_move_set_all(&cands[kept].move, m->tiles, 0, m->tiles_length - 1,
                       m->score, orig_row, orig_col, m->tiles_played, is_vert,
                       m->move_type);
    cands[kept].static_score = (int)equity_to_int(m->score);
    cands[kept].equity_for_prior = m->equity;
    kept++;
  }
  move_list_destroy(initial_ml);
  num_candidates = kept;
  if (num_candidates == 0) {
    // No scoring plays from this rack. bai_peg can't search the pass branch
    // (no recursion into the post-pass position), so fall back to pass-with-
    // score-0 as the best move and return success. The caller decides what
    // to do; for the CLI we simply surface the pass.
    free(cands);
    if (pruned_kwgs[0]) {
      kwg_destroy(pruned_kwgs[0]);
    }
    if (pruned_kwgs[1]) {
      kwg_destroy(pruned_kwgs[1]);
    }
    game_destroy(base_game);
    small_move_set_as_pass(&result->best_move);
    result->best_win_pct = 0.0;
    result->best_mean_spread = 0.0;
    result->best_depth_evaluated = 0;
    result->candidates_considered = 0;
    return;
  }

  qsort(cands, num_candidates, sizeof(BaiCand), bp_compare_cands_by_prior);
  // Progressive widening keeps ALL generated moves in the pool — the active
  // subset grows from a small seed as PUCT accumulates visits, so weak moves
  // don't get touched at short budgets.
  if (!args->progressive_widening && num_candidates > initial_top_k) {
    num_candidates = initial_top_k;
  }

  // Compute softmax priors over each cand's equity_for_prior (= score +
  // KLV leave value). Cands are already sorted in basis-descending order
  // by qsort above. Temperature scaled so the gap between top moves
  // doesn't fully starve the rest.
  {
    double T = 25.0; // points per "natural" temperature unit
    double max_basis = equity_to_double(cands[0].equity_for_prior);
    double sum = 0.0;
    for (int ci = 0; ci < num_candidates; ci++) {
      double basis = equity_to_double(cands[ci].equity_for_prior);
      double s = exp((basis - max_basis) / T);
      cands[ci].prior = s; // store unnormalized for now
      sum += s;
    }
    for (int ci = 0; ci < num_candidates; ci++) {
      cands[ci].prior /= sum;
    }
  }

  // Pre-stage post-candidate games (each candidate's board + cross-sets
  // after its move is played, opp rack untouched). With *just* progressive
  // widening (no initial_playout, no min_active), most candidates never
  // get visited, so we leave post_cand_game = NULL and create it lazily on
  // first PUCT visit. With initial_playout, every cand gets a Phase 0
  // visit. With min_active, the top-N cands get a Phase 1 d=1 warmup. In
  // both cases we must pre-stage at least those cands.
  // Number of cands to eagerly pre-stage. The rest get lazy creation on
  // first PUCT visit (saves duplicating thousands of unused Game copies).
  int prestage_count = num_candidates;
  if (args->progressive_widening && !args->initial_playout) {
    if (args->min_active > 0) {
      prestage_count = (args->min_active < num_candidates) ? args->min_active
                                                           : num_candidates;
    } else {
      prestage_count = 0; // pure pwn: lazy create only when admitted.
    }
  }
  for (int ci = 0; ci < num_candidates; ci++) {
    if (ci < prestage_count) {
      Game *g = game_duplicate(base_game);
      game_set_endgame_solving_mode(g);
      game_set_backup_mode(g, BACKUP_MODE_OFF);
      play_move_without_drawing_tiles(&cands[ci].move_full, g);
      bp_clear_false_game_end(g);
      cands[ci].post_cand_game = g;
    } else {
      cands[ci].post_cand_game = NULL;
    }
    // Structural prior on the cost of this candidate's first eval (depth 1).
    // Replaced by measured time after the warm-up pass.
    cands[ci].cost_estimate = bp_initial_cost_estimate(&cands[ci].move_full, 1);
  }

  // Build distinct-tile-types arrays from `unseen` once.
  MachineLetter tile_types[MAX_ALPHABET_SIZE];
  int tile_counts[MAX_ALPHABET_SIZE];
  int num_tile_types = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    if (unseen[ml] > 0) {
      tile_types[num_tile_types] = (MachineLetter)ml;
      tile_counts[num_tile_types] = (int)unseen[ml];
      num_tile_types++;
    }
  }

  // Per-thread TT setup. Three modes (priority order):
  //   1. shared_tt_per_thread != NULL: caller-supplied array, length =
  //   num_threads
  //   2. shared_tt != NULL (legacy): mirror into a per-thread array
  //   3. Otherwise allocate num_threads private TTs at fraction/num_threads
  bool tts_owned_local = false;
  bool per_tt_array_owned = false;
  TranspositionTable **per_thread_tts = NULL;
  if (args->shared_tt_per_thread) {
    per_thread_tts = args->shared_tt_per_thread;
  } else if (args->shared_tt) {
    per_thread_tts =
        malloc_or_die((size_t)num_threads * sizeof(TranspositionTable *));
    for (int ti = 0; ti < num_threads; ti++) {
      per_thread_tts[ti] = args->shared_tt;
    }
    per_tt_array_owned = true;
  } else if (args->tt_fraction_of_mem > 0.0) {
    per_thread_tts =
        malloc_or_die((size_t)num_threads * sizeof(TranspositionTable *));
    double per_thread_fraction = args->tt_fraction_of_mem / (double)num_threads;
    for (int ti = 0; ti < num_threads; ti++) {
      per_thread_tts[ti] = transposition_table_create(per_thread_fraction);
    }
    per_tt_array_owned = true;
    tts_owned_local = true;
  }

  Timer wall_timer;
  ctimer_start(&wall_timer);

  // Absolute monotonic-ns wall-clock deadline. Plumbed into every endgame
  // solve so workers stop mid-ply once the bai_peg_solve budget is hit.
  int64_t bai_deadline_ns = 0;
  if (args->time_budget_seconds > 0.0) {
    bai_deadline_ns =
        ctimer_monotonic_ns() + (int64_t)(args->time_budget_seconds * 1.0e9);
  }

  // Minimum per-scenario time we'll allow ourselves to commit to a new
  // evaluation. Below this we'd rather short-circuit than start a doomed-to-
  // overshoot search.
  const double MIN_PER_SOLVE = 0.05;

  // Phase 0 (optional, initial_playout): scenario-aware prior. Evaluates
  // every candidate at depth=0 in a single flattened (cand, scenario) batch
  // — one thread spawn/join cycle covers all num_candidates*num_tile_types
  // scenarios. Endgame solver routes plies==0 to greedy leaf playout
  // (highest-scoring move each turn to end-of-game with rack adjustments).
  // PUCT then picks depth-1 evals for the candidates that look promising.
  // Phase 0 runs whenever initial_playout is set, with or without progressive
  // widening. With widening + ipo, every cand starts the PUCT loop with its
  // own scenario-aware Q from the playout, so neighbor-inheritance becomes a
  // no-op (only fires for unvisited cands). Without widening, Phase 0 alone
  // is the warm-up. Phase 1 (depth-1 negamax sweep) is the fallback for the
  // non-widening, non-playout case.
  if (args->initial_playout) {
    Timer playout_timer;
    ctimer_start(&playout_timer);
    bp_playout_batch(
        cands, num_candidates, 0, mover_idx, opp_idx, unseen, ld_size,
        tile_types, tile_counts, num_tile_types, args->thread_control,
        per_thread_tts, dlm, args->endgame_time_per_solve, num_threads,
        args->thread_index_offset, bai_deadline_ns, args->pure_playout);
    // Snapshot the playout signal for downstream regression analysis. After
    // PUCT runs, q_* gets overwritten by deeper negamax results.
    for (int ci = 0; ci < num_candidates; ci++) {
      cands[ci].playout_q_win_pct = cands[ci].q_win_pct;
      cands[ci].playout_q_mean_spread = cands[ci].q_mean_spread;
      cands[ci].playout_q_set = true;
    }
    double playout_total = ctimer_elapsed_seconds(&playout_timer);
    // Spread playout cost evenly across cands. PUCT uses time_paid in the
    // exploration bonus denominator; equal time_paid means PUCT's first
    // negamax pick is driven by Q + prior, exactly what we want from the
    // playout-as-prior pattern. Don't count playouts in evaluations_done —
    // they're the prior, not a negamax search at any depth.
    if (num_candidates > 0) {
      double per_cand = playout_total / (double)num_candidates;
      for (int ci = 0; ci < num_candidates; ci++) {
        cands[ci].time_paid += per_cand;
        cands[ci].last_eval_seconds = per_cand;
        // Leave cost_estimate at the bp_initial_cost_estimate(depth=1) value
        // computed during cand setup — playout time isn't predictive of d1.
      }
    }
  } else if (!args->progressive_widening || args->min_active > 0) {
    // Phase 1: warm-up — evaluate every candidate at depth 1.
    // With progressive_widening + min_active set, only warm up the seed
    // top-min_active cands; widening will admit more later if budget allows.
    int warmup_count = num_candidates;
    if (args->progressive_widening && args->min_active > 0 &&
        args->min_active < warmup_count) {
      warmup_count = args->min_active;
    }
    for (int ci = 0; ci < warmup_count; ci++) {
      double per_solve = args->endgame_time_per_solve;
      if (args->time_budget_seconds > 0.0) {
        double remaining =
            args->time_budget_seconds - ctimer_elapsed_seconds(&wall_timer);
        if (remaining <= MIN_PER_SOLVE) {
          result->stopped_by_time = true;
          break;
        }
        // Cap per-scenario budget so the worst-case eval can't push us past
        // the wall-clock cap by more than one IDS iteration.
        if (per_solve <= 0.0 || per_solve > remaining) {
          per_solve = remaining;
        }
      }
      Timer eval_timer;
      ctimer_start(&eval_timer);
      if (cands[ci].is_pass) {
        if (args->log_solve_details) {
          fprintf(stderr,
                  "  [bp] warmup pass cand depth=1 (recursive opp PEG, %d "
                  "scenarios)\n",
                  num_tile_types);
          fflush(stderr);
        }
        bp_evaluate_pass(&cands[ci], 1, args, mover_idx, opp_idx, unseen,
                         ld_size, tile_types, tile_counts, num_tile_types,
                         per_solve);
      } else {
        bp_evaluate_cand(&cands[ci], 1, mover_idx, opp_idx, unseen, ld_size,
                         tile_types, tile_counts, num_tile_types,
                         args->thread_control, per_thread_tts, dlm, per_solve,
                         num_threads, args->thread_index_offset,
                         bai_deadline_ns, args->executor);
      }
      double measured = ctimer_elapsed_seconds(&eval_timer);
      cands[ci].time_paid += measured;
      cands[ci].last_eval_seconds = measured;
      // Predict next-depth cost: rough doubling per ply, floored at the
      // measured value so it can never under-predict the depth we just ran.
      cands[ci].cost_estimate = measured * 2.0;
      result->evaluations_done++;
      if (max_depth <= 1) {
        cands[ci].fully_explored = true;
      }
    }
  }

  // Phase 2: PUCT-driven adaptive deepening.
  int progress_top = args->progress_num_top > 0 ? args->progress_num_top
                                                : BAI_PEG_DEFAULT_PROGRESS_TOP;
  if (progress_top > num_candidates) {
    progress_top = num_candidates;
  }
  // Working buffers for the progress callback (reused across calls).
  SmallMove *cb_moves = malloc_or_die(progress_top * sizeof(SmallMove));
  double *cb_wp = malloc_or_die(progress_top * sizeof(double));
  double *cb_sp = malloc_or_die(progress_top * sizeof(double));
  int *cb_depths = malloc_or_die(progress_top * sizeof(int));
  BaiCand **ranking = malloc_or_die(num_candidates * sizeof(BaiCand *));

  // Diagnostic sweep mode: skip PUCT, just evaluate every cand at every
  // depth 1..sweep_max_depth and log every (cand, depth, q, time) tuple.
  // Used to gather ground truth for the depth -> value curve.
  if (args->sweep_max_depth > 0) {
    int sweep_d_max = args->sweep_max_depth;
    if (sweep_d_max > max_depth) {
      sweep_d_max = max_depth;
    }
    fprintf(stderr, "[bp.sweep] mode: %d cands x %d depths = %d evaluations\n",
            num_candidates, sweep_d_max, num_candidates * sweep_d_max);
    fflush(stderr);
    for (int d = 1; d <= sweep_d_max; d++) {
      for (int ci = 0; ci < num_candidates; ci++) {
        Timer eval_timer;
        ctimer_start(&eval_timer);
        if (cands[ci].is_pass) {
          bp_evaluate_pass(&cands[ci], d, args, mover_idx, opp_idx, unseen,
                           ld_size, tile_types, tile_counts, num_tile_types,
                           args->endgame_time_per_solve);
        } else {
          if (cands[ci].post_cand_game == NULL) {
            Game *g = game_duplicate(base_game);
            game_set_endgame_solving_mode(g);
            game_set_backup_mode(g, BACKUP_MODE_OFF);
            play_move_without_drawing_tiles(&cands[ci].move_full, g);
            bp_clear_false_game_end(g);
            cands[ci].post_cand_game = g;
          }
          bp_evaluate_cand(
              &cands[ci], d, mover_idx, opp_idx, unseen, ld_size, tile_types,
              tile_counts, num_tile_types, args->thread_control, per_thread_tts,
              dlm, args->endgame_time_per_solve, num_threads,
              args->thread_index_offset, bai_deadline_ns, args->executor);
        }
        double measured = ctimer_elapsed_seconds(&eval_timer);
        cands[ci].time_paid += measured;
        cands[ci].last_eval_seconds = measured;
        cands[ci].cost_estimate = measured * 2.0;
        result->evaluations_done++;
        fprintf(stderr,
                "[bp.sweep] cand[%d] %s static=%d depth=%d "
                "q_win%%=%.4f q_spread=%+0.3f time=%.3fs\n",
                ci, cands[ci].is_pass ? "PASS" : "play", cands[ci].static_score,
                d, cands[ci].q_win_pct, cands[ci].q_mean_spread, measured);
        fflush(stderr);
      }
    }
    goto sweep_finish;
  }

  // Sequential Halving (Karnin–Koren–Somekh 2013) alternative to PUCT:
  // each round, deepen every surviving cand by one ply, then drop the
  // bottom half of the surviving set by current utility. Continues until
  // one cand survives, or wall-clock / max_depth runs out. Uses the
  // existing bp_evaluate_cand path so all the executor / coupled-pass
  // plumbing applies unchanged.
  if (args->sequential_halving) {
    int *surviving = malloc_or_die(num_candidates * sizeof(int));
    int n_surv = num_candidates;
    for (int i = 0; i < num_candidates; i++) {
      surviving[i] = i;
    }
    int round_depth = 2; // warmup already evaluated at d=1
    while (n_surv > 1 && round_depth <= max_depth && !result->stopped_by_time &&
           !result->stopped_by_max_evals) {
      // Deepen every surviving cand to round_depth.
      for (int s_i = 0; s_i < n_surv; s_i++) {
        if (args->time_budget_seconds > 0.0 &&
            ctimer_elapsed_seconds(&wall_timer) >= args->time_budget_seconds) {
          result->stopped_by_time = true;
          break;
        }
        if (args->max_evaluations > 0 &&
            result->evaluations_done >= args->max_evaluations) {
          result->stopped_by_max_evals = true;
          break;
        }
        int ci = surviving[s_i];
        if (cands[ci].depth_evaluated >= round_depth) {
          continue;
        }
        // Predictive budget gate: skip this cand if its predicted eval
        // cost exceeds the remaining budget. Without this, SH happily
        // launches a 10s pass eval with 1s remaining and ends ~10s past
        // budget. Matches the corresponding PUCT gate below.
        if (args->time_budget_seconds > 0.0) {
          double remaining = args->time_budget_seconds -
                             ctimer_elapsed_seconds(&wall_timer);
          if (remaining <= MIN_PER_SOLVE) {
            result->stopped_by_time = true;
            break;
          }
          if (cands[ci].cost_estimate > 0.0 &&
              cands[ci].cost_estimate > remaining) {
            continue;
          }
        }
        if (!cands[ci].is_pass && cands[ci].post_cand_game == NULL) {
          Game *g = game_duplicate(base_game);
          game_set_endgame_solving_mode(g);
          game_set_backup_mode(g, BACKUP_MODE_OFF);
          play_move_without_drawing_tiles(&cands[ci].move_full, g);
          bp_clear_false_game_end(g);
          cands[ci].post_cand_game = g;
        }
        Timer eval_t;
        ctimer_start(&eval_t);
        if (cands[ci].is_pass) {
          bp_evaluate_pass(&cands[ci], round_depth, args, mover_idx, opp_idx,
                           unseen, ld_size, tile_types, tile_counts,
                           num_tile_types, args->endgame_time_per_solve);
        } else {
          bp_evaluate_cand(
              &cands[ci], round_depth, mover_idx, opp_idx, unseen, ld_size,
              tile_types, tile_counts, num_tile_types, args->thread_control,
              per_thread_tts, dlm, args->endgame_time_per_solve, num_threads,
              args->thread_index_offset, bai_deadline_ns, args->executor);
        }
        double measured = ctimer_elapsed_seconds(&eval_t);
        cands[ci].time_paid += measured;
        cands[ci].last_eval_seconds = measured;
        cands[ci].cost_estimate = measured * 2.0;
        result->evaluations_done++;
      }
      if (result->stopped_by_time || result->stopped_by_max_evals) {
        break;
      }
      // Halve the surviving set by current utility. Use the same
      // utility function as the final pick so behavior is consistent.
      bp_populate_sort_utility(cands, num_candidates, alpha);
      // Sort surviving[] indices by utility descending.
      for (int i = 0; i < n_surv; i++) {
        for (int j = i + 1; j < n_surv; j++) {
          if (cands[surviving[j]].sort_utility >
              cands[surviving[i]].sort_utility) {
            int t = surviving[i];
            surviving[i] = surviving[j];
            surviving[j] = t;
          }
        }
      }
      int next_n = n_surv / 2;
      if (next_n < 1) {
        next_n = 1;
      }
      n_surv = next_n;
      round_depth++;
    }
    free(surviving);
    goto sweep_finish;
  }

  // Tracks how many candidates were active in the previous PUCT iteration.
  // When widening expands the active set, each newly-admitted candidate
  // inherits Q from its rank-up neighbor (the cand just above it by static
  // score, which has already been visited). Calibration-free seeding —
  // avoids needing to map move-score to win%/spread, and avoids the cost of
  // a depth-0 playout per new cand.
  int prev_active = 0;

  while (!result->stopped_by_time && !result->stopped_by_max_evals &&
         !result->stopped_by_confidence) {
    if (args->time_budget_seconds > 0.0 &&
        ctimer_elapsed_seconds(&wall_timer) >= args->time_budget_seconds) {
      result->stopped_by_time = true;
      break;
    }
    if (args->max_evaluations > 0 &&
        result->evaluations_done >= args->max_evaluations) {
      result->stopped_by_max_evals = true;
      break;
    }

    // Cost-weighted PUCT: rather than counting evaluations equally, we use
    // wall time spent per candidate as the "visit budget." This way a
    // candidate at depth 4 (which paid ~5s to get there) doesn't get the
    // same exploration bonus shrinkage as a depth-1 candidate that only
    // paid 50ms. As the leader gets deeper, its bonus shrinks fast and
    // cheap-to-explore candidates compete naturally.
    double total_time = 0.0;
    for (int ci = 0; ci < num_candidates; ci++) {
      total_time += cands[ci].time_paid;
    }

    // Compute remaining wall-clock budget for cost-aware selection.
    double remaining_budget = -1.0; // -1 = no time budget
    if (args->time_budget_seconds > 0.0) {
      remaining_budget =
          args->time_budget_seconds - ctimer_elapsed_seconds(&wall_timer);
    }

    // Compute median-ish cost across candidates for bonus normalization.
    // Use a simple mean of cost_estimates as the reference.
    double sum_cost = 0.0;
    int cost_n = 0;
    for (int ci = 0; ci < num_candidates; ci++) {
      if (!cands[ci].fully_explored) {
        sum_cost += cands[ci].cost_estimate;
        cost_n++;
      }
    }
    double ref_cost = (cost_n > 0) ? sum_cost / cost_n : 0.1;
    if (ref_cost <= 0) {
      ref_cost = 0.01;
    }

    // Progressive widening: only the top-N candidates by static-score order
    // are eligible for selection, where N grows with total visits done so
    // far. Bad moves never get touched at short budgets.
    int active_count = num_candidates;
    if (args->progressive_widening) {
      double w_c = args->widening_c > 0.0 ? args->widening_c : 2.0;
      double sqrt_visits = sqrt((double)result->evaluations_done + 1.0);
      int target = (int)ceil(w_c * sqrt_visits);
      if (target < 2) {
        target = 2;
      }
      // Floor to min_active when set: top-K cands are always active even
      // before widening's sqrt growth catches up.
      if (args->min_active > 0 && target < args->min_active) {
        target = args->min_active;
      }
      if (target > num_candidates) {
        target = num_candidates;
      }
      active_count = target;

      // Inherit Q from the rank-up neighbor when a new candidate becomes
      // active. Calibration-free seeding: the neighbor's Q is already on
      // the right scale (win% in [0,1], spread in points). The new cand
      // starts where its slightly-stronger predecessor stands; PUCT then
      // refines via real evaluations. Skip cands whose neighbor is itself
      // unvisited (cand 0 has no neighbor; later cands rarely hit this
      // since rank K-1 is admitted before rank K under monotonic growth).
      const double bw_p = args->blend_w_playout;
      const double bw_n = args->blend_w_neighbor;
      const bool blend_enabled = (bw_p > 0.0 || bw_n > 0.0);
      for (int K = prev_active; K < active_count; K++) {
        if (K == 0) {
          continue;
        }
        // Snapshot the neighbor's Q at admission for regression analysis,
        // regardless of whether this cand will use it as a seed. Done once
        // per cand at the moment of admission.
        if (!cands[K].neighbor_q_set && cands[K - 1].visits > 0) {
          cands[K].neighbor_q_win_pct = cands[K - 1].q_win_pct;
          cands[K].neighbor_q_mean_spread = cands[K - 1].q_mean_spread;
          cands[K].neighbor_q_set = true;
        }
        // Initial Q assignment at admission. Three modes:
        // 1) Blend (both signals + blend_enabled): weighted combo.
        // 2) Neighbor-only (cand has no playout, predecessor visited).
        // 3) No-op (cand already has real-Q from Phase 0 or negamax).
        const bool predecessor_visited = (cands[K - 1].visits > 0);
        if (blend_enabled && cands[K].playout_q_set &&
            cands[K].neighbor_q_set) {
          cands[K].q_win_pct = bw_p * cands[K].playout_q_win_pct +
                               bw_n * cands[K].neighbor_q_win_pct;
          cands[K].q_mean_spread = bw_p * cands[K].playout_q_mean_spread +
                                   bw_n * cands[K].neighbor_q_mean_spread;
        } else if (cands[K].visits == 0 && predecessor_visited) {
          cands[K].q_win_pct = cands[K - 1].q_win_pct;
          cands[K].q_mean_spread = cands[K - 1].q_mean_spread;
        }
      }
      prev_active = active_count;
    }

    // Pick the highest-PUCT non-fully-explored candidate.
    int chosen = -1;
    double best_puct = -1.0e300;
    double sqrt_total_time = sqrt(total_time + 1.0);
    for (int ci = 0; ci < active_count; ci++) {
      if (cands[ci].fully_explored) {
        continue;
      }
      // (1) Reject infeasible: skip if predicted next-eval cost won't fit
      //     in the remaining budget. Lets compute focus on candidates that
      //     can actually finish a useful evaluation.
      if (remaining_budget > 0.0 &&
          cands[ci].cost_estimate > remaining_budget) {
        continue;
      }
      // Q is win-rate in [0, 1] so the exploration bonus (also in [0, ~1])
      // is on a comparable scale.
      double bonus = puct_c * cands[ci].prior * sqrt_total_time /
                     (1.0 + cands[ci].time_paid);
      // (2) Cost-normalize the bonus: an expensive candidate's exploration
      //     bonus is scaled down by sqrt(its_cost / median_cost). High-Q
      //     leaders still win on Q dominance; cheap-but-ambiguous arms
      //     still attract early exploration; expensive arms have to earn
      //     their deepening through Q rather than bonus.
      double cost_penalty = sqrt(cands[ci].cost_estimate / ref_cost);
      if (cost_penalty < 0.1) {
        cost_penalty = 0.1;
      }
      // Q is utility = win_pct + alpha * spread. With alpha == 0 this is pure
      // win-rate; alpha > 0 rewards spread on the same scale (e.g. alpha=0.01
      // makes 100 spread points equivalent to 1 win).
      double q = bp_compute_utility(&cands[ci], alpha);
      double score = q + (bonus / cost_penalty);
      // Tiny spread tiebreaker for the alpha==0 case so candidates with
      // identical win-rates still separate consistently.
      if (alpha == 0.0) {
        score += 1e-3 * cands[ci].q_mean_spread;
      }
      if (score > best_puct) {
        best_puct = score;
        chosen = ci;
      }
    }
    if (chosen < 0) {
      // Everyone is fully explored; nothing more to do.
      break;
    }

    // First visit to any cand starts at depth 1 (full negamax). Without
    // widening the warm-up phase already ran depth=1 for all cands so
    // depth_evaluated is at least 1 here. With widening, newly-admitted
    // cands inherit Q from their rank-up neighbor (calibration-free
    // seeding) and skip the playout step.
    int next_depth = cands[chosen].depth_evaluated + 1;
    if (next_depth > max_depth) {
      cands[chosen].fully_explored = true;
      continue;
    }
    // Lazy post_cand_game creation: with widening, most cands never get
    // touched, so we don't pre-stage. Build it now if needed. (Skipped for
    // pass cands — pass evaluation rebuilds the post-pass game per
    // scenario inside bp_evaluate_pass.)
    if (!cands[chosen].is_pass && cands[chosen].post_cand_game == NULL) {
      Game *g = game_duplicate(base_game);
      game_set_endgame_solving_mode(g);
      game_set_backup_mode(g, BACKUP_MODE_OFF);
      play_move_without_drawing_tiles(&cands[chosen].move_full, g);
      bp_clear_false_game_end(g);
      cands[chosen].post_cand_game = g;
    }
    // Cap per-scenario time to whatever's left so the eval can't overshoot
    // the wall-clock budget by more than one IDS iteration's worth.
    double per_solve = args->endgame_time_per_solve;
    if (args->time_budget_seconds > 0.0) {
      double remaining =
          args->time_budget_seconds - ctimer_elapsed_seconds(&wall_timer);
      if (remaining <= MIN_PER_SOLVE) {
        result->stopped_by_time = true;
        break;
      }
      if (per_solve <= 0.0 || per_solve > remaining) {
        per_solve = remaining;
      }
    }
    if (args->log_solve_details) {
      fprintf(stderr,
              "  [bp] step %d: chose %s (rank=%d static_score=%d "
              "is_pass=%d) at depth=%d  prev_q_win%%=%.3f prev_q_spread="
              "%+0.2f\n",
              result->evaluations_done, cands[chosen].is_pass ? "PASS" : "play",
              chosen, cands[chosen].static_score, (int)cands[chosen].is_pass,
              next_depth, cands[chosen].q_win_pct, cands[chosen].q_mean_spread);
      fflush(stderr);
    }
    Timer eval_timer;
    ctimer_start(&eval_timer);
    if (cands[chosen].is_pass) {
      if (args->log_solve_details) {
        fprintf(stderr,
                "  [bp] visit pass cand depth=%d (recursive opp PEG, %d "
                "scenarios)\n",
                next_depth, num_tile_types);
        fflush(stderr);
      }
      // Pass eval: budget per call = per_solve seconds total (split across
      // scenarios inside bp_evaluate_pass).
      bp_evaluate_pass(&cands[chosen], next_depth, args, mover_idx, opp_idx,
                       unseen, ld_size, tile_types, tile_counts, num_tile_types,
                       per_solve);
      if (args->log_solve_details) {
        fprintf(stderr,
                "  [bp] pass cand updated: q_win%%=%.3f q_spread=%+0.2f "
                "depth=%d\n",
                cands[chosen].q_win_pct, cands[chosen].q_mean_spread,
                cands[chosen].depth_evaluated);
        fflush(stderr);
      }
    } else {
      bp_evaluate_cand(&cands[chosen], next_depth, mover_idx, opp_idx, unseen,
                       ld_size, tile_types, tile_counts, num_tile_types,
                       args->thread_control, per_thread_tts, dlm, per_solve,
                       num_threads, args->thread_index_offset, bai_deadline_ns,
                       args->executor);
    }
    double measured = ctimer_elapsed_seconds(&eval_timer);
    cands[chosen].time_paid += measured;
    cands[chosen].last_eval_seconds = measured;
    // Predict next-depth cost from this measurement.
    cands[chosen].cost_estimate = measured * 2.0;
    if (next_depth >= max_depth) {
      cands[chosen].fully_explored = true;
    }
    result->evaluations_done++;

    // Confidence-based early stop: leader vs best challenger by utility.
    if (args->early_stop_gap > 0.0 && args->early_stop_min_depth > 0) {
      int li = -1;
      double l_q = -1.0e300;
      for (int ci = 0; ci < num_candidates; ci++) {
        double q = bp_compute_utility(&cands[ci], alpha);
        if (q > l_q) {
          l_q = q;
          li = ci;
        }
      }
      int ci2 = -1;
      double c_q = -1.0e300;
      for (int ci = 0; ci < num_candidates; ci++) {
        if (ci == li) {
          continue;
        }
        double q = bp_compute_utility(&cands[ci], alpha);
        if (q > c_q) {
          c_q = q;
          ci2 = ci;
        }
      }
      if (li >= 0 && ci2 >= 0 &&
          cands[li].depth_evaluated >= args->early_stop_min_depth &&
          cands[ci2].depth_evaluated >= args->early_stop_min_depth &&
          (l_q - c_q) >= args->early_stop_gap) {
        result->stopped_by_confidence = true;
      }
    }

    // Progress callback: ranked snapshot of top-K by win% / spread.
    if (args->progress_callback) {
      for (int ci = 0; ci < num_candidates; ci++) {
        ranking[ci] = &cands[ci];
      }
      bp_populate_sort_utility(cands, num_candidates, alpha);
      qsort(ranking, num_candidates, sizeof(BaiCand *),
            bp_compare_cand_ptrs_by_q);
      for (int i = 0; i < progress_top; i++) {
        cb_moves[i] = ranking[i]->move;
        cb_wp[i] = ranking[i]->q_win_pct;
        cb_sp[i] = ranking[i]->q_mean_spread;
        cb_depths[i] = ranking[i]->depth_evaluated;
      }
      args->progress_callback(result->evaluations_done,
                              ctimer_elapsed_seconds(&wall_timer), cb_moves,
                              cb_wp, cb_sp, cb_depths, progress_top, args->game,
                              args->progress_callback_data);
    }
  }

sweep_finish:
  // Final ranking and result fill.
  for (int ci = 0; ci < num_candidates; ci++) {
    ranking[ci] = &cands[ci];
  }
  bp_populate_sort_utility(cands, num_candidates, alpha);
  qsort(ranking, num_candidates, sizeof(BaiCand *), bp_compare_cand_ptrs_by_q);
  // Sanity: never return an unvisited candidate. If somehow the budget was
  // so tight that even cand 0 didn't get a depth-0 visit, log an error and
  // fall back to the top-static-score move (which is what an unvisited
  // ranking[0] already resolves to via the static-score tiebreak in the
  // compare, but we want the warning to surface).
  if (ranking[0]->visits == 0) {
    log_warn("bai_peg_solve: budget exhausted before any candidate was "
             "evaluated; falling back to top-static-score move");
  }
  result->best_move = ranking[0]->move;
  result->best_win_pct = ranking[0]->q_win_pct;
  result->best_mean_spread = ranking[0]->q_mean_spread;
  result->best_depth_evaluated = ranking[0]->depth_evaluated;
  result->candidates_considered = num_candidates;
  result->seconds_elapsed = ctimer_elapsed_seconds(&wall_timer);

  // Per-cand stats for offline analysis (regression on which prior signal
  // best predicts the final Q). cand_stats[i] follows ranking order: index 0
  // is the picked move, then descending by utility.
  if (args->request_cand_stats) {
    result->cand_stats = malloc_or_die(num_candidates * sizeof(BaiCandStats));
    for (int i = 0; i < num_candidates; i++) {
      const BaiCand *c = ranking[i];
      BaiCandStats *s = &result->cand_stats[i];
      s->move = c->move;
      s->static_score = c->static_score;
      // The original cand index (rank by static score) is its offset from
      // the sorted cands[] array.
      s->rank = (int)(c - cands);
      s->playout_q_set = c->playout_q_set;
      s->playout_q_win_pct = c->playout_q_win_pct;
      s->playout_q_mean_spread = c->playout_q_mean_spread;
      s->neighbor_q_set = c->neighbor_q_set;
      s->neighbor_q_win_pct = c->neighbor_q_win_pct;
      s->neighbor_q_mean_spread = c->neighbor_q_mean_spread;
      s->final_q_win_pct = c->q_win_pct;
      s->final_q_mean_spread = c->q_mean_spread;
      s->depth_evaluated = c->depth_evaluated;
      s->visits = c->visits;
      s->is_best = (i == 0);
    }
  } else {
    result->cand_stats = NULL;
  }

  // Visit histogram: a candidate that reached depth_evaluated=N completed one
  // evaluation at each depth 1..N (BAI's bp_evaluate_cand goes one ply at a
  // time). So visits_at_depth[d] = count of candidates with depth_evaluated >=
  // d.
  for (int d = 0; d <= BAI_PEG_MAX_DEPTH; d++) {
    result->visits_at_depth[d] = 0;
  }
  for (int ci = 0; ci < num_candidates; ci++) {
    int de = cands[ci].depth_evaluated;
    if (de > BAI_PEG_MAX_DEPTH) {
      de = BAI_PEG_MAX_DEPTH;
    }
    for (int d = 1; d <= de; d++) {
      result->visits_at_depth[d]++;
    }
  }

  // Cleanup.
  free(cb_moves);
  free(cb_wp);
  free(cb_sp);
  free(cb_depths);
  free(ranking);
  for (int ci = 0; ci < num_candidates; ci++) {
    if (cands[ci].post_cand_game) {
      game_destroy(cands[ci].post_cand_game);
    }
    if (cands[ci].coupled_inner_sessions) {
      for (int si = 0; si < cands[ci].coupled_count; si++) {
        bp_inner_session_destroy(&cands[ci].coupled_inner_sessions[si]);
      }
      free(cands[ci].coupled_inner_sessions);
    }
    for (int player_idx = 0; player_idx < 2; player_idx++) {
      if (cands[ci].cached_pruned_kwgs[player_idx]) {
        kwg_destroy(cands[ci].cached_pruned_kwgs[player_idx]);
        cands[ci].cached_pruned_kwgs[player_idx] = NULL;
      }
    }
  }
  free(cands);
  if (tts_owned_local) {
    for (int ti = 0; ti < num_threads; ti++) {
      transposition_table_destroy(per_thread_tts[ti]);
    }
  }
  if (per_tt_array_owned) {
    free(per_thread_tts);
  }
  if (pruned_kwgs[0]) {
    kwg_destroy(pruned_kwgs[0]);
  }
  if (pruned_kwgs[1]) {
    kwg_destroy(pruned_kwgs[1]);
  }
  game_destroy(base_game);
}

void bai_cand_stats_free(BaiCandStats *cand_stats) { free(cand_stats); }
