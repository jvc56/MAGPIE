#include "peg.h"

#include "../compat/cpthread.h"
#include "../compat/cpthread_defs.h"
#include "../compat/ctime.h"
#include "../compat/memory_info.h"
#include "../def/board_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/kwg_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/peg_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/dictionary_word.h"
#include "../ent/endgame_results.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/thread_control.h"
#include "../ent/transposition_table.h"
#include "../str/move_string.h"
#include "../util/fnv.h"
#include "../util/io_util.h"
#include "../util/stable_sort.h"
#include "../util/string_util.h"
#include "cgp.h"
#include "endgame.h"
#include "gameplay.h"
#include "kwg_maker.h"
#include "move_gen.h"
#include "peg_combinatorics.h"
#include "peg_memory.h"
#include "peg_pool.h"
#include "word_prune.h"
#include <assert.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Default schedule for the halving stages, which run AFTER the root. Stage 0 is
// not in this table: it greedy-evaluates EVERY candidate play (a fast playout,
// no narrowing) and keeps the top counts[0]. Each halving stage then carries
// the surviving top counts[i] forward and re-ranks them at one more ply of
// fidelity: counts[0]=32 at 2-ply, counts[1]=16 at 3-ply, counts[2]=8 at 4-ply,
// counts[3]=4 at 5-ply, counts[4]=2 at 6-ply. So each entry is how many plays
// are KEPT at that stage, not a cap on what enters it (stage 0 always sees them
// all). The tail is top-2, never top-1 — a stage re-ranks a set, so it needs
// >= 2 to compare. The table length is the default number of halving stages;
// there is no fixed cap, so a caller may pass a longer schedule via
// PegArgs.stage_top_k.
static const int PEG_DEFAULT_HALVING_COUNTS[] = {32, 16, 8, 4, 2};

// Two win%s closer than this are tied: floating-point noise, not a result.
#define PEG_WIN_PCT_TIE_TOLERANCE 1e-9

// Solve-scoped cache mapping a post-candidate board signature to its
// per-candidate pruned KWG. At an emptier (0-in-bag) endgame leaf the pruned
// KWG depends only on the post-cand board and the union of both racks (all
// remaining tiles) -- both fixed per candidate across every scenario AND stage
// -- so one build is reused by every leaf of that candidate. Built lazily by
// chaining off the parent (root) prune already on the game, which is correct
// because the parent is a superset of any descendant's playable words. Shared
// across workers: lookups hold the lock briefly, builds run outside it.
typedef struct PegPruneCache {
  cpthread_mutex_t mutex;
  uint64_t *keys; // 0 marks an empty slot
  KWG **values;   // owned; destroyed with the cache
  int capacity;   // power of two
  int count;
} PegPruneCache;

// A reusable scratch frame for the nested recursion: one game (a candidate
// template or a per-scenario sub-game) and one candidate move list. Acquired
// from / released to the owning worker's free-list. Each worker is touched by
// exactly one thread (its pool index, or the main thread for the helper slot),
// so the free-list needs no locking -- and a frame held across a parallel
// scenario submission survives help-while-waiting interleaving.
typedef struct PegNestFrame {
  Game *game;
  MoveList *moves;
  struct PegNestFrame *free_next; // free-list link (released frames)
  struct PegNestFrame *all_next;  // all-frames link (for teardown)
} PegNestFrame;

// The endgame transposition table every worker of one solve shares. Jobs are
// per (candidate, split) and land on any worker, so a table per worker would
// split the cross-scenario and stage-to-stage reuse that makes the leaf
// endgames cheap. The greedy seed does not use it, and many solves never reach
// an exact leaf, so the table is created on first use: under the mutex, by the
// first worker that needs it. Caller-provided tables
// (PegArgs.shared_endgame_tt) are installed up front and never destroyed here.
typedef struct PegSharedEndgameTT {
  cpthread_mutex_t mutex;
  TranspositionTable *tt; // guarded by mutex until every worker has cached it
  double fraction;        // size of the table to create
  int zero_threads;       // threads that zero it (the other workers wait)
  bool owned;             // created here (destroy at teardown)
} PegSharedEndgameTT;

// One move of a greedy playout that several scenarios play identically: the
// first playout to reach it records what the generation picked, and the rest
// play that instead of generating again.
typedef struct PegSharedPly {
  bool known;
  // The generation found no move, so the playout stops here.
  bool none;
  Move move;
} PegSharedPly;

// The opponent's first greedy reply to the candidate being evaluated, by the
// opponent's rack. With the candidate played and its draw made, that reply
// depends only on the board, the scores, the opponent's rack and the sizes of
// the bag and the mover's rack, all fixed for the candidate except the
// opponent's rack: while tiles remain, equity (the PAT term included) reads
// the bag and the other rack only through their sizes, and once the bag is
// empty the playout ranks by score. So every split that leaves the opponent
// the same rack shares it. Entries from an earlier candidate are told apart by
// their stamp.
typedef struct PegReplyCacheEntry {
  uint64_t stamp;
  int n_letters;
  MachineLetter letters[RACK_SIZE];
  PegSharedPly reply;
} PegReplyCacheEntry;

typedef struct PegReplyCache {
  uint64_t stamp;
  int count;
  PegReplyCacheEntry entries[PEG_REPLY_CACHE_SLOTS];
} PegReplyCache;

// Per-worker scratch: a greedy-playout move list plus a reusable endgame
// context/results pair. Indexed by the pool worker_idx (one extra slot for the
// main thread when it helps drain the queue).
typedef struct PegWorker {
  MoveList *playout_ml;
  EndgameCtx *eg_ctx;
  EndgameResults *eg_results;
  // Per-candidate template: the post-cand board with cross-sets generated once
  // (the board is identical across all of a cand's scenarios), so the cand play
  // and cross-set generation are hoisted out of the per-scenario loop.
  Game *template_game;
  // Reused per-scenario game: game_copy from the template, then only the racks
  // and bag are reset (no cand replay, no cross-set work).
  Game *scratch_game;
  // This worker's cached pointer to the solve's shared endgame TT (NULL until
  // its first exact leaf). Many scenarios reach identical board states, so
  // cross-scenario reuse is the dominant endgame speedup.
  TranspositionTable *eg_tt;
  PegSharedEndgameTT *shared_eg_tt;
  // Shared per-solve cache of per-candidate leaf prunes (see PegPruneCache).
  PegPruneCache *prune_cache;
  // PegArgs.disable_best_reply_shortcuts, for every leaf endgame solve.
  bool disable_best_reply_shortcuts;
  // PegArgs.first_win, for every leaf endgame solve.
  bool first_win;
  // Opponent replies to the candidate this worker is evaluating.
  PegReplyCache *reply_cache;

  // Nested-PEG lookahead state. Solve-level knobs copied here so the recursion
  // needn't thread them through every job/ctx.
  ThreadControl *thread_control; // needed by nested emptier endgame solves
  PegPool *nest_pool; // shared pool for parallel inner scenarios (NULL=inline)
  struct PegWorker *nest_workers; // worker array; jobs index by worker_idx
  int nest_worker_idx; // this worker's pool index (helper idx when submitting)
  bool nested_enabled;
  int nested_cand_cap;
  const int *nested_cand_caps; // inner-peg stage schedule (NULL = flat cap)
  int nested_n_cand_caps;
  int nested_stride;
  const int *nested_strides_by_bag; // see PegArgs.nested_strides_by_bag
  int nested_n_strides_by_bag;
  int nested_emptier_ply_cap;
  int nested_max_depth;
  bool nested_score_actual; // see PegArgs
  bool nested_pessimistic;  // see PegArgs
  int inner_top_k;          // see PegArgs; the pessimistic opponent's cap
  int root_mover_idx;       // the player the solve chooses a move for
  // The opponent's known tiles (PegArgs.opp_rack_known), by machine letter,
  // which every root scenario puts back on its rack; NULL when none.
  const uint8_t *known_opp;
  // Free-list of reusable scratch frames (see PegNestFrame). nest_free holds
  // released frames; nest_all chains every allocated frame for teardown.
  PegNestFrame *nest_free;
  PegNestFrame *nest_all;
} PegWorker;

// The greedy seed does not use an endgame transposition table. Allocate the
// shared one only when a worker actually reaches an exact leaf so a
// short-budget PEG does not spend most of its clock zeroing a table it will
// never consult. Each worker takes the mutex once, then uses its cached copy.
// The others are waiting for the table meanwhile, so it is zeroed on as many
// threads as the solve has, rather than on the creator alone.
static TranspositionTable *peg_worker_get_endgame_tt(PegWorker *worker) {
  if (worker->eg_tt == NULL) {
    PegSharedEndgameTT *shared = worker->shared_eg_tt;
    cpthread_mutex_lock(&shared->mutex);
    if (shared->tt == NULL) {
      assert(shared->fraction > 0.0);
      shared->tt = transposition_table_create_with_threads(
          shared->fraction, PEG_TT_MIN_SIZE_POWER, shared->zero_threads);
    }
    worker->eg_tt = shared->tt;
    cpthread_mutex_unlock(&shared->mutex);
  }
  return worker->eg_tt;
}

// Acquire a scratch frame from the worker's free-list (allocating if empty);
// release returns it. Single-thread-per-worker, so no locking.
static PegNestFrame *peg_nest_acquire(PegWorker *worker) {
  PegNestFrame *frame = worker->nest_free;
  if (frame != NULL) {
    worker->nest_free = frame->free_next;
    return frame;
  }
  frame = malloc_or_die(sizeof(*frame));
  frame->game = NULL;
  frame->moves = NULL;
  frame->all_next = worker->nest_all;
  worker->nest_all = frame;
  return frame;
}

static void peg_nest_release(PegWorker *worker, PegNestFrame *frame) {
  frame->free_next = worker->nest_free;
  worker->nest_free = frame;
}

// ----- live poll -----------------------------------------------------------
// Mutex-guarded leaderboard the solver refreshes while running; a separate
// thread reads consistent snapshots via peg_poll_read. Caller-owned (see
// peg.h). All update helpers are no-ops when poll == NULL.
struct PegPoll {
  cpthread_mutex_t mutex;
  PegPollSnapshot s;
  // Per-candidate outcomes for the live status display, deep-copied in at each
  // stage boundary and grown/replaced as candidates reach deeper fidelity. Kept
  // off the fixed-size snapshot (the row count per candidate is up to
  // (RACK_SIZE+PEG_MAX_BAG)!/RACK_SIZE! = 7920 for 4-in-bag) and owned here;
  // each PegCandOutcomes.rows is heap-allocated.
  PegCandOutcomes *outcomes;
  int n_outcomes;
  // The mover draws of the candidate in progress (peg_poll_start_scenarios),
  // and the solve's workers, whose endgames a read reports for the running
  // draws. workers is set only while they exist (peg_poll_attach_workers), and
  // read only under the mutex, so a read never follows it into freed memory.
  PegDrawStatus *draws;
  int n_draws;
  int draws_cap;
  const PegWorker *workers;
  int n_workers;
  // The candidates solved at once, one per thread: cands[worker_idx] is the
  // one that worker is solving, if its start_ns is set. Sized to the workers
  // while they are attached.
  PegCandStatus *cands;
  int n_cands;
  // Every candidate the current stage has finished, in finishing order, from
  // which a read ranks the solved ones (peg_poll_fill_solved).
  PegRankedCand *finished;
  int n_finished;
  int finished_cap;
};

void peg_scenario_rows_destroy(PegPerScenario *rows, int count) {
  for (int row_idx = 0; row_idx < count; row_idx++) {
    free(rows[row_idx].pv);
  }
  free(rows);
}

static PegPerScenario *peg_scenario_rows_copy(const PegPerScenario *rows,
                                              int count) {
  if (!count) {
    return NULL;
  }
  PegPerScenario *copy = malloc_or_die((size_t)count * sizeof(*copy));
  memcpy(copy, rows, (size_t)count * sizeof(*copy));
  for (int row_idx = 0; row_idx < count; row_idx++) {
    copy[row_idx].pv = string_duplicate_allow_null(rows[row_idx].pv);
  }
  return copy;
}

void peg_cand_outcomes_destroy_array(PegCandOutcomes *arr, int n) {
  if (arr == NULL) {
    return;
  }
  for (int cand_idx = 0; cand_idx < n; cand_idx++) {
    peg_scenario_rows_destroy(arr[cand_idx].rows, arr[cand_idx].n_rows);
  }
  free(arr);
}

// Deep-copies src[0..n) into a fresh PegCandOutcomes array (rows duplicated).
// Returns NULL when n <= 0 or src is NULL.
static PegCandOutcomes *peg_cand_outcomes_dup_array(const PegCandOutcomes *src,
                                                    int n) {
  if (src == NULL || n <= 0) {
    return NULL;
  }
  PegCandOutcomes *copy = malloc_or_die((size_t)n * sizeof(PegCandOutcomes));
  for (int cand_idx = 0; cand_idx < n; cand_idx++) {
    copy[cand_idx] = src[cand_idx];
    copy[cand_idx].rows =
        peg_scenario_rows_copy(src[cand_idx].rows, src[cand_idx].n_rows);
  }
  return copy;
}

PegPoll *peg_poll_create(void) {
  PegPoll *poll = malloc_or_die(sizeof(*poll));
  cpthread_mutex_init(&poll->mutex);
  memset(&poll->s, 0, sizeof(poll->s));
  poll->s.stage = -1;
  poll->outcomes = NULL;
  poll->n_outcomes = 0;
  poll->draws = NULL;
  poll->n_draws = 0;
  poll->draws_cap = 0;
  poll->workers = NULL;
  poll->n_workers = 0;
  poll->cands = NULL;
  poll->n_cands = 0;
  poll->finished = NULL;
  poll->n_finished = 0;
  poll->finished_cap = 0;
  return poll;
}

void peg_poll_reset(PegPoll *poll) {
  if (poll == NULL) {
    return;
  }
  cpthread_mutex_lock(&poll->mutex);
  memset(&poll->s, 0, sizeof(poll->s));
  poll->s.stage = -1;
  PegCandOutcomes *old = poll->outcomes;
  const int old_n = poll->n_outcomes;
  poll->outcomes = NULL;
  poll->n_outcomes = 0;
  poll->n_draws = 0;
  poll->n_finished = 0;
  for (int cand_idx = 0; cand_idx < poll->n_cands; cand_idx++) {
    poll->cands[cand_idx].start_ns = 0;
  }
  cpthread_mutex_unlock(&poll->mutex);
  peg_cand_outcomes_destroy_array(old, old_n);
}

void peg_poll_destroy(PegPoll *poll) {
  if (poll == NULL) {
    return;
  }
  peg_cand_outcomes_destroy_array(poll->outcomes, poll->n_outcomes);
  free(poll->draws);
  free(poll->cands);
  free(poll->finished);
  // No cpthread_mutex_destroy: project mutexes don't dynamically allocate.
  free(poll);
}

// Reads the endgame `worker` is searching into a running draw's status. An
// endgame takes helpers only with more than one thread, and only then opens
// the window this checks, so with one thread nothing is read. A worker that
// waits on a nested batch helps with other jobs, so for a pass's draw this
// can be one of those jobs' endgames: it is for display only.
static void peg_poll_read_worker_endgame(const PegWorker *worker,
                                         PegDrawStatus *draw) {
  EndgameCtx *eg_ctx = worker->eg_ctx;
  uint64_t nodes = 0;
  int live_workers = 0;
  if (eg_ctx == NULL ||
      !endgame_ctx_read_live_workers(eg_ctx, &nodes, &live_workers)) {
    return;
  }
  int ply2_done = 0;
  int ply2_total = 0;
  draw->in_endgame = true;
  endgame_ctx_get_progress(eg_ctx, &draw->depth, &draw->root_moves_done,
                           &draw->root_moves_total, &ply2_done, &ply2_total);
  draw->nodes = nodes;
  draw->helpers = live_workers - 1;
}

// Fills out's draws of the candidate in progress from the poll's: the counts
// in each state, the done ones' orderings, and the rows, every running draw
// first, then finished ones while there is room, kept in solve order. Called
// under the mutex.
static void peg_poll_fill_draws(const PegPoll *poll, PegPollSnapshot *out) {
  int running = 0;
  for (int draw_idx = 0; draw_idx < poll->n_draws; draw_idx++) {
    if (poll->draws[draw_idx].state == PEG_DRAW_RUNNING) {
      running++;
    }
  }
  int finished_room = PEG_POLL_MAX_DRAW_ROWS - running;
  for (int draw_idx = 0; draw_idx < poll->n_draws; draw_idx++) {
    const PegDrawStatus *draw = &poll->draws[draw_idx];
    bool listed = false;
    switch (draw->state) {
    case PEG_DRAW_QUEUED:
      out->draws_queued++;
      break;
    case PEG_DRAW_RUNNING:
      out->draws_running++;
      listed = true;
      break;
    case PEG_DRAW_DONE:
      out->draws_done++;
      out->draws_done_weight += draw->weight_sum;
      out->draws_done_wins += draw->win_count;
      out->draws_done_ties += draw->tie_count;
      listed = finished_room-- > 0;
      break;
    case PEG_DRAW_STOPPED:
      out->draws_stopped++;
      listed = finished_room-- > 0;
      break;
    }
    if (!listed || out->n_draw_rows >= PEG_POLL_MAX_DRAW_ROWS) {
      continue;
    }
    PegDrawStatus *row = &out->draw_rows[out->n_draw_rows++];
    *row = *draw;
    if (draw->state == PEG_DRAW_RUNNING && poll->workers != NULL &&
        draw->worker_idx >= 0 && draw->worker_idx < poll->n_workers) {
      peg_poll_read_worker_endgame(&poll->workers[draw->worker_idx], row);
    }
  }
}

// Fills out's candidates solved at once from the poll's, longest running
// first, each running draw with the endgame its worker is searching. Called
// under the mutex.
static void peg_poll_fill_cands(const PegPoll *poll, PegPollSnapshot *out) {
  for (int worker_idx = 0; worker_idx < poll->n_cands; worker_idx++) {
    const PegCandStatus *cand = &poll->cands[worker_idx];
    if (cand->start_ns == 0) {
      continue;
    }
    int row_idx = out->n_cand_rows;
    if (row_idx == PEG_POLL_MAX_CAND_ROWS) {
      if (cand->start_ns >= out->cand_rows[row_idx - 1].start_ns) {
        continue;
      }
      row_idx--;
    } else {
      out->n_cand_rows++;
    }
    while (row_idx > 0 &&
           out->cand_rows[row_idx - 1].start_ns > cand->start_ns) {
      out->cand_rows[row_idx] = out->cand_rows[row_idx - 1];
      row_idx--;
    }
    out->cand_rows[row_idx] = *cand;
    if (cand->draw.state == PEG_DRAW_RUNNING && poll->workers != NULL &&
        worker_idx < poll->n_workers) {
      peg_poll_read_worker_endgame(&poll->workers[worker_idx],
                                   &out->cand_rows[row_idx].draw);
    }
  }
}

// Lets a read report the endgames of `workers` (n_workers of them) for the
// running draws, until called again with NULL before they are freed.
static void peg_poll_attach_workers(PegPoll *poll, const PegWorker *workers,
                                    int n_workers) {
  if (poll == NULL) {
    return;
  }
  cpthread_mutex_lock(&poll->mutex);
  poll->workers = workers;
  poll->n_workers = n_workers;
  if (n_workers > poll->n_cands) {
    poll->cands =
        realloc_or_die(poll->cands, (size_t)n_workers * sizeof(PegCandStatus));
  }
  poll->n_cands = n_workers;
  for (int worker_idx = 0; worker_idx < n_workers; worker_idx++) {
    poll->cands[worker_idx].start_ns = 0;
  }
  cpthread_mutex_unlock(&poll->mutex);
}

// Replaces the poll's live outcomes with a deep copy of src[0..n). Called at
// each stage boundary; values change as candidates deepen. src == NULL clears.
void peg_poll_set_outcomes(PegPoll *poll, const PegCandOutcomes *src, int n) {
  if (poll == NULL) {
    return;
  }
  // Build the copy outside the lock to keep the hold time short.
  PegCandOutcomes *copy = peg_cand_outcomes_dup_array(src, n);
  const int copy_n = (copy != NULL) ? n : 0;
  cpthread_mutex_lock(&poll->mutex);
  PegCandOutcomes *old = poll->outcomes;
  const int old_n = poll->n_outcomes;
  poll->outcomes = copy;
  poll->n_outcomes = copy_n;
  cpthread_mutex_unlock(&poll->mutex);
  peg_cand_outcomes_destroy_array(old, old_n);
}

static bool peg_move_eq(const Move *a, const Move *b) {
  if (move_get_type(a) != move_get_type(b) ||
      move_get_dir(a) != move_get_dir(b) ||
      move_get_row_start(a) != move_get_row_start(b) ||
      move_get_col_start(a) != move_get_col_start(b) ||
      move_get_tiles_length(a) != move_get_tiles_length(b) ||
      move_get_tiles_played(a) != move_get_tiles_played(b)) {
    return false;
  }
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(a); tile_idx++) {
    if (move_get_tile(a, tile_idx) != move_get_tile(b, tile_idx)) {
      return false;
    }
  }
  return true;
}

static void peg_poll_upsert_outcomes(PegPoll *poll,
                                     const PegCandOutcomes *one) {
  if (!poll || !one) {
    return;
  }
  PegCandOutcomes *copy = peg_cand_outcomes_dup_array(one, 1);
  cpthread_mutex_lock(&poll->mutex);
  // Find by the same board placement identity as the candidate ranking.
  int index = 0;
  for (; index < poll->n_outcomes; index++) {
    if (peg_move_eq(&poll->outcomes[index].move, &one->move)) {
      break;
    }
  }
  if (index == poll->n_outcomes) {
    poll->outcomes = realloc_or_die(
        poll->outcomes, (size_t)(++poll->n_outcomes) * sizeof(*poll->outcomes));
  } else {
    peg_scenario_rows_destroy(poll->outcomes[index].rows,
                              poll->outcomes[index].n_rows);
  }
  poll->outcomes[index] = *copy;
  cpthread_mutex_unlock(&poll->mutex);
  free(copy);
}

// Deep-copies the poll's current outcomes into a fresh array (caller owns; free
// with peg_cand_outcomes_destroy_array). *out is NULL when there are none.
void peg_poll_copy_outcomes(PegPoll *poll, PegCandOutcomes **out, int *n_out) {
  *out = NULL;
  *n_out = 0;
  if (poll == NULL) {
    return;
  }
  cpthread_mutex_lock(&poll->mutex);
  *out = peg_cand_outcomes_dup_array(poll->outcomes, poll->n_outcomes);
  *n_out = (*out != NULL) ? poll->n_outcomes : 0;
  cpthread_mutex_unlock(&poll->mutex);
}

// Orders candidates by results: win% (wins plus half the draws) first, and
// mean spread, when both are known, only to break an exact tie. Two candidates
// scored over the same labeled orderings are compared exactly, in those integer
// units; otherwise (a sampled, unfinished or cut candidate) win% is compared
// with a tolerance for floating-point noise. Returns < 0 when a ranks above b,
// > 0 when below.
static int peg_cand_compare(const PegRankedCand *a, const PegRankedCand *b) {
  if (a->weight_sum > 0 && a->weight_sum == b->weight_sum && !a->cut &&
      !b->cut) {
    const int64_t a_score = 2 * a->win_count + a->tie_count;
    const int64_t b_score = 2 * b->win_count + b->tie_count;
    if (a_score != b_score) {
      return a_score > b_score ? -1 : 1;
    }
  } else if (a->win_pct - b->win_pct > PEG_WIN_PCT_TIE_TOLERANCE) {
    return -1;
  } else if (b->win_pct - a->win_pct > PEG_WIN_PCT_TIE_TOLERANCE) {
    return 1;
  }
  if (!a->spread_unknown && !b->spread_unknown &&
      a->mean_spread != b->mean_spread) {
    return a->mean_spread > b->mean_spread ? -1 : 1;
  }
  return 0;
}

// Fills out's solved rows from the stage's finished candidates: the best
// uncut ones, best first, each ranked among all of them. Called under the
// mutex.
static void peg_poll_fill_solved(const PegPoll *poll, PegPollSnapshot *out) {
  if (out->fidelity_plies == 0) {
    return;
  }
  for (int finished_idx = 0; finished_idx < poll->n_finished; finished_idx++) {
    const PegRankedCand *cand = &poll->finished[finished_idx];
    if (cand->cut) {
      continue;
    }
    int row_idx = out->n_solved_rows;
    if (row_idx == PEG_POLL_MAX_SOLVED_ROWS) {
      if (peg_cand_compare(cand, &out->solved_rows[row_idx - 1]) >= 0) {
        continue;
      }
      row_idx--;
    } else {
      out->n_solved_rows++;
    }
    while (row_idx > 0 &&
           peg_cand_compare(&out->solved_rows[row_idx - 1], cand) > 0) {
      out->solved_rows[row_idx] = out->solved_rows[row_idx - 1];
      row_idx--;
    }
    out->solved_rows[row_idx] = *cand;
  }
  for (int row_idx = 0; row_idx < out->n_solved_rows; row_idx++) {
    int rank = 1;
    for (int finished_idx = 0; finished_idx < poll->n_finished;
         finished_idx++) {
      if (peg_cand_compare(&poll->finished[finished_idx],
                           &out->solved_rows[row_idx]) < 0) {
        rank++;
      }
    }
    out->solved_ranks[row_idx] = rank;
  }
}

void peg_poll_read(PegPoll *poll, PegPollSnapshot *out) {
  cpthread_mutex_lock(&poll->mutex);
  *out = poll->s;
  peg_poll_fill_draws(poll, out);
  peg_poll_fill_cands(poll, out);
  peg_poll_fill_solved(poll, out);
  cpthread_mutex_unlock(&poll->mutex);
}

// The current (still-running) stage-history entry, or NULL when there is none:
// either no stage has begun yet, or the history is full (PEG_POLL_MAX_STAGES)
// and its last entry has already been finalized (end_ns != 0). All live stage
// progress -- end_ns, cands_done, best_win_pct -- must be written only through
// this, so an overflowed history never corrupts an already-ended stage. The
// caller must hold poll->mutex.
static PegStageSnapshot *peg_poll_open_stage(PegPollSnapshot *snap) {
  if (snap->n_stage_history > 0 &&
      snap->stage_history[snap->n_stage_history - 1].end_ns == 0) {
    return &snap->stage_history[snap->n_stage_history - 1];
  }
  return NULL;
}

// Stage-boundary metadata, set before a stage's evaluation begins.
// Finalizes the previous stage entry (sets its end_ns) and opens a new one.
static void peg_poll_begin_stage(PegPoll *poll, int stage, int fidelity_plies,
                                 int field_size) {
  if (poll == NULL) {
    return;
  }
  const int64_t now_ns = ctimer_monotonic_ns();
  cpthread_mutex_lock(&poll->mutex);
  // Finalize the previous stage: record when it ended.
  PegStageSnapshot *prev = peg_poll_open_stage(&poll->s);
  if (prev != NULL) {
    prev->end_ns = now_ns;
  }
  // Open a new stage entry if there is room.
  if (poll->s.n_stage_history < PEG_POLL_MAX_STAGES) {
    PegStageSnapshot *st = &poll->s.stage_history[poll->s.n_stage_history];
    st->fidelity_plies = fidelity_plies;
    st->field_size = field_size;
    st->cands_done = 0;
    st->cands_cut = 0;
    st->start_ns = now_ns;
    st->end_ns = 0;
    st->best_win_pct = -1.0;
    st->scenarios_done = 0;
    st->scenarios_total = 0;
    poll->s.n_stage_history++;
  }
  poll->s.stage = stage;
  poll->s.fidelity_plies = fidelity_plies;
  poll->s.field_size = field_size;
  poll->n_finished = 0;
  poll->s.version++;
  cpthread_mutex_unlock(&poll->mutex);
}

// Record the moves of every candidate this stage will score, so a live renderer
// can size the move column to the whole stage instead of growing it as cands
// finish. `moves[i]` points at the i-th candidate's move.
static void peg_poll_set_stage_moves(PegPoll *poll, const Move *const *moves,
                                     int n) {
  if (poll == NULL) {
    return;
  }
  const int entry_count = n < PEG_POLL_MAX_ENTRIES ? n : PEG_POLL_MAX_ENTRIES;
  cpthread_mutex_lock(&poll->mutex);
  for (int entry_idx = 0; entry_idx < entry_count; entry_idx++) {
    poll->s.stage_moves[entry_idx] = *moves[entry_idx];
  }
  poll->s.n_stage_moves = entry_count;
  poll->s.version++;
  cpthread_mutex_unlock(&poll->mutex);
}

// Bump the current stage's cands_done counter. Used by the halving stages,
// whose candidates are evaluated via peg_eval_candidates_scenario. Stage-0
// greedy instead counts each candidate inside peg_poll_upsert. Exactly one of
// the two paths runs per candidate, so they must stay mutually exclusive --
// otherwise a candidate is double-counted and cands_done overshoots field_size.
static void peg_poll_bump_cand_done(PegPoll *poll) {
  if (poll == NULL) {
    return;
  }
  cpthread_mutex_lock(&poll->mutex);
  PegStageSnapshot *cur = peg_poll_open_stage(&poll->s);
  if (cur != NULL) {
    cur->cands_done++;
  }
  poll->s.version++;
  cpthread_mutex_unlock(&poll->mutex);
}

// Insert one finished candidate into the descending top-K (stage-0 liveness).
// Also bumps the stage cands_done counter and updates best_win_pct. Called from
// worker threads, so it locks. Returns true iff the insert shuffled the
// displayed ranking (landed above the current bottom, or is the first entry),
// so a caller can redraw only on a meaningful change rather than every append.
static bool peg_poll_upsert(PegPoll *poll, const PegRankedCand *cand) {
  if (poll == NULL) {
    return false;
  }
  cpthread_mutex_lock(&poll->mutex);
  PegPollSnapshot *snap = &poll->s;
  const int old_n_entries = snap->n_entries;
  PegStageSnapshot *cur = peg_poll_open_stage(snap);
  // Always count this candidate as done regardless of leaderboard outcome,
  // and keep it among the stage's finished candidates.
  if (cur != NULL) {
    cur->cands_done++;
    if (cand->cut) {
      cur->cands_cut++;
    }
  }
  if (poll->n_finished == poll->finished_cap) {
    poll->finished_cap = poll->finished_cap > 0 ? 2 * poll->finished_cap : 64;
    poll->finished = realloc_or_die(poll->finished, (size_t)poll->finished_cap *
                                                        sizeof(PegRankedCand));
  }
  poll->finished[poll->n_finished++] = *cand;
  int i;
  if (snap->n_entries < PEG_POLL_MAX_ENTRIES) {
    i = snap->n_entries++;
  } else if (peg_cand_compare(cand, &snap->entries[PEG_POLL_MAX_ENTRIES - 1]) <
             0) {
    i = PEG_POLL_MAX_ENTRIES - 1;
  } else {
    snap->version++;
    cpthread_mutex_unlock(&poll->mutex);
    return false; // full and outranked — still counted done, but not inserted
  }
  while (i > 0 && peg_cand_compare(&snap->entries[i - 1], cand) > 0) {
    snap->entries[i] = snap->entries[i - 1];
    i--;
  }
  snap->entries[i] = *cand;
  // entries[0] is always the best; reflect in the current stage.
  if (cur != NULL) {
    cur->best_win_pct = snap->entries[0].win_pct;
  }
  snap->version++;
  // A simple bottom append (reordered == false) only when the list actually
  // grew. If it was already full, a bottom insert evicted the previous worst
  // entry without changing the index, so the displayed content changed and a
  // streaming renderer must redraw -- treat that as reordered.
  const bool grew = snap->n_entries > old_n_entries;
  const bool reordered = old_n_entries == 0 || i < snap->n_entries - 1 || !grew;
  cpthread_mutex_unlock(&poll->mutex);
  return reordered;
}

// Replace the leaderboard with an authoritative ranking at a stage boundary.
// Also records the best win% for the current stage history entry.
static void peg_poll_replace(PegPoll *poll, const PegRankedCand *ranked, int n,
                             int stage, int fidelity_plies, int field_size) {
  if (poll == NULL) {
    return;
  }
  const int k = n < PEG_POLL_MAX_ENTRIES ? n : PEG_POLL_MAX_ENTRIES;
  cpthread_mutex_lock(&poll->mutex);
  for (int i = 0; i < k; i++) {
    poll->s.entries[i] = ranked[i];
  }
  poll->s.n_entries = k;
  poll->s.stage = stage;
  poll->s.fidelity_plies = fidelity_plies;
  poll->s.field_size = field_size;
  PegStageSnapshot *cur = peg_poll_open_stage(&poll->s);
  if (cur != NULL && k > 0) {
    cur->best_win_pct = ranked[0].win_pct;
  }
  poll->s.version++;
  cpthread_mutex_unlock(&poll->mutex);
}

// Empty the live leaderboard. Used at the start of a halving stage in live
// (poll) mode so the per-candidate upserts below show only the candidates
// evaluated so far in THIS stage, not the previous stage's carried-over
// ranking. The current entries are saved as the baseline so status_peg can
// build a cross-depth merged display while the new stage runs.
static void peg_poll_clear_entries(PegPoll *poll) {
  if (poll == NULL) {
    return;
  }
  cpthread_mutex_lock(&poll->mutex);
  // Save completed non-greedy stages to history for per-depth time columns.
  if (poll->s.fidelity_plies > 0 &&
      poll->s.n_history_stages < PEG_POLL_MAX_HISTORY_STAGES) {
    const int slot = poll->s.n_history_stages++;
    poll->s.history_fidelities[slot] = poll->s.fidelity_plies;
    const int n = poll->s.n_entries < PEG_POLL_MAX_ENTRIES
                      ? poll->s.n_entries
                      : PEG_POLL_MAX_ENTRIES;
    poll->s.history_n_cands[slot] = n;
    for (int cand_idx = 0; cand_idx < n; cand_idx++) {
      poll->s.history_cands[slot][cand_idx].move =
          poll->s.entries[cand_idx].move;
      poll->s.history_cands[slot][cand_idx].eval_seconds =
          poll->s.entries[cand_idx].eval_seconds;
    }
  }
  // Save as baseline for the cross-depth merged candidate view.
  poll->s.n_baseline_entries = poll->s.n_entries;
  poll->s.baseline_fidelity = poll->s.fidelity_plies;
  const int k = poll->s.n_entries < PEG_POLL_MAX_ENTRIES ? poll->s.n_entries
                                                         : PEG_POLL_MAX_ENTRIES;
  for (int baseline_idx = 0; baseline_idx < k; baseline_idx++) {
    poll->s.baseline_entries[baseline_idx] = poll->s.entries[baseline_idx];
  }
  poll->s.n_entries = 0;
  poll->s.version++;
  cpthread_mutex_unlock(&poll->mutex);
}

// Mark the candidate `move`, the stage's move_idx-th, as currently in-flight so
// the status display can show a live elapsed time and '*' depth marker. Call
// with move_idx = -1 / start_ns = 0 / move = NULL to clear (no candidate in
// flight), which also forgets the finished candidate's draws.
static void peg_poll_set_evaluating(PegPoll *poll, int move_idx,
                                    int64_t start_ns, const Move *move) {
  if (poll == NULL) {
    return;
  }
  cpthread_mutex_lock(&poll->mutex);
  poll->s.currently_evaluating_move_idx = move_idx;
  poll->s.eval_start_ns = start_ns;
  if (move != NULL) {
    poll->s.evaluating_move = *move;
  } else {
    poll->n_draws = 0;
  }
  poll->s.version++;
  cpthread_mutex_unlock(&poll->mutex);
}

// Starts (move non-NULL) or ends (NULL) the candidate worker_idx solves at
// once with others, and counts it among those running.
static void peg_poll_set_cand(PegPoll *poll, int worker_idx, const Move *move) {
  if (poll == NULL) {
    return;
  }
  cpthread_mutex_lock(&poll->mutex);
  poll->s.cands_running += move != NULL ? 1 : -1;
  if (worker_idx < poll->n_cands) {
    PegCandStatus *cand = &poll->cands[worker_idx];
    if (move != NULL) {
      memset(cand, 0, sizeof(*cand));
      cand->move = *move;
      cand->start_ns = ctimer_monotonic_ns();
      cand->draw.state = PEG_DRAW_QUEUED;
      cand->draw.worker_idx = worker_idx;
    } else {
      cand->start_ns = 0;
    }
  }
  poll->s.version++;
  cpthread_mutex_unlock(&poll->mutex);
}

// Records how many draws the candidate worker_idx solves at once has.
static void peg_poll_set_cand_draws(PegPoll *poll, int worker_idx,
                                    int n_draws) {
  if (poll == NULL) {
    return;
  }
  cpthread_mutex_lock(&poll->mutex);
  if (worker_idx < poll->n_cands) {
    poll->cands[worker_idx].draws_total = n_draws;
  }
  cpthread_mutex_unlock(&poll->mutex);
}

static void peg_poll_finish(PegPoll *poll, bool can_resume) {
  if (poll == NULL) {
    return;
  }
  const int64_t now_ns = ctimer_monotonic_ns();
  cpthread_mutex_lock(&poll->mutex);
  // Finalize the last stage.
  PegStageSnapshot *cur = peg_poll_open_stage(&poll->s);
  if (cur != NULL) {
    cur->end_ns = now_ns;
  }
  poll->s.can_resume = can_resume;
  poll->s.done = true;
  poll->s.version++;
  cpthread_mutex_unlock(&poll->mutex);
}

// ----- position setup ------------------------------------------------------

// Tiles not visible to the mover: full distribution minus mover's rack minus
// the board. Returns the total count.
static int peg_compute_unseen(const Game *game, int mover_idx,
                              uint8_t unseen[MAX_ALPHABET_SIZE]) {
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
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
      const MachineLetter ml = board_get_letter(board, row, col);
      if (get_is_blanked(ml)) {
        if (unseen[BLANK_MACHINE_LETTER] > 0) {
          unseen[BLANK_MACHINE_LETTER]--;
        }
      } else if (unseen[ml] > 0) {
        unseen[ml]--;
      }
    }
  }
  int total = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    total += unseen[ml];
  }
  return total;
}

// Set opp's rack to (unseen minus the bag tiles) — i.e. the tiles opp must
// be holding once the bag is fixed to `bag_tiles`.
static void peg_set_opp_rack(Rack *opp_rack,
                             const uint8_t unseen[MAX_ALPHABET_SIZE],
                             int ld_size, const MachineLetter *bag_tiles,
                             int n_bag) {
  uint8_t remaining[MAX_ALPHABET_SIZE];
  for (int ml = 0; ml < ld_size; ml++) {
    remaining[ml] = unseen[ml];
  }
  for (int i = 0; i < n_bag; i++) {
    // bag_tiles[0..n_bag) are machine letters in [0, ld_size); remaining[] is
    // initialized for that range by the loop above.
    // NOLINTNEXTLINE(clang-analyzer-core.uninitialized.ArraySubscript)
    if (remaining[bag_tiles[i]] > 0) {
      remaining[bag_tiles[i]]--;
    }
  }
  rack_reset(opp_rack);
  for (int ml = 0; ml < ld_size; ml++) {
    for (int i = 0; i < remaining[ml]; i++) {
      rack_add_letter(opp_rack, (MachineLetter)ml);
    }
  }
}

// Build the per-candidate template once: copy the prepared base (which carries
// the root pruned KWG override), play the cand, and generate cross-sets for the
// post-cand board with that pruned KWG — fast, and shared by all of this cand's
// scenarios (the board, hence the cross-sets, is identical across them). This
// keeps both the cand play and the cross-set generation out of the per-scenario
// loop. Leaves the mover's leave on its rack; per-scenario draws are added
// later.
static void peg_build_template_into(Game **slot, const Game *prepared_base,
                                    const Move *cand) {
  if (*slot == NULL) {
    *slot = game_duplicate(prepared_base);
  } else {
    game_copy(*slot, prepared_base);
  }
  play_move_without_drawing_tiles(cand, *slot);
  game_gen_all_cross_sets(*slot);
}

static void peg_build_template(PegWorker *worker, const Game *prepared_base,
                               const Move *cand) {
  peg_build_template_into(&worker->template_game, prepared_base, cand);
}

// Build the post-cand game for one (mover_drawn, bag_remaining) split by
// copying the per-cand template (which already has the cand played, cross-sets,
// and the pruned-KWG override) and resetting only the racks/bag: bag holds
// (mover_drawn
// ++ bag_remaining), opp rack is unseen minus the bag, and the mover draws
// their k_drawn tiles onto the leave. Returns the scratch game (worker-owned;
// not destroyed per call). Racks/bag don't affect cross-sets, so the copied
// ones stay valid and the leaf endgame can skip regenerating them.
static Game *peg_make_post_cand_game_into(Game **slot, const Game *template_src,
                                          int mover_idx, const uint8_t *unseen,
                                          int ld_size, int k_drawn,
                                          const MachineLetter *mover_drawn,
                                          int n_bag_remaining,
                                          const MachineLetter *bag_remaining,
                                          const uint8_t *known_opp) {
  if (*slot == NULL) {
    *slot = game_duplicate(template_src);
  } else {
    game_copy(*slot, template_src);
  }
  Game *game = *slot;
  Bag *bag = game_get_bag(game);
  Rack *opp_rack = player_get_rack(game_get_player(game, 1 - mover_idx));
  Rack *mover_rack = player_get_rack(game_get_player(game, mover_idx));

  // opp rack = unseen minus the original K-tile bag (mover_drawn ++
  // bag_remaining); the mover would have drawn mover_drawn off the top.
  MachineLetter all_bag[PEG_SCENARIO_ARRAY_CAP + 1];
  const int n_bag = k_drawn + n_bag_remaining;
  for (int i = 0; i < k_drawn; i++) {
    all_bag[i] = mover_drawn[i];
  }
  for (int i = 0; i < n_bag_remaining; i++) {
    all_bag[k_drawn + i] = bag_remaining[i];
  }
  peg_set_opp_rack(opp_rack, unseen, ld_size, all_bag, n_bag);
  // `unseen` left out the opponent's known tiles: it holds them in every
  // scenario.
  if (known_opp != NULL) {
    for (int ml = 0; ml < ld_size; ml++) {
      for (int count = 0; count < known_opp[ml]; count++) {
        rack_add_letter(opp_rack, (MachineLetter)ml);
      }
    }
  }
  // Set the bag directly to this scenario's leftover ordering and add the
  // mover's drawn tiles to the leave. We deliberately do NOT lay out the full
  // K-tile bag and bag_draw_letter the mover tiles back off: that draw swaps
  // each removed tile to the bag end, scrambling bag_remaining's order. The
  // caller (peg_eval_split) enumerates the *distinct orderings* of
  // bag_remaining and averages the leaf over them, so the bag must hold exactly
  // the passed ordering — otherwise the scramble could collapse two enumerated
  // orderings onto the same realized one (when the mover drew a tile whose
  // value is also in the bag), double-counting one ordering and dropping
  // another.
  bag_set_to_tiles(bag, bag_remaining, n_bag_remaining);
  for (int i = 0; i < k_drawn; i++) {
    rack_add_letter(mover_rack, mover_drawn[i]);
  }
  // Playing the cand in the template may have flagged GAME_END_REASON_STANDARD
  // (rack emptied in the no-draw world). We re-stock here, so clear the stale
  // flag unless the rack is genuinely empty.
  if (!rack_is_empty(mover_rack)) {
    game_set_game_end_reason(game, GAME_END_REASON_NONE);
  }
  return game;
}

static Game *peg_make_post_cand_game(PegWorker *worker,
                                     const Game *template_src, int mover_idx,
                                     const uint8_t *unseen, int ld_size,
                                     int k_drawn,
                                     const MachineLetter *mover_drawn,
                                     int n_bag_remaining,
                                     const MachineLetter *bag_remaining) {
  return peg_make_post_cand_game_into(
      &worker->scratch_game, template_src, mover_idx, unseen, ld_size, k_drawn,
      mover_drawn, n_bag_remaining, bag_remaining, worker->known_opp);
}

static bool peg_should_stop(int64_t deadline_ns,
                            ThreadControl *thread_control) {
  if (deadline_ns != 0 && ctimer_monotonic_ns() >= deadline_ns) {
    return true;
  }
  // A spent movegen budget stops it as its deadline would.
  if (gen_movegen_budget_spent()) {
    return true;
  }
  return thread_control != NULL &&
         thread_control_search_checkpoint(thread_control) ==
             THREAD_CONTROL_STATUS_USER_INTERRUPT;
}

// Greedy playout to game end; returns signed mover spread (points), with the
// usual rack-leave adjustment when the game has not actually ended. A timed
// playout checks between plies so an in-flight PEG scenario can wind down at
// the same absolute deadline as the surrounding stage. shared_plies, when
// non-NULL, holds a PegSharedPly (or NULL) for each of the first
// PEG_SHARED_PLAYOUT_PLIES plies: a known one is played without generating, and
// an unknown one records the generation's pick.
static int32_t peg_greedy_playout(Game *game, int mover_idx,
                                  MoveList *playout_ml, int64_t deadline_ns,
                                  ThreadControl *thread_control,
                                  bool *interrupted,
                                  PegSharedPly *const *shared_plies) {
  const LetterDistribution *ld = game_get_ld(game);
  for (int ply = 0; ply < PEG_PLAYOUT_MAX_PLIES; ply++) {
    if (peg_should_stop(deadline_ns, thread_control)) {
      if (interrupted != NULL) {
        *interrupted = true;
      }
      break;
    }
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      break;
    }
    PegSharedPly *shared =
        shared_plies != NULL && ply < PEG_SHARED_PLAYOUT_PLIES
            ? shared_plies[ply]
            : NULL;
    if (shared != NULL && shared->known) {
      if (shared->none) {
        break;
      }
      play_move(&shared->move, game, NULL);
      continue;
    }
    const bool bag_has_tiles = bag_get_letters(game_get_bag(game)) > 0;
    const MoveGenArgs args = {
        .game = game,
        .move_record_type = MOVE_RECORD_BEST,
        .move_sort_type = bag_has_tiles ? MOVE_SORT_EQUITY : MOVE_SORT_SCORE,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .move_list = playout_ml,
        .tiles_played_bv = NULL,
        .initial_tiles_bv = 0,
    };
    generate_moves(&args);
    if (peg_should_stop(deadline_ns, thread_control)) {
      if (interrupted != NULL) {
        *interrupted = true;
      }
      break;
    }
    if (move_list_get_count(playout_ml) == 0) {
      if (shared != NULL) {
        shared->none = true;
        shared->known = true;
      }
      break;
    }
    const Move *best = move_list_get_move(playout_ml, 0);
    if (shared != NULL) {
      shared->move = *best;
      shared->none = false;
      shared->known = true;
    }
    play_move(best, game, NULL);
  }
  const Player *me = game_get_player(game, mover_idx);
  const Player *op = game_get_player(game, 1 - mover_idx);
  int32_t spread = equity_to_int(player_get_score(me) - player_get_score(op));
  if (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    spread -= equity_to_int(rack_get_score(ld, player_get_rack(me)));
    spread += equity_to_int(rack_get_score(ld, player_get_rack(op)));
  }
  return spread;
}

// Starts a new candidate: forgets every reply the cache holds.
static void peg_reply_cache_reset(PegReplyCache *cache) {
  cache->stamp++;
  cache->count = 0;
}

// The opponent-reply slot for the opponent rack `letters` (n_letters of them,
// ascending), created empty on first use. Returns NULL once half the slots are
// taken, so probes stay short; the caller then shares the reply only within
// its own split.
static PegSharedPly *peg_reply_cache_slot(PegReplyCache *cache,
                                          const MachineLetter *letters,
                                          int n_letters) {
  uint64_t hash = fnv64a_step(FNV_64_OFFSET_BASIS, (uint64_t)n_letters);
  for (int letter_idx = 0; letter_idx < n_letters; letter_idx++) {
    hash = fnv64a_step(hash, letters[letter_idx]);
  }
  uint64_t slot_idx = hash & (PEG_REPLY_CACHE_SLOTS - 1);
  for (;;) {
    PegReplyCacheEntry *entry = &cache->entries[slot_idx];
    if (entry->stamp != cache->stamp) {
      if (2 * cache->count >= PEG_REPLY_CACHE_SLOTS) {
        return NULL;
      }
      entry->stamp = cache->stamp;
      entry->n_letters = n_letters;
      memcpy(entry->letters, letters,
             (size_t)n_letters * sizeof(MachineLetter));
      entry->reply.known = false;
      cache->count++;
      return &entry->reply;
    }
    if (entry->n_letters == n_letters &&
        memcmp(entry->letters, letters,
               (size_t)n_letters * sizeof(MachineLetter)) == 0) {
      return &entry->reply;
    }
    slot_idx = (slot_idx + 1) & (PEG_REPLY_CACHE_SLOTS - 1);
  }
}

// Pessimistic playout: the mover plays greedily, but at each opponent turn the
// opponent plays the reply that minimizes the mover's final spread — a 1-ply
// adversarial choice scored by a greedy rollout of the remainder. This is the
// PEG_OPP_PESSIMISTIC leaf for non-emptier scenarios; its mover spread is
// always
// <= the greedy (rational) playout's. Returns the signed mover spread with the
// same rack-leave adjustment as peg_greedy_playout.
static int32_t peg_pessimistic_playout(Game *game, int mover_idx,
                                       MoveList *playout_ml, int inner_top_k,
                                       int64_t deadline_ns,
                                       ThreadControl *thread_control,
                                       bool *interrupted) {
  const LetterDistribution *ld = game_get_ld(game);
  Game *branch = NULL;         // lazily created scratch for opp-reply trials
  MoveList *opp_ml = NULL;     // opponent's candidate replies
  MoveList *rollout_ml = NULL; // greedy rollout of each trial
  for (int ply = 0; ply < PEG_PLAYOUT_MAX_PLIES; ply++) {
    if (peg_should_stop(deadline_ns, thread_control)) {
      if (interrupted != NULL) {
        *interrupted = true;
      }
      break;
    }
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      break;
    }
    const bool bag_has_tiles = bag_get_letters(game_get_bag(game)) > 0;
    const move_sort_t sort = bag_has_tiles ? MOVE_SORT_EQUITY : MOVE_SORT_SCORE;
    if (game_get_player_on_turn_index(game) == mover_idx) {
      // Mover: greedy best move.
      const MoveGenArgs ga = {
          .game = game,
          .move_record_type = MOVE_RECORD_BEST,
          .move_sort_type = sort,
          .override_kwg = NULL,
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
          .move_list = playout_ml,
          .tiles_played_bv = NULL,
          .initial_tiles_bv = 0,
      };
      generate_moves(&ga);
      if (peg_should_stop(deadline_ns, thread_control)) {
        if (interrupted != NULL) {
          *interrupted = true;
        }
        break;
      }
      if (move_list_get_count(playout_ml) == 0) {
        break;
      }
      play_move(move_list_get_move(playout_ml, 0), game, NULL);
      continue;
    }
    // Opponent: enumerate replies and pick the one worst for the mover.
    if (opp_ml == NULL) {
      opp_ml = move_list_create(PEG_PESSIMISTIC_OPP_LIST_CAP);
      rollout_ml = move_list_create(1);
      branch = game_duplicate(game);
    }
    const MoveGenArgs ga = {
        .game = game,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = sort,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .move_list = opp_ml,
        .tiles_played_bv = NULL,
        .initial_tiles_bv = 0,
    };
    generate_moves(&ga);
    if (peg_should_stop(deadline_ns, thread_control)) {
      if (interrupted != NULL) {
        *interrupted = true;
      }
      break;
    }
    const int n_opp = move_list_get_count(opp_ml);
    if (n_opp == 0) {
      break;
    }
    // Inner top-K cap: a strong-but-bounded opponent weighs only its
    // inner_top_k highest-equity replies. MOVE_RECORD_ALL is unsorted, so sort
    // by equity descending first, then consider the leading prefix. 0 (the
    // default) weighs every reply (the unbounded worst-case opponent).
    int n_consider = n_opp;
    if (inner_top_k > 0 && inner_top_k < n_opp) {
      move_list_sort_moves(opp_ml);
      n_consider = inner_top_k;
    }
    int worst_idx = 0;
    int32_t worst_for_mover = INT32_MAX;
    for (int i = 0; i < n_consider; i++) {
      if (peg_should_stop(deadline_ns, thread_control)) {
        if (interrupted != NULL) {
          *interrupted = true;
        }
        break;
      }
      game_copy(branch, game);
      play_move(move_list_get_move(opp_ml, i), branch, NULL);
      const int32_t mover_net =
          peg_greedy_playout(branch, mover_idx, rollout_ml, deadline_ns,
                             thread_control, interrupted,
                             /*shared_plies=*/NULL);
      if (interrupted != NULL && *interrupted) {
        break;
      }
      if (mover_net < worst_for_mover) {
        worst_for_mover = mover_net;
        worst_idx = i;
      }
    }
    if (interrupted != NULL && *interrupted) {
      break;
    }
    play_move(move_list_get_move(opp_ml, worst_idx), game, NULL);
  }
  const Player *me = game_get_player(game, mover_idx);
  const Player *op = game_get_player(game, 1 - mover_idx);
  int32_t spread = equity_to_int(player_get_score(me) - player_get_score(op));
  if (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    spread -= equity_to_int(rack_get_score(ld, player_get_rack(me)));
    spread += equity_to_int(rack_get_score(ld, player_get_rack(op)));
  }
  if (branch != NULL) {
    game_destroy(branch);
    move_list_destroy(opp_ml);
    move_list_destroy(rollout_ml);
  }
  return spread;
}

// ----- per-candidate scenario evaluation -----------------------------------

// Optional progress callbacks bundled for threading through the worker structs.
// on_stage_start is invoked directly by peg_solve and so is not carried here.
// The on_cand_done / on_scenario_done callbacks may fire concurrently from
// worker threads; the caller is responsible for their thread-safety (see
// peg.h). stage_idx is set by peg_solve before each stage's dispatch and read
// (not written) during the stage, so it is race-free. A NULL PegProgress
// pointer, or NULL individual callbacks, disables the respective events.
typedef struct PegProgress {
  PegOnCandDone on_cand_done;
  PegOnScenarioDone on_scenario_done;
  void *user_data;
  int stage_idx;
} PegProgress;

// Optional per-scenario capture sink (PegArgs.include_per_scenario). Populated
// single-threaded during a dedicated capture pass over the published best cand,
// so it needs no locking. Rows grow on demand; ownership transfers to
// PegResult.per_scenario.
typedef struct PegScenarioCapture {
  PegPerScenario *rows;
  int count;
  int capacity;
  const LetterDistribution *ld;
  char *pending_pv;
} PegScenarioCapture;

// The exact early cutoff of one candidate (PegArgs.early_cutoff), shared by
// its scenario jobs. Results count in half-orderings: a labeled ordering won
// is worth 2, drawn 1 and lost 0, so a candidate scores 2 * wins + draws out
// of 2 * weight_sum. `deficit` sums what the orderings solved so far fell
// short of 2; once it exceeds 2 * weight_sum less the best finished
// candidate's score, the candidate can no longer reach the best and `cut` is
// set. The best is read live: candidates solved at once raise it as they
// finish, which can cut one already under way.
typedef struct PegCutoff {
  _Atomic int64_t deficit;
  int64_t full_score;                // 2 * weight_sum
  const _Atomic int64_t *best_score; // the best finished score, -1 for none
  atomic_bool cut;
} PegCutoff;

static bool peg_cutoff_over(const PegCutoff *cutoff, int64_t deficit) {
  return deficit > cutoff->full_score - atomic_load(cutoff->best_score);
}

static bool peg_cutoff_is_cut(PegCutoff *cutoff) {
  if (cutoff == NULL) {
    return false;
  }
  if (!atomic_load(&cutoff->cut) &&
      peg_cutoff_over(cutoff, atomic_load(&cutoff->deficit))) {
    atomic_store(&cutoff->cut, true);
  }
  return atomic_load(&cutoff->cut);
}

// Adds what one solved ordering of `per_ordering` labeled orderings fell short
// of a win, and cuts the candidate once it can no longer reach the best.
static void peg_cutoff_add(PegCutoff *cutoff, int64_t per_ordering,
                           int32_t value) {
  if (cutoff == NULL || value > 0) {
    return;
  }
  const int64_t shortfall = (value == 0 ? 1 : 2) * per_ordering;
  if (peg_cutoff_over(cutoff, atomic_fetch_add(&cutoff->deficit, shortfall) +
                                  shortfall)) {
    atomic_store(&cutoff->cut, true);
  }
}

// One scenario job: a single (cand, mover-draw split) unit of work. Carries the
// shared per-cand template to copy from and a result the worker fills.
// Splitting at this granularity (rather than per-cand) keeps every core fed
// even when a stage has only a couple of candidates.
typedef struct PegScenarioJob {
  const Game *template_src;
  int mover_idx;
  const uint8_t *unseen;
  int ld_size;
  int k_drawn;
  int n_bag_remaining;
  MachineLetter mover_drawn[PEG_SCENARIO_ARRAY_CAP + 1];
  MachineLetter bag_remaining[PEG_SCENARIO_ARRAY_CAP + 1];
  int64_t weight;
  PegOppModel opp_model;
  int inner_top_k;
  int fidelity_plies;
  int injection_cap;
  int64_t deadline_ns;
  ThreadControl *thread_control;
  PegWorker *workers;
  int cand_idx;
  const PegProgress *progress; // optional; cand_idx is the cand rank
  // Optional per-ordering capture (PegArgs.include_per_scenario). When
  // do_capture is set, the worker records this split's orderings into its own
  // job-local `cap` (no cross-job sharing, so no locking); the reducer
  // concatenates per candidate afterwards and frees cap.rows. `ld` formats the
  // captured tile strings.
  bool do_capture;
  const LetterDistribution *ld;
  PegScenarioCapture cap;
  // Result (filled by the worker, reduced per-cand afterwards).
  double total_weight;
  double win_weight;
  double spread_weight;
  int64_t weight_sum;
  int64_t win_count;
  int64_t tie_count;
  int n_scenarios;
  // Populated only when on_cand_done is active. A scenario-parallel candidate
  // finishes when its last scenario job finishes.
  bool completed;
  int64_t completed_ns;
  // Optional: the candidate's early cutoff (NULL = none), and the poll whose
  // current stage counts its solved scenario jobs (NULL = none).
  PegCutoff *cutoff;
  PegPoll *scenario_poll;
  // Greedy value of the split's first ordering, by which a cutoff solve
  // orders its jobs (losses first).
  int32_t greedy_value;
  // This job's index among scenario_poll's draws.
  int draw_idx;
  // The poll that lists this job's candidate among those solved at once, one
  // per thread, with the draw it is solving (NULL = none).
  PegPoll *cand_poll;
} PegScenarioJob;

// A growable list of scenario jobs (collect mode). Not recursive, so defined
// inline.
typedef struct PegScenarioJobList {
  PegScenarioJob *jobs;
  int count;
  int capacity;
} PegScenarioJobList;

typedef struct PegEvalCtx {
  const Game *base_game;
  // Source game each leaf copies (the cand's post-cand template, with the cand
  // played + cross-sets + pruned override). Shared read-only across scenario
  // workers of the same cand.
  const Game *template_src;
  int mover_idx;
  const uint8_t *unseen;
  int ld_size;
  const Move *cand;
  int bag_size;
  int k_drawn;
  // Opponent model for non-emptier leaves (see PegOppModel).
  PegOppModel opp_model;
  // Pessimistic-model inner cap: weigh only the inner_top_k highest-equity opp
  // replies per opp turn (0 = all). See PegArgs.inner_top_k.
  int inner_top_k;
  // 0 = greedy leaf (Stage 0); > 0 = emptier scenarios solved exactly with an
  // endgame_solve at this ply depth (non-emptier still uses the greedy leaf).
  int fidelity_plies;
  // Endgame max_workers ceiling: > num_threads opens the injection window so
  // the monitor can lend idle cores to this leaf's endgame solve. 0 disables.
  int injection_cap;
  // Weight-stratified scenario sampling: keep ~1/scenario_stride of the splits
  // (each survivor reweighted so the aggregate is preserved in expectation).
  // 1 = full enumeration. Already bag-gated by the caller (1 for bag <= 2).
  // sampled_weight_seen is the per-candidate running weight tally; valid only
  // during a single enumeration (so it must not be shared across threads).
  int scenario_stride;
  int64_t sampled_weight_seen;
  int64_t deadline_ns;
  ThreadControl *thread_control;
  PegWorker *worker;
  // The opponent replies of this candidate's splits when one context
  // enumerates them all (NULL for a single scenario job), and the plies the
  // current split's greedy playouts share (see peg_eval_split).
  PegReplyCache *reply_cache;
  PegSharedPly *shared_plies[PEG_SHARED_PLAYOUT_PLIES];
  // Collect mode: when out_jobs is non-NULL, peg_eval_split pushes one scenario
  // job per split (for scenario-level parallelism) instead of evaluating it
  // inline. cand_idx tags the pushed jobs; workers is the shared scratch array.
  PegScenarioJobList *out_jobs;
  int cand_idx;
  PegWorker *workers;
  // Optional progress callbacks (NULL = none). cand_idx is the cand rank passed
  // to on_scenario_done; progress->stage_idx carries the current stage.
  const PegProgress *progress;
  // Optional per-scenario capture sink (NULL = off). Set only for the
  // single-threaded best-cand capture pass.
  PegScenarioCapture *capture;
  // accumulators
  double total_weight;
  double win_weight; // wins + 0.5 * draws, weighted
  double spread_weight;
  int64_t weight_sum;
  // Integer labeled-ordering tallies: each distinct bag ordering carries its
  // labeled weight (full_weight / n_orderings, an integer by symmetry), so
  // win_count + tie_count + losses == weight_sum and win% == (win_count +
  // tie_count / 2) / weight_sum exactly. Counted by orderings, not by multiset
  // weights, so the CLI can show whole-number wins/ties.
  int64_t win_count;
  int64_t tie_count;
  int n_scenarios;
  // Set when this candidate/scenario hits the wall-clock deadline or a user
  // interrupt. Its partial score must not enter a stage ranking.
  bool interrupted;
  // The candidate's early cutoff (NULL = none), copied into collected jobs.
  PegCutoff *cutoff;
} PegEvalCtx;

static bool peg_eval_should_stop(PegEvalCtx *ctx) {
  if (!peg_should_stop(ctx->deadline_ns, ctx->thread_control)) {
    return false;
  }
  ctx->interrupted = true;
  return true;
}

static void peg_scenario_joblist_push(PegScenarioJobList *list,
                                      const PegScenarioJob *job) {
  if (list->count == list->capacity) {
    list->capacity = list->capacity ? list->capacity * 2 : 256;
    list->jobs = realloc_or_die(list->jobs, (size_t)list->capacity *
                                                sizeof(PegScenarioJob));
  }
  list->jobs[list->count++] = *job;
}

static PegPruneCache *peg_prune_cache_create(void) {
  PegPruneCache *cache = malloc_or_die(sizeof(*cache));
  cpthread_mutex_init(&cache->mutex);
  cache->capacity = PEG_PRUNE_CACHE_CAPACITY;
  cache->count = 0;
  cache->keys = calloc_or_die((size_t)cache->capacity, sizeof(uint64_t));
  cache->values = calloc_or_die((size_t)cache->capacity, sizeof(KWG *));
  return cache;
}

static void peg_prune_cache_destroy(PegPruneCache *cache) {
  if (cache == NULL) {
    return;
  }
  for (int slot = 0; slot < cache->capacity; slot++) {
    kwg_destroy(cache->values[slot]);
  }
  free(cache->keys);
  free(cache->values);
  // No cpthread_mutex_destroy: project mutexes don't dynamically allocate.
  free(cache);
}

static uint64_t peg_board_signature(const Game *game) {
  const Board *board = game_get_board(game);
  uint64_t hash = FNV_64_OFFSET_BASIS; // FNV-1a over the board letters
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      hash = fnv64a_step(hash, (uint64_t)board_get_letter(board, row, col));
    }
  }
  return hash == 0 ? 1 : hash; // reserve 0 as the empty-slot sentinel
}

// Whether solves prune the lexicon to the words a position can play. Woadwage
// solves read the whole lexicon: pruning leaves out words of the lexicon that
// a woadwage word joins to another.
static bool peg_prunes_words(const Game *game) {
  return game_get_variant(game) != GAME_VARIANT_WOADWAGE;
}

// Prunes the lexicon to the words this position can still form. Cross-check
// aware pruning is only sound for classic play with one shared lexicon; see
// generate_possible_words_with_cross_checks.
static void peg_generate_possible_words(const Game *game, const KWG *kwg,
                                        DictionaryWordList *word_list) {
  if (game_get_variant(game) == GAME_VARIANT_CLASSIC &&
      game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG)) {
    generate_possible_words_with_cross_checks(game, kwg, word_list);
  } else {
    generate_possible_words(game, kwg, word_list);
  }
}

// Return the per-candidate pruned KWG for this leaf's board, building it once
// (chained off the parent prune on the game) and caching it by board signature.
// Builds run outside the lock; a rare insert race just discards the loser's
// build. If the table is somehow full, fall back to the (looser but correct)
// parent prune rather than caching.
static const KWG *peg_prune_cache_get(PegPruneCache *cache, const Game *game,
                                      int mover_idx) {
  const uint64_t key = peg_board_signature(game);
  const uint32_t mask = (uint32_t)cache->capacity - 1;
  cpthread_mutex_lock(&cache->mutex);
  uint32_t slot = (uint32_t)key & mask;
  while (cache->keys[slot] != 0) {
    if (cache->keys[slot] == key) {
      const KWG *hit = cache->values[slot];
      cpthread_mutex_unlock(&cache->mutex);
      return hit;
    }
    slot = (slot + 1) & mask;
  }
  cpthread_mutex_unlock(&cache->mutex);

  const KWG *parent_kwg = game_get_effective_kwg(game, mover_idx);
  DictionaryWordList *word_list = dictionary_word_list_create();
  peg_generate_possible_words(game, parent_kwg, word_list);
  KWG *built = make_kwg_from_words_small(word_list, KWG_MAKER_OUTPUT_GADDAG,
                                         KWG_MAKER_MERGE_EXACT);
  dictionary_word_list_destroy(word_list);

  cpthread_mutex_lock(&cache->mutex);
  slot = (uint32_t)key & mask;
  while (cache->keys[slot] != 0) {
    if (cache->keys[slot] == key) { // another worker built it first
      const KWG *other = cache->values[slot];
      cpthread_mutex_unlock(&cache->mutex);
      kwg_destroy(built);
      return other;
    }
    slot = (slot + 1) & mask;
  }
  if (cache->count >= cache->capacity - 1) {
    cpthread_mutex_unlock(&cache->mutex);
    kwg_destroy(built);
    return parent_kwg;
  }
  cache->keys[slot] = key;
  cache->values[slot] = built;
  cache->count++;
  cpthread_mutex_unlock(&cache->mutex);
  return built;
}

// ----- Nested pre-endgame lookahead (PegArgs.nested_enabled) ----------------
//
// A non-emptier leaf hands the opponent a genuine sub-pre-endgame. Instead of a
// single greedy/pessimistic rollout, peg_nested_value solves that sub-game by
// the same min/max-over-scenarios logic the outer PEG uses, recursing until the
// bag empties (exact endgame) or the lookahead budget / depth cap is spent
// (greedy floor). Single-threaded, inline on the worker's per-level scratch.
// Negamax in points: every node returns the ON-TURN player's signed final
// spread; the parent negates a child evaluated from the next player's view.

// The inner peg is a staged cascade (greedy seed -> halving stages at rising
// fidelity), run recursively on the opponent's sub-position. peg_inner_leaf
// evaluates one scenario's resolved game; peg_inner_cascade runs the staged
// search over the on-turn player's candidates and returns its best value.
static double peg_inner_cascade(PegWorker *worker, Game *game, int depth,
                                int outer_fidelity, int64_t deadline_ns);
static double peg_inner_leaf(PegWorker *worker, Game *game, int stage_fidelity,
                             int depth, int outer_fidelity,
                             int64_t deadline_ns);
static double peg_nested_best_reply(PegWorker *worker, Game *game,
                                    int stage_fidelity, int depth,
                                    int outer_fidelity, int64_t deadline_ns);

// A nested value as the solve reports it: the spread, or under first-win
// (PegWorker.first_win) its sign, so that averaging over scenarios ranks a
// nested player's plays by wins plus half the draws.
static double peg_nested_result(const PegWorker *worker, double value) {
  if (!worker->first_win) {
    return value;
  }
  return (double)((value > 0) - (value < 0));
}

// The nested recursion can be deep and serial, so it must observe the same stop
// signals the outer solve does — the wall-clock deadline and a user interrupt —
// at every node and between candidates, not just hand a deadline to the leaf
// endgame. Once stopped, nodes degrade to the cheap greedy floor immediately.
static bool peg_nested_should_stop(const PegWorker *worker,
                                   int64_t deadline_ns) {
  return peg_should_stop(deadline_ns, worker->thread_control);
}

// Greedy-rollout floor value (on-turn perspective) on a scratch copy, since the
// playout mutates the game it walks.
static int32_t peg_nested_floor(PegWorker *worker, const Game *game,
                                int on_turn, int64_t deadline_ns) {
  PegNestFrame *frame = peg_nest_acquire(worker);
  if (frame->game == NULL) {
    frame->game = game_duplicate(game);
  } else {
    game_copy(frame->game, game);
  }
  const int32_t value =
      peg_greedy_playout(frame->game, on_turn, worker->playout_ml, deadline_ns,
                         worker->thread_control, /*interrupted=*/NULL,
                         /*shared_plies=*/NULL);
  peg_nest_release(worker, frame);
  return value;
}

// Exact endgame value at an emptier nested leaf, from the on-turn perspective.
// nested_emptier_ply_cap sets the endgame depth: a positive value solves the
// emptier (bag-empty) leaf to exactly that many plies -- a fixed depth, so a
// deep forced outplay is searched in full -- while 0 ties the depth to the
// per-stage lookahead, which is cheaper but understates the spread of any
// outplay deeper than that lookahead.
static int32_t peg_nested_endgame_value(PegWorker *worker, Game *game,
                                        int on_turn, int lookahead,
                                        int64_t deadline_ns) {
  int plies = worker->nested_emptier_ply_cap > 0
                  ? worker->nested_emptier_ply_cap
                  : lookahead;
  if (plies < 1) {
    plies = 1;
  }
  if (plies >= 2 && peg_prunes_words(game)) {
    const KWG *leaf_kwg =
        peg_prune_cache_get(worker->prune_cache, game, on_turn);
    game_set_override_kwgs(game, leaf_kwg, NULL, DUAL_LEXICON_MODE_IGNORANT);
  }
  EndgameArgs ea;
  endgame_args_fill(
      worker->thread_control, game, /*tt_fraction_of_mem=*/0.0, plies,
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, /*num_threads=*/1,
      /*use_heuristics=*/true, /*num_top_moves=*/1,
      /*per_ply_callback=*/NULL, /*per_ply_callback_data=*/NULL,
      /*before_search_callback=*/NULL, /*before_search_callback_data=*/NULL,
      /*per_root_move_callback=*/NULL, /*per_root_move_callback_data=*/NULL,
      DUAL_LEXICON_MODE_IGNORANT, /*forced_pass_bypass=*/true,
      /*incremental_movegen=*/true, worker->disable_best_reply_shortcuts,
      /*enable_pv_display=*/false, /*soft_time_limit=*/0.0,
      /*hard_time_limit=*/0.0, PEG_ENDGAME_SEED, /*skip_word_pruning=*/true,
      peg_worker_get_endgame_tt(worker),
      // nested endgames are small and many; no core injection
      /*max_workers=*/0, worker->first_win, /*first_win_fallback_moves=*/0,
      /*use_initial_window=*/false, /*initial_alpha=*/0, /*initial_beta=*/0,
      deadline_ns, /*actual_move=*/NULL, &ea);
  endgame_results_reset(worker->eg_results);
  endgame_solve_inline(&worker->eg_ctx, &ea, worker->eg_results);
  if (endgame_results_get_depth(worker->eg_results, ENDGAME_RESULT_BEST) < 0) {
    return peg_greedy_playout(game, on_turn, worker->playout_ml, deadline_ns,
                              worker->thread_control,
                              /*interrupted=*/NULL, /*shared_plies=*/NULL);
  }
  const int32_t eg_val =
      endgame_results_get_value(worker->eg_results, ENDGAME_RESULT_BEST);
  const Player *me = game_get_player(game, on_turn);
  const Player *op = game_get_player(game, 1 - on_turn);
  const int32_t lead =
      equity_to_int(player_get_score(me) - player_get_score(op));
  return lead + eg_val; // on_turn is on move, so eg_val is its future delta
}

// One inner-peg scenario as a pool work item: a (mover_drawn, bag_remaining)
// split off a shared candidate template. The worker fn builds the sub-game on
// its own scratch frame, scores it, and writes mover_spread; the parent reduces
// the batch afterward (no atomics).
typedef struct PegNestScenarioJob {
  struct PegWorker *workers; // job indexes by the worker_idx it runs on
  const Game
      *template_src; // cand template (read-only, shared across scenarios)
  int mover_idx;
  const uint8_t *unseen;
  int ld_size;
  int k_drawn;
  int n_bag_remaining;
  MachineLetter mover_drawn[PEG_MAX_BAG + 1];
  MachineLetter bag_remaining[PEG_MAX_BAG + 1];
  int64_t weight;
  int stage_fidelity;
  int depth;
  int outer_fidelity;
  int64_t deadline_ns;
  double mover_spread; // result (mover-perspective leaf value)
} PegNestScenarioJob;

static void peg_nest_scenario_worker_fn(void *arg, int worker_idx) {
  PegNestScenarioJob *job = (PegNestScenarioJob *)arg;
  PegWorker *worker = &job->workers[worker_idx];
  PegNestFrame *frame = peg_nest_acquire(worker);
  Game *sub = peg_make_post_cand_game_into(
      &frame->game, job->template_src, job->mover_idx, job->unseen,
      job->ld_size, job->k_drawn, job->mover_drawn, job->n_bag_remaining,
      job->bag_remaining, /*known_opp=*/NULL);
  const double child =
      peg_inner_leaf(worker, sub, job->stage_fidelity, job->depth,
                     job->outer_fidelity, job->deadline_ns);
  const int child_turn = game_get_player_on_turn_index(sub);
  job->mover_spread = (child_turn == job->mover_idx) ? child : -child;
  peg_nest_release(worker, frame);
}

// Collects one inner candidate's scenarios as a growable job array, to be run
// across the pool (or inline) and reduced by peg_nested_cand_value.
typedef struct PegNestCtx {
  struct PegWorker *workers;
  const Game *template_src;
  int mover_idx;
  const uint8_t *unseen;
  int ld_size;
  int k_drawn;
  int stage_fidelity;
  int depth;
  int outer_fidelity;
  int64_t deadline_ns;
  int scenario_stride;
  int64_t sampled_weight_seen;
  PegNestScenarioJob *jobs;
  int n_jobs;
  int cap_jobs;
} PegNestCtx;

static void peg_nest_push_job(PegNestCtx *nc, const MachineLetter *mover_drawn,
                              int n_bag_rem, const MachineLetter *bag_remaining,
                              int64_t full_weight) {
  if (nc->n_jobs == nc->cap_jobs) {
    nc->cap_jobs = nc->cap_jobs ? nc->cap_jobs * 2 : 16;
    nc->jobs = realloc_or_die(nc->jobs, (size_t)nc->cap_jobs *
                                            sizeof(PegNestScenarioJob));
  }
  PegNestScenarioJob *job = &nc->jobs[nc->n_jobs++];
  job->workers = nc->workers;
  job->template_src = nc->template_src;
  job->mover_idx = nc->mover_idx;
  job->unseen = nc->unseen;
  job->ld_size = nc->ld_size;
  job->k_drawn = nc->k_drawn;
  job->n_bag_remaining = n_bag_rem;
  for (int drawn_idx = 0; drawn_idx < nc->k_drawn; drawn_idx++) {
    job->mover_drawn[drawn_idx] = mover_drawn[drawn_idx];
  }
  for (int bag_idx = 0; bag_idx < n_bag_rem; bag_idx++) {
    job->bag_remaining[bag_idx] = bag_remaining[bag_idx];
  }
  job->weight = full_weight;
  job->stage_fidelity = nc->stage_fidelity;
  job->depth = nc->depth;
  job->outer_fidelity = nc->outer_fidelity;
  job->deadline_ns = nc->deadline_ns;
  job->mover_spread = 0.0;
}

// Distribute the unseen pool into (mover draws k_drawn, bag keeps n_bag_rem,
// opp gets the rest); push one job per resulting scenario. Mirrors
// peg_enum_splits, but emits parallelizable work rather than evaluating inline.
static void peg_nest_enum_splits(PegNestCtx *nc, int ml, int mover_left,
                                 int bag_rem_left, int64_t weight,
                                 MachineLetter *mover_drawn, int n_mover,
                                 MachineLetter *bag_remaining, int n_bag_rem) {
  if (ml == nc->ld_size) {
    if (mover_left != 0 || bag_rem_left != 0) {
      return;
    }
    int64_t full_weight = weight;
    for (int factor = 2; factor <= nc->k_drawn; factor++) {
      full_weight *= factor;
    }
    if (nc->scenario_stride > 1) {
      const int64_t stride = nc->scenario_stride;
      const int64_t old_seen = nc->sampled_weight_seen;
      nc->sampled_weight_seen += full_weight;
      const int64_t samples =
          (old_seen + full_weight) / stride - old_seen / stride;
      if (samples == 0) {
        return;
      }
      full_weight = samples * stride;
    }
    peg_nest_push_job(nc, mover_drawn, n_bag_rem, bag_remaining, full_weight);
    return;
  }
  const int avail = nc->unseen[ml];
  const int max_mover = mover_left < avail ? mover_left : avail;
  for (int to_mover = 0; to_mover <= max_mover; to_mover++) {
    const int max_bag =
        bag_rem_left < (avail - to_mover) ? bag_rem_left : (avail - to_mover);
    for (int to_bag = 0; to_bag <= max_bag; to_bag++) {
      const int64_t add_weight = peg_binomial(avail, to_mover) *
                                 peg_binomial(avail - to_mover, to_bag);
      for (int copy_idx = 0; copy_idx < to_mover; copy_idx++) {
        mover_drawn[n_mover + copy_idx] = (MachineLetter)ml;
      }
      for (int copy_idx = 0; copy_idx < to_bag; copy_idx++) {
        bag_remaining[n_bag_rem + copy_idx] = (MachineLetter)ml;
      }
      peg_nest_enum_splits(nc, ml + 1, mover_left - to_mover,
                           bag_rem_left - to_bag, weight * add_weight,
                           mover_drawn, n_mover + to_mover, bag_remaining,
                           n_bag_rem + to_bag);
    }
  }
}

// Scenario sampling stride for an inner peg of the given bag size. An explicit
// nested_stride > 0 overrides; otherwise bag-dependent (2-peg full, 3-peg 1/5,
// 4-peg 1/7) so deeper inner pegs sample rather than enumerate every split.
static int peg_nested_stride_for_bag(const PegWorker *worker, int bag) {
  if (worker->nested_strides_by_bag != NULL && bag >= 0 &&
      bag < worker->nested_n_strides_by_bag &&
      worker->nested_strides_by_bag[bag] > 0) {
    return worker->nested_strides_by_bag[bag];
  }
  if (worker->nested_stride > 0) {
    return worker->nested_stride;
  }
  if (bag <= 2) {
    return 1;
  }
  if (bag == 3) {
    return 5;
  }
  return 7;
}

// Expected (over the candidate's scenarios) on-turn value of playing `cand`.
// Collects the scenarios as jobs, runs them across the pool (or inline when
// there is no pool / a single scenario), and reduces the weighted average:
// rounded to whole points for exact spreads, unrounded under first-win, where
// the scenario values are -1, 0 and +1.
static double peg_nested_cand_value(PegWorker *worker, const Game *parent_game,
                                    int mover_idx, const Move *cand,
                                    const uint8_t *unseen, int ld_size,
                                    int stage_fidelity, int depth,
                                    int outer_fidelity, int64_t deadline_ns) {
  const int bag = bag_get_letters(game_get_bag(parent_game));
  const int tiles_played = move_get_tiles_played(cand);
  const int k_drawn = tiles_played < bag ? tiles_played : bag;
  // PegNestScenarioJob's mover_drawn/bag_remaining (and the locals below) are
  // sized PEG_MAX_BAG + 1, not PEG_SCENARIO_ARRAY_CAP + 1: this nested-peg
  // path is only reachable from a non-relaxed (bag <= PEG_MAX_BAG) top-level
  // solve, since peg_solve's bag-emptying-guarantee exception (relaxed bag up
  // to RACK_SIZE) makes every candidate empty the bag outright, leaving no
  // inner peg to nest into. Guard the invariant here rather than relying on
  // it silently holding.
  assert(k_drawn <= PEG_MAX_BAG);
  const int bag_rem = bag - k_drawn;
  // The template (cand played + cross-sets) is read concurrently by the
  // scenario jobs, so it is held on its own frame for the lifetime of the
  // submission.
  PegNestFrame *tframe = peg_nest_acquire(worker);
  peg_build_template_into(&tframe->game, parent_game, cand);
  PegNestCtx nc;
  memset(&nc, 0, sizeof(nc));
  nc.workers = worker->nest_workers;
  nc.template_src = tframe->game;
  nc.mover_idx = mover_idx;
  nc.unseen = unseen;
  nc.ld_size = ld_size;
  nc.k_drawn = k_drawn;
  nc.stage_fidelity = stage_fidelity;
  nc.depth = depth;
  nc.outer_fidelity = outer_fidelity;
  nc.deadline_ns = deadline_ns;
  nc.scenario_stride = peg_nested_stride_for_bag(worker, bag);
  MachineLetter mover_drawn[PEG_MAX_BAG + 1];
  MachineLetter bag_remaining[PEG_MAX_BAG + 1];
  peg_nest_enum_splits(&nc, 0, k_drawn, bag_rem, 1, mover_drawn, 0,
                       bag_remaining, 0);
  if (worker->nest_pool != NULL && nc.n_jobs > 1) {
    void **ptrs = malloc_or_die((size_t)nc.n_jobs * sizeof(void *));
    for (int job_idx = 0; job_idx < nc.n_jobs; job_idx++) {
      ptrs[job_idx] = &nc.jobs[job_idx];
    }
    // Re-entrant submit so a blocked worker only help-drains other re-entrant
    // (inner) items. Every nesting level fans out -- the inner unseen is
    // diverse even when the outer pool is uniform (the mover's leave joins it),
    // so deep nesting is where most of the cores come from on few-candidate
    // positions; the recursion depth bounds the submit nesting.
    peg_pool_submit_and_wait_reentrant(worker->nest_pool,
                                       peg_nest_scenario_worker_fn, ptrs,
                                       nc.n_jobs, worker->nest_worker_idx);
    free(ptrs);
  } else {
    for (int job_idx = 0; job_idx < nc.n_jobs; job_idx++) {
      peg_nest_scenario_worker_fn(&nc.jobs[job_idx], worker->nest_worker_idx);
    }
  }
  int64_t weight_sum = 0;
  double value_weight = 0.0;
  for (int job_idx = 0; job_idx < nc.n_jobs; job_idx++) {
    weight_sum += nc.jobs[job_idx].weight;
    value_weight +=
        (double)nc.jobs[job_idx].weight * nc.jobs[job_idx].mover_spread;
  }
  free(nc.jobs);
  peg_nest_release(worker, tframe);
  if (weight_sum == 0) {
    return peg_nested_result(
        worker, peg_nested_floor(worker, parent_game, mover_idx, deadline_ns));
  }
  const double avg = value_weight / (double)weight_sum;
  if (worker->first_win) {
    return avg;
  }
  return (double)(int32_t)(avg >= 0 ? avg + 0.5 : avg - 0.5);
}

// Score one scenario's fully-resolved game (on-turn perspective): terminal
// score; stop/seed -> greedy floor; emptier -> exact endgame at stage_fidelity;
// non-emptier -> a deeper inner peg if recursion depth remains, else greedy.
static double peg_inner_leaf(PegWorker *worker, Game *game, int stage_fidelity,
                             int depth, int outer_fidelity,
                             int64_t deadline_ns) {
  const int on_turn = game_get_player_on_turn_index(game);
  if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
    const Player *me = game_get_player(game, on_turn);
    const Player *op = game_get_player(game, 1 - on_turn);
    return peg_nested_result(
        worker, equity_to_int(player_get_score(me) - player_get_score(op)));
  }
  if (peg_nested_should_stop(worker, deadline_ns) || stage_fidelity <= 0) {
    return peg_nested_result(
        worker, peg_nested_floor(worker, game, on_turn, deadline_ns));
  }
  if (bag_get_letters(game_get_bag(game)) == 0) {
    return peg_nested_result(
        worker, peg_nested_endgame_value(worker, game, on_turn, stage_fidelity,
                                         deadline_ns));
  }
  if (worker->nested_pessimistic && on_turn != worker->root_mover_idx) {
    return peg_nested_best_reply(worker, game, stage_fidelity, depth,
                                 outer_fidelity, deadline_ns);
  }
  if (depth <= 1) {
    return peg_nested_result(
        worker, peg_nested_floor(worker, game, on_turn, deadline_ns));
  }
  return peg_inner_cascade(worker, game, depth - 1, outer_fidelity,
                           deadline_ns);
}

// The on-turn player's value of `cand` on this scenario's actual racks and
// bag: the play is made, its draw taken from the bag in order, and the
// position after it valued as any nested leaf.
static double peg_nested_play_actual(PegWorker *worker, const Game *game,
                                     const Move *cand, int stage_fidelity,
                                     int depth, int outer_fidelity,
                                     int64_t deadline_ns) {
  const int chooser = game_get_player_on_turn_index(game);
  PegNestFrame *frame = peg_nest_acquire(worker);
  if (frame->game == NULL) {
    frame->game = game_duplicate(game);
  } else {
    game_copy(frame->game, game);
  }
  play_move(cand, frame->game, NULL);
  const double value = peg_inner_leaf(worker, frame->game, stage_fidelity,
                                      depth, outer_fidelity, deadline_ns);
  const int next = game_get_player_on_turn_index(frame->game);
  peg_nest_release(worker, frame);
  return next == chooser ? value : -value;
}

// The pessimistic opponent (PegWorker.nested_pessimistic): with the scenario's
// racks and bag order known, it tries every reply, or its inner_top_k
// highest-equity ones, and takes the best for itself, which is the worst for
// the mover. Returns that value from its own (on-turn) view. Replies are tried
// best equity first, and under first-win the search stops at the first reply
// that wins for it.
static double peg_nested_best_reply(PegWorker *worker, Game *game,
                                    int stage_fidelity, int depth,
                                    int outer_fidelity, int64_t deadline_ns) {
  const int on_turn = game_get_player_on_turn_index(game);
  PegNestFrame *list_frame = peg_nest_acquire(worker);
  if (list_frame->moves == NULL) {
    list_frame->moves = move_list_create(PEG_NEST_CAND_LIST_CAP);
  }
  MoveList *replies = list_frame->moves;
  const MoveGenArgs movegen_args = {
      .game = game,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .move_list = replies,
      .tiles_played_bv = NULL,
      .initial_tiles_bv = 0,
  };
  generate_moves(&movegen_args);
  const int num_replies = move_list_get_count(replies);
  if (num_replies == 0) {
    peg_nest_release(worker, list_frame);
    return peg_nested_result(
        worker, peg_nested_floor(worker, game, on_turn, deadline_ns));
  }
  move_list_sort_moves(replies);
  int num_considered = num_replies;
  if (worker->inner_top_k > 0 && worker->inner_top_k < num_replies) {
    num_considered = worker->inner_top_k;
  }
  bool any = false;
  double best = 0.0;
  for (int reply_idx = 0; reply_idx < num_considered; reply_idx++) {
    if (reply_idx > 0 && peg_nested_should_stop(worker, deadline_ns)) {
      break;
    }
    const double value = peg_nested_play_actual(
        worker, game, move_list_get_move(replies, reply_idx), stage_fidelity,
        depth, outer_fidelity, deadline_ns);
    if (!any || value > best) {
      best = value;
      any = true;
    }
    if (worker->first_win && best > 0) {
      break;
    }
  }
  peg_nest_release(worker, list_frame);
  return best;
}

// Staged cascade over the on-turn player's candidates: seed the field by static
// equity to the schedule's first cap, then run one stage per schedule entry at
// rising endgame fidelity (2-ply, 3-ply, ...), narrowing to each entry's keep
// count. Returns the best candidate's value (on-turn perspective).
static double peg_inner_cascade(PegWorker *worker, Game *game, int depth,
                                int outer_fidelity, int64_t deadline_ns) {
  const int on_turn = game_get_player_on_turn_index(game);
  // The candidate list is read across the stage loop (which submits + waits per
  // candidate), so it is held on its own frame for the cascade's lifetime.
  PegNestFrame *cframe = peg_nest_acquire(worker);
  if (cframe->moves == NULL) {
    cframe->moves = move_list_create(PEG_NEST_CAND_LIST_CAP);
  }
  MoveList *cand_moves = cframe->moves;
  const MoveGenArgs movegen_args = {
      .game = game,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .move_list = cand_moves,
      .tiles_played_bv = NULL,
      .initial_tiles_bv = 0,
  };
  generate_moves(&movegen_args);
  const int num_cands = move_list_get_count(cand_moves);
  if (num_cands == 0) {
    peg_nest_release(worker, cframe);
    return peg_nested_result(
        worker, peg_nested_floor(worker, game, on_turn, deadline_ns));
  }
  move_list_sort_moves(cand_moves);
  uint8_t unseen[MAX_ALPHABET_SIZE];
  const int ld_size = ld_get_size(game_get_ld(game));
  peg_compute_unseen(game, on_turn, unseen);
  // Stage schedule: nested_cand_caps (e.g. {8,4,2} arm, {8,8,8} oracle). Entry
  // 0 is the initial field size; each later entry is the keep-count after that
  // stage. No schedule -> the flat cap as a single stage.
  const int *sched = worker->nested_cand_caps;
  const int n_sched = worker->nested_n_cand_caps;
  int field_cap = num_cands;
  if (n_sched > 0) {
    field_cap = sched[0];
  } else if (worker->nested_cand_cap > 0) {
    field_cap = worker->nested_cand_cap;
  }
  if (field_cap > num_cands) {
    field_cap = num_cands;
  }
  if (field_cap > PEG_NEST_FIELD_MAX) {
    field_cap = PEG_NEST_FIELD_MAX;
  }
  int idx[PEG_NEST_FIELD_MAX];
  double val[PEG_NEST_FIELD_MAX];
  // memset first so the analyzer treats the whole array as initialized: the
  // read at idx[cand_idx] (cand_idx < field_n) is provably in-bounds since
  // field_n is seeded to field_cap and only ever shrinks, but the analyzer
  // can't follow that across the separate init/read bounds. Behaviorally
  // identical (entries >= field_cap are never read); clears a clang-analyzer
  // uninit-read false positive.
  memset(idx, 0, sizeof(idx));
  for (int cand_idx = 0; cand_idx < field_cap; cand_idx++) {
    idx[cand_idx] = cand_idx;
  }
  int field_n = field_cap;
  // Scale inner depth to the outer stage: inner stage stage_idx runs at endgame
  // fidelity 2 + stage_idx, so cap the stage count at outer_fidelity - 1. Early
  // outer stages (2-ply) thus run a single cheap inner stage; the inner peg
  // only deepens as the outer cascade does, keeping early outer stages
  // affordable.
  int n_stages = n_sched > 0 ? n_sched : 1;
  const int fidelity_stage_cap = outer_fidelity - 1;
  if (fidelity_stage_cap >= 1 && fidelity_stage_cap < n_stages) {
    n_stages = fidelity_stage_cap;
  }
  if (n_stages < 1) {
    n_stages = 1;
  }
  for (int stage_idx = 0; stage_idx < n_stages; stage_idx++) {
    if (stage_idx > 0 && peg_nested_should_stop(worker, deadline_ns)) {
      break;
    }
    const int stage_fidelity = 2 + stage_idx; // rising endgame fidelity / stage
    for (int cand_idx = 0; cand_idx < field_n; cand_idx++) {
      if (cand_idx > 0 && peg_nested_should_stop(worker, deadline_ns)) {
        field_n = cand_idx;
        break;
      }
      const Move *cand = move_list_get_move(cand_moves, idx[cand_idx]);
      val[cand_idx] = peg_nested_cand_value(worker, game, on_turn, cand, unseen,
                                            ld_size, stage_fidelity, depth,
                                            outer_fidelity, deadline_ns);
    }
    // Selection sort the small field by value, descending.
    for (int sel_idx = 0; sel_idx < field_n; sel_idx++) {
      int best_idx = sel_idx;
      for (int scan_idx = sel_idx + 1; scan_idx < field_n; scan_idx++) {
        if (val[scan_idx] > val[best_idx]) {
          best_idx = scan_idx;
        }
      }
      if (best_idx != sel_idx) {
        const int tmp_idx = idx[sel_idx];
        idx[sel_idx] = idx[best_idx];
        idx[best_idx] = tmp_idx;
        const double tmp_val = val[sel_idx];
        val[sel_idx] = val[best_idx];
        val[best_idx] = tmp_val;
      }
    }
    if (stage_idx + 1 < n_stages) {
      const int keep = sched[stage_idx + 1];
      if (keep > 0 && keep < field_n) {
        field_n = keep;
      }
    }
  }
  double result = 0.0;
  if (field_n == 0) {
    result = peg_nested_result(
        worker, peg_nested_floor(worker, game, on_turn, deadline_ns));
  } else if (worker->nested_score_actual) {
    // The choice was made from what this player can see; its value is what it
    // does on the scenario at hand.
    result = peg_nested_play_actual(
        worker, game, move_list_get_move(cand_moves, idx[0]), 1 + n_stages,
        depth, outer_fidelity, deadline_ns);
  } else {
    result = val[0];
  }
  peg_nest_release(worker, cframe);
  return result;
}

// Evaluate the leaf of one fully-resolved scenario (a specific post-cand game).
// Returns mover's signed spread (points) — exact via endgame_solve for emptier
// scenarios at fidelity > 0, else the greedy playout.
static int32_t peg_eval_leaf(PegEvalCtx *ctx, Game *game) {
  if (peg_eval_should_stop(ctx)) {
    return 0;
  }
  const bool emptier = bag_get_letters(game_get_bag(game)) == 0 &&
                       game_get_game_end_reason(game) == GAME_END_REASON_NONE;
  if (ctx->fidelity_plies <= 0 || !emptier) {
    // Non-emptier (or stage-0) leaf: the opponent still draws, so the model
    // matters. Emptier leaves fall through to the exact endgame below, which is
    // already adversarial-optimal and thus identical for both models.
    //
    // Nested lookahead: at fidelity > 0, solve the opponent's response as a
    // staged inner peg instead of a flat rollout. nested_max_depth is the
    // inner-peg recursion depth (how many nested pegs before greedy rollout):
    // 0 or unset = 1 (one minimal peg, greedy below); a value >= the bag size
    // nests until the bag empties (the recursion bottoms out at the exact
    // endgame, so it is bounded by the bag regardless of the cap).
    if (!emptier && ctx->fidelity_plies > 0 && ctx->worker->nested_enabled) {
      const int depth =
          ctx->worker->nested_max_depth > 0 ? ctx->worker->nested_max_depth : 1;
      const int on_turn = game_get_player_on_turn_index(game);
      double on_turn_val =
          ctx->worker->nested_pessimistic && on_turn != ctx->mover_idx
              ? peg_nested_best_reply(ctx->worker, game, ctx->fidelity_plies,
                                      depth, ctx->fidelity_plies,
                                      ctx->deadline_ns)
              : peg_inner_cascade(ctx->worker, game, depth, ctx->fidelity_plies,
                                  ctx->deadline_ns);
      if (ctx->worker->first_win) {
        // A first-win nested value is an average of signs: report its sign.
        on_turn_val = peg_nested_result(ctx->worker, on_turn_val);
      }
      const int32_t value = (int32_t)on_turn_val;
      return (on_turn == ctx->mover_idx) ? value : -value;
    }
    if (ctx->opp_model == PEG_OPP_PESSIMISTIC) {
      return peg_pessimistic_playout(
          game, ctx->mover_idx, ctx->worker->playout_ml, ctx->inner_top_k,
          ctx->deadline_ns, ctx->thread_control, &ctx->interrupted);
    }
    return peg_greedy_playout(game, ctx->mover_idx, ctx->worker->playout_ml,
                              ctx->deadline_ns, ctx->thread_control,
                              &ctx->interrupted, ctx->shared_plies);
  }
  // Exact endgame leaf. After the mover plays and draws it is the opponent's
  // turn, so the solved value is from the on-turn player's perspective; fold
  // it into the mover lead accordingly.
  //
  // Chained re-prune: install this candidate's tighter pruned KWG (built once
  // per board, chained off the root prune already on the game) for the endgame
  // to use. The existing (parent) cross-sets are kept as-is and NOT
  // regenerated: narrowing the word list only removes words, and the parent
  // cross-sets already agree with the leaf's on every rack-playable letter (any
  // perpendicular word a rack tile forms is itself playable, hence in the leaf
  // KWG too), so the looser parent cross-sets gate the exact same legal moves.
  //
  // Only worth it for >=2-ply searches: a tighter KWG speeds up move
  // generation, so the gain scales with how many move-gens the endgame runs. At
  // 1 ply there are too few to amortize the build. (The default cascade only
  // ever uses 2..6-ply exact leaves, so this is a robustness guard for custom
  // configs.)
  if (ctx->fidelity_plies >= 2 && peg_prunes_words(game)) {
    const KWG *leaf_kwg =
        peg_prune_cache_get(ctx->worker->prune_cache, game, ctx->mover_idx);
    game_set_override_kwgs(game, leaf_kwg, NULL, DUAL_LEXICON_MODE_IGNORANT);
  }
  if (peg_eval_should_stop(ctx)) {
    return 0;
  }
  EndgameArgs ea;
  endgame_args_fill(
      ctx->thread_control, game, /*tt_fraction_of_mem=*/0.0,
      ctx->fidelity_plies, DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      /*num_threads=*/1, /*use_heuristics=*/true, /*num_top_moves=*/1,
      /*per_ply_callback=*/NULL, /*per_ply_callback_data=*/NULL,
      /*before_search_callback=*/NULL, /*before_search_callback_data=*/NULL,
      /*per_root_move_callback=*/NULL, /*per_root_move_callback_data=*/NULL,
      DUAL_LEXICON_MODE_IGNORANT, /*forced_pass_bypass=*/true,
      /*incremental_movegen=*/true, ctx->worker->disable_best_reply_shortcuts,
      /*enable_pv_display=*/ctx->capture != NULL, /*soft_time_limit=*/0.0,
      /*hard_time_limit=*/0.0, PEG_ENDGAME_SEED, /*skip_word_pruning=*/true,
      peg_worker_get_endgame_tt(ctx->worker),
      // > num_threads (1) opens the injection window so the monitor can lend
      // idle cores to this (potentially long) endgame mid-solve.
      /*max_workers=*/ctx->injection_cap, ctx->worker->first_win,
      /*first_win_fallback_moves=*/0, /*use_initial_window=*/false,
      /*initial_alpha=*/0, /*initial_beta=*/0, ctx->deadline_ns,
      /*actual_move=*/NULL, &ea);
  endgame_results_reset(ctx->worker->eg_results);
  endgame_solve_inline(&ctx->worker->eg_ctx, &ea, ctx->worker->eg_results);
  // If the solver was interrupted before completing any search depth (depth
  // remains -1 after reset), eg_results still holds a stale value from a prior
  // solve. Fall back to greedy rather than misreporting the scenario outcome.
  if (endgame_results_get_depth(ctx->worker->eg_results, ENDGAME_RESULT_BEST) <
      0) {
    return peg_greedy_playout(game, ctx->mover_idx, ctx->worker->playout_ml,
                              ctx->deadline_ns, ctx->thread_control,
                              &ctx->interrupted, /*shared_plies=*/NULL);
  }
  if (ctx->capture != NULL) {
    const PVLine *pv =
        endgame_results_get_num_pvs(ctx->worker->eg_results) > 0
            ? endgame_results_get_multi_pvline(ctx->worker->eg_results, 0)
            : endgame_results_get_pvline(ctx->worker->eg_results,
                                         ENDGAME_RESULT_BEST);
    Game *pv_game = game_duplicate(game);
    StringBuilder *line = string_builder_create();
    for (int move_idx = 0; move_idx < pv->num_moves; move_idx++) {
      Move move;
      small_move_to_move(&move, &pv->moves[move_idx], game_get_board(pv_game));
      if (move_idx > 0) {
        string_builder_add_string(line, " → ");
      }
      string_builder_add_move(line, game_get_board(pv_game), &move,
                              game_get_ld(pv_game), false);
      play_move(&move, pv_game, NULL);
    }
    ctx->capture->pending_pv = string_builder_dump_and_destroy(line, NULL);
    game_destroy(pv_game);
  }
  const int eg_val =
      endgame_results_get_value(ctx->worker->eg_results, ENDGAME_RESULT_BEST);
  const Player *me = game_get_player(game, ctx->mover_idx);
  const Player *op = game_get_player(game, 1 - ctx->mover_idx);
  const int32_t mover_lead =
      equity_to_int(player_get_score(me) - player_get_score(op));
  const int turn = game_get_player_on_turn_index(game);
  return (turn == ctx->mover_idx) ? mover_lead + eg_val : mover_lead - eg_val;
}

// Append the human-readable letters of tiles[0..n) to out (for per-scenario
// detail rows). PEG distributions use single-byte letters, so a few tiles fit
// the small fixed PegPerScenario buffers.
static void peg_tiles_to_string(const LetterDistribution *ld,
                                const MachineLetter *tiles, int n, char *out,
                                size_t out_size) {
  size_t len = 0;
  for (int i = 0; i < n; i++) {
    const char *hl = ld->ld_ml_to_hl[tiles[i]];
    const size_t hl_len = strlen(hl);
    if (len + hl_len + 1 > out_size) {
      break;
    }
    memcpy(out + len, hl, hl_len);
    len += hl_len;
  }
  out[len] = '\0';
}

// Record one captured scenario row (best-cand capture pass only).
static void peg_capture_row(PegScenarioCapture *capture,
                            const MachineLetter *mover_drawn, int k_drawn,
                            const MachineLetter *bag_order, int n_bag_remaining,
                            int64_t weight, int32_t mover_total) {
  if (capture->count == capture->capacity) {
    capture->capacity = capture->capacity ? capture->capacity * 2 : 64;
    capture->rows = realloc_or_die(capture->rows, (size_t)capture->capacity *
                                                      sizeof(PegPerScenario));
  }
  PegPerScenario *row = &capture->rows[capture->count];
  row->pv = capture->pending_pv;
  capture->pending_pv = NULL;
  row->scenario_idx = capture->count;
  peg_tiles_to_string(capture->ld, mover_drawn, k_drawn, row->drawn,
                      sizeof(row->drawn));
  peg_tiles_to_string(capture->ld, bag_order, n_bag_remaining, row->remaining,
                      sizeof(row->remaining));
  row->weight = weight;
  row->mover_total = mover_total;
  capture->count++;
}

// Where the greedy playouts of `game`'s split share the opponent's first
// reply: the candidate's cache slot for the opponent's rack, or `split_slot`
// when there is no cache or it is full.
static PegSharedPly *peg_split_opp_reply(const PegEvalCtx *ctx,
                                         const Game *game,
                                         PegSharedPly *split_slot) {
  if (ctx->reply_cache == NULL) {
    return split_slot;
  }
  const Rack *opp_rack =
      player_get_rack(game_get_player(game, 1 - ctx->mover_idx));
  MachineLetter letters[RACK_SIZE] = {0};
  int n_letters = 0;
  for (int ml = 0; ml < ctx->ld_size; ml++) {
    for (int count = rack_get_letter(opp_rack, (MachineLetter)ml); count > 0;
         count--) {
      assert(n_letters < RACK_SIZE);
      letters[n_letters++] = (MachineLetter)ml;
    }
  }
  PegSharedPly *slot =
      peg_reply_cache_slot(ctx->reply_cache, letters, n_letters);
  return slot != NULL ? slot : split_slot;
}

// Evaluate one (mover_drawn, bag_remaining) split: walk the distinct orderings
// of bag_remaining (each equally likely), evaluate each leaf, and fold the
// multiset weight into the accumulator.
static void peg_eval_split(PegEvalCtx *ctx, const MachineLetter *mover_drawn,
                           int n_bag_remaining,
                           const MachineLetter *bag_remaining, int64_t weight) {
  if (peg_eval_should_stop(ctx)) {
    return;
  }
  // Collect mode: emit this split as its own job for scenario-level parallelism
  // instead of evaluating it inline.
  if (ctx->out_jobs != NULL) {
    PegScenarioJob job;
    memset(&job, 0, sizeof(job));
    job.template_src = ctx->template_src;
    job.mover_idx = ctx->mover_idx;
    job.unseen = ctx->unseen;
    job.ld_size = ctx->ld_size;
    job.k_drawn = ctx->k_drawn;
    job.n_bag_remaining = n_bag_remaining;
    for (int i = 0; i < ctx->k_drawn; i++) {
      job.mover_drawn[i] = mover_drawn[i];
    }
    for (int i = 0; i < n_bag_remaining; i++) {
      job.bag_remaining[i] = bag_remaining[i];
    }
    job.weight = weight;
    job.opp_model = ctx->opp_model;
    job.inner_top_k = ctx->inner_top_k;
    job.fidelity_plies = ctx->fidelity_plies;
    job.injection_cap = ctx->injection_cap;
    job.deadline_ns = ctx->deadline_ns;
    job.thread_control = ctx->thread_control;
    job.workers = ctx->workers;
    job.cand_idx = ctx->cand_idx;
    job.progress = ctx->progress;
    job.cutoff = ctx->cutoff;
    peg_scenario_joblist_push(ctx->out_jobs, &job);
    return;
  }
  // Permute a LOCAL copy: peg_next_perm reorders the whole array in place, and
  // the caller's bag_remaining buffer is shared across the peg_enum_splits
  // recursion (ancestor letter-branches own its earlier positions). Mutating it
  // here would scramble those positions for subsequent sibling branches and
  // corrupt their multisets. Copy first so the caller's buffer is untouched.
  MachineLetter perm[PEG_SCENARIO_ARRAY_CAP + 1];
  for (int i = 0; i < n_bag_remaining; i++) {
    perm[i] = bag_remaining[i];
  }
  MachineLetter *bag_perm = perm;
  // Sort bag_perm ascending so next_perm enumerates distinct orderings.
  for (int i = 1; i < n_bag_remaining; i++) {
    MachineLetter key = bag_perm[i];
    int j = i - 1;
    while (j >= 0 && bag_perm[j] > key) {
      bag_perm[j + 1] = bag_perm[j];
      j--;
    }
    bag_perm[j + 1] = key;
  }
  // Each distinct ordering stands for weight * prod(multiplicity!) labeled
  // ordered draws (see the tallies below); the cutoff needs it as each
  // ordering is solved.
  int64_t per_ordering_up_front = weight;
  for (int run_start = 0; run_start < n_bag_remaining;) {
    int run_end = run_start + 1;
    while (run_end < n_bag_remaining &&
           bag_perm[run_end] == bag_perm[run_start]) {
      run_end++;
    }
    for (int factor = 2; factor <= run_end - run_start; factor++) {
      per_ordering_up_front *= factor;
    }
    run_start = run_end;
  }
  double ordering_win = 0.0;
  double ordering_spread = 0.0;
  int n_orderings = 0;
  int ordering_wins = 0;
  int ordering_ties = 0;
  // Captured rows for this split start here; their weight is backfilled to the
  // per-ordering labeled count once n_orderings is known (below).
  const int capture_start = ctx->capture != NULL ? ctx->capture->count : 0;
  // Every ordering of this split leaves the opponent the same rack and the
  // mover the same draw, and the bag's order first matters when the opponent
  // draws after its reply, so their greedy playouts open with the same two
  // moves: the opponent's reply (shared across the candidate's splits too,
  // through ctx->reply_cache) and the mover's answer to it.
  PegSharedPly split_opp_reply = {0};
  PegSharedPly mover_reply = {0};
  ctx->shared_plies[1] = &mover_reply;
  // Set when the cutoff stops the split before its last ordering. A split
  // whose last ordering cuts the candidate has solved them all, so it counts.
  bool cut_short = false;
  do {
    if (peg_eval_should_stop(ctx)) {
      break;
    }
    if (peg_cutoff_is_cut(ctx->cutoff)) {
      cut_short = true;
      break;
    }
    Game *game = peg_make_post_cand_game(
        ctx->worker, ctx->template_src, ctx->mover_idx, ctx->unseen,
        ctx->ld_size, ctx->k_drawn, mover_drawn, n_bag_remaining, bag_perm);
    if (n_orderings == 0) {
      ctx->shared_plies[0] = peg_split_opp_reply(ctx, game, &split_opp_reply);
    }
    const int32_t value = peg_eval_leaf(ctx, game);
    if (!ctx->interrupted) {
      peg_cutoff_add(ctx->cutoff, per_ordering_up_front, value);
    }
    if (ctx->progress != NULL && ctx->progress->on_scenario_done != NULL) {
      ctx->progress->on_scenario_done(ctx->progress->stage_idx, ctx->cand_idx,
                                      ctx->n_scenarios + n_orderings, value,
                                      weight, ctx->progress->user_data);
    }
    if (ctx->capture != NULL) {
      peg_capture_row(ctx->capture, mover_drawn, ctx->k_drawn, bag_perm,
                      n_bag_remaining, weight, value);
    }
    if (value > 0) {
      ordering_win += 1.0;
      ordering_wins++;
    } else if (value == 0) {
      ordering_win += 0.5;
      ordering_ties++;
    }
    ordering_spread += (double)value;
    n_orderings++;
  } while (peg_next_perm(bag_perm, n_bag_remaining));
  ctx->shared_plies[0] = NULL;
  ctx->shared_plies[1] = NULL;

  if (ctx->interrupted || n_orderings == 0 || cut_short) {
    return;
  }

  // Each ordering is equally likely within this multiset, so the multiset's
  // weight is split evenly across its orderings.
  ctx->total_weight += (double)weight;
  ctx->win_weight += (double)weight * (ordering_win / n_orderings);
  ctx->spread_weight += (double)weight * (ordering_spread / n_orderings);
  ctx->n_scenarios += n_orderings;
  // Integer labeled-ordering tallies. `weight` (full_weight) counts the labeled
  // deals with the bag held as an unordered multiset; the fully ordered draw
  // space has perm(unseen, bag_size) = total_weight * n_bag! members. So each
  // of the n_orderings distinct bag orderings stands for weight * n_bag! /
  // n_orderings labeled ordered draws (= weight * prod(letter_mult!), always an
  // integer since n_orderings divides n_bag!). Tallying wins/ties in those
  // units keeps them whole, makes win/tie/loss sum to the constant perm(unseen,
  // bag_size), and reconstructs win% exactly.
  int64_t n_bag_factorial = 1;
  for (int factor = 2; factor <= n_bag_remaining; factor++) {
    n_bag_factorial *= factor;
  }
  const int64_t per_ordering = weight * n_bag_factorial / n_orderings;
  ctx->win_count += per_ordering * ordering_wins;
  ctx->tie_count += per_ordering * ordering_ties;
  ctx->weight_sum += weight * n_bag_factorial;
  // Backfill each captured ordering's weight with its labeled-ordering count so
  // a renderer's per-draw multipliers sum to the integer win/tie/loss columns.
  if (ctx->capture != NULL) {
    for (int row_idx = capture_start; row_idx < ctx->capture->count;
         row_idx++) {
      ctx->capture->rows[row_idx].weight = per_ordering;
    }
  }
}

// Recursively choose, per machine letter, how many tiles go to the mover's
// draw (m) and to the bag remainder (b), with m+b <= unseen[ml], mover total ==
// k_drawn and bag-remainder total == n_bag_remaining. Opp gets the complement.
static void peg_enum_splits(PegEvalCtx *ctx, int ml, int mover_left,
                            int bag_rem_left, int64_t weight,
                            MachineLetter *mover_drawn, int n_mover,
                            MachineLetter *bag_remaining, int n_bag_rem) {
  if (peg_eval_should_stop(ctx)) {
    return;
  }
  if (ml == ctx->ld_size) {
    if (mover_left == 0 && bag_rem_left == 0) {
      // k_drawn! accounts for the order in which the mover draws its tiles.
      int64_t full_weight = weight;
      for (int factor = 2; factor <= ctx->k_drawn; factor++) {
        full_weight *= factor;
      }
      // Weight-stratified sampling: this split covers the weight band
      // [seen, seen + full_weight); keep it only if a stride boundary falls
      // inside, and reweight by (boundaries x stride) so the expected aggregate
      // weight is preserved. Applied once per enumeration (collect or stage 0);
      // the scenario worker re-evaluates the already-sampled weight directly.
      if (ctx->scenario_stride > 1) {
        const int64_t stride = ctx->scenario_stride;
        const int64_t old_seen = ctx->sampled_weight_seen;
        ctx->sampled_weight_seen += full_weight;
        const int64_t samples =
            (old_seen + full_weight) / stride - old_seen / stride;
        if (samples == 0) {
          return; // no stride boundary in this split's weight band — skip it
        }
        full_weight = samples * stride;
      }
      peg_eval_split(ctx, mover_drawn, n_bag_rem, bag_remaining, full_weight);
    }
    return;
  }
  const int avail = ctx->unseen[ml];
  const int max_mover = mover_left < avail ? mover_left : avail;
  for (int to_mover = 0; to_mover <= max_mover; to_mover++) {
    const int max_bag =
        bag_rem_left < (avail - to_mover) ? bag_rem_left : (avail - to_mover);
    for (int to_bag = 0; to_bag <= max_bag; to_bag++) {
      const int64_t add_weight = peg_binomial(avail, to_mover) *
                                 peg_binomial(avail - to_mover, to_bag);
      for (int i = 0; i < to_mover; i++) {
        mover_drawn[n_mover + i] = (MachineLetter)ml;
      }
      for (int i = 0; i < to_bag; i++) {
        bag_remaining[n_bag_rem + i] = (MachineLetter)ml;
      }
      peg_enum_splits(ctx, ml + 1, mover_left - to_mover, bag_rem_left - to_bag,
                      weight * add_weight, mover_drawn, n_mover + to_mover,
                      bag_remaining, n_bag_rem + to_bag);
    }
  }
}

// One candidate-evaluation job (cand at a given fidelity), dispatched to a
// pool worker or run inline.
typedef struct PegCandJob {
  const Game *base_game;
  int mover_idx;
  const uint8_t *unseen;
  int ld_size;
  const Move *cand;
  int bag_size;
  PegOppModel opp_model;
  int inner_top_k;
  int fidelity_plies;
  int scenario_stride;
  int64_t deadline_ns;
  ThreadControl *thread_control;
  PegWorker *workers; // array; indexed by worker_idx
  PegRankedCand *out;
  PegPoll *poll;               // optional live leaderboard; NULL = no polling
  const PegProgress *progress; // optional progress callbacks (NULL = none)
  int cand_rank;               // this cand's index, passed to the callbacks
  // When eval_bag_order_len > 0, evaluate exactly this one bag ordering instead
  // of enumerating scenarios (see PegArgs.eval_bag_order).
  const MachineLetter *eval_bag_order;
  int eval_bag_order_len;
} PegCandJob;

// Evaluate exactly one bag ordering for the cand (no enumeration): the mover
// draws the first ctx->k_drawn tiles of bag_order, the rest stay in the bag,
// and the opponent gets the remaining unseen tiles. Fills the ctx accumulators
// for a single scenario of weight 1. Used by the eval_bag_order path.
static void peg_eval_fixed_ordering(PegEvalCtx *ctx,
                                    const MachineLetter *bag_order, int n_bag) {
  const int k_drawn = ctx->k_drawn;
  const int n_bag_remaining = n_bag - k_drawn;
  MachineLetter mover_drawn[PEG_SCENARIO_ARRAY_CAP + 1] = {0};
  MachineLetter bag_remaining[PEG_SCENARIO_ARRAY_CAP + 1] = {0};
  for (int i = 0; i < k_drawn; i++) {
    mover_drawn[i] = bag_order[i];
  }
  for (int i = 0; i < n_bag_remaining; i++) {
    bag_remaining[i] = bag_order[k_drawn + i];
  }
  Game *game = peg_make_post_cand_game(
      ctx->worker, ctx->template_src, ctx->mover_idx, ctx->unseen, ctx->ld_size,
      k_drawn, mover_drawn, n_bag_remaining, bag_remaining);
  const int32_t value = peg_eval_leaf(ctx, game);
  if (ctx->progress != NULL && ctx->progress->on_scenario_done != NULL) {
    ctx->progress->on_scenario_done(ctx->progress->stage_idx, ctx->cand_idx,
                                    /*scenario_idx=*/0, value, /*weight=*/1,
                                    ctx->progress->user_data);
  }
  ctx->total_weight = 1.0;
  if (value > 0) {
    ctx->win_weight = 1.0;
    ctx->win_count = 1;
  } else if (value == 0) {
    ctx->win_weight = 0.5;
    ctx->tie_count = 1;
  } else {
    ctx->win_weight = 0.0;
  }
  ctx->spread_weight = (double)value;
  ctx->weight_sum = 1;
  ctx->n_scenarios = 1;
}

static void peg_cand_worker_fn(void *arg, int worker_idx) {
  PegCandJob *job = (PegCandJob *)arg;
  // Budget gate: once the wall-clock deadline has passed, skip this candidate's
  // (potentially large) scenario enumeration and emit a sentinel that sorts
  // last (win_pct < 0), so a heavy-rack stage 0 over thousands of candidates
  // cannot blow past the time budget. Candidates dispatched before the deadline
  // still evaluate and rank normally; the partial top-K is what gets published.
  if (peg_should_stop(job->deadline_ns, job->thread_control)) {
    job->out->move = *job->cand;
    job->out->win_pct = -1.0;
    job->out->mean_spread = 0.0;
    job->out->weight_sum = 0;
    job->out->win_count = 0;
    job->out->tie_count = 0;
    job->out->n_scenarios = 0;
    job->out->eval_seconds = 0;
    job->out->spread_unknown = false;
    job->out->cut = false;
    return;
  }
  PegEvalCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.base_game = job->base_game;
  ctx.mover_idx = job->mover_idx;
  ctx.unseen = job->unseen;
  ctx.ld_size = job->ld_size;
  ctx.cand = job->cand;
  ctx.bag_size = job->bag_size;
  ctx.opp_model = job->opp_model;
  ctx.inner_top_k = job->inner_top_k;
  ctx.fidelity_plies = job->fidelity_plies;
  ctx.scenario_stride = job->scenario_stride;
  ctx.deadline_ns = job->deadline_ns;
  ctx.thread_control = job->thread_control;
  ctx.worker = &job->workers[worker_idx];
  // Progress: cand_idx carries this cand's rank for on_scenario_done.
  ctx.progress = job->progress;
  ctx.cand_idx = job->cand_rank;
  // Post-cand board + cross-sets, built once for all this cand's scenarios
  // (job->base_game is the pruned prepared base).
  peg_build_template(ctx.worker, job->base_game, job->cand);
  ctx.template_src = ctx.worker->template_game;
  ctx.reply_cache = ctx.worker->reply_cache;
  peg_reply_cache_reset(ctx.reply_cache);
  const int tiles_played = move_get_tiles_played(job->cand);
  ctx.k_drawn = tiles_played < job->bag_size ? tiles_played : job->bag_size;
  const int n_bag_remaining = job->bag_size - ctx.k_drawn;
  if (job->eval_bag_order_len > 0) {
    // Pinned single scenario: evaluate exactly the caller's bag ordering.
    peg_eval_fixed_ordering(&ctx, job->eval_bag_order, job->eval_bag_order_len);
  } else {
    MachineLetter mover_drawn[PEG_SCENARIO_ARRAY_CAP + 1];
    MachineLetter bag_remaining[PEG_SCENARIO_ARRAY_CAP + 1];
    peg_enum_splits(&ctx, /*ml=*/0, ctx.k_drawn, n_bag_remaining, /*weight=*/1,
                    mover_drawn, 0, bag_remaining, 0);
  }
  job->out->move = *job->cand;
  if (ctx.interrupted) {
    job->out->win_pct = -1.0;
    job->out->mean_spread = 0.0;
    job->out->weight_sum = 0;
    job->out->win_count = 0;
    job->out->tie_count = 0;
    job->out->n_scenarios = 0;
    job->out->eval_seconds = 0;
    job->out->spread_unknown = false;
    job->out->cut = false;
    return;
  }
  job->out->win_pct =
      ctx.total_weight > 0 ? ctx.win_weight / ctx.total_weight : 0.0;
  job->out->mean_spread =
      ctx.total_weight > 0 ? ctx.spread_weight / ctx.total_weight : 0.0;
  job->out->weight_sum = ctx.weight_sum;
  job->out->win_count = ctx.win_count;
  job->out->tie_count = ctx.tie_count;
  job->out->n_scenarios = ctx.n_scenarios;
  job->out->eval_seconds = 0; // greedy stage is not per-candidate timed
  job->out->spread_unknown = false;
  job->out->cut = false;
  // Live poll: surface this finished candidate into the leaderboard so a
  // poller sees stage 0 fill in as candidates resolve.
  const bool reordered = peg_poll_upsert(job->poll, job->out);
  if (job->progress != NULL && job->progress->on_cand_done != NULL) {
    job->progress->on_cand_done(
        job->progress->stage_idx, job->cand_rank, &job->out->move,
        job->out->win_pct, job->out->mean_spread, job->out->n_scenarios,
        ctimer_monotonic_ns(), reordered, job->progress->user_data);
  }
}

static int peg_rank_cmp(const void *lhs, const void *rhs) {
  return peg_cand_compare((const PegRankedCand *)lhs,
                          (const PegRankedCand *)rhs);
}

// True if `move` matches one of the protected moves (snoprune analogue).
// Cheap linear scan: the protected set is tiny.
static bool peg_move_protected(const Move *move,
                               const Move *const *protect_moves,
                               int n_protect) {
  for (int protect_idx = 0; protect_idx < n_protect; protect_idx++) {
    if (compare_moves_without_equity(move, protect_moves[protect_idx], true) ==
        -1) {
      return true;
    }
  }
  return false;
}

// In-place select of the candidates that advance from a sorted ranked[0..
// live_count): the top `keep` unprotected entries plus every protected move,
// compacted into [0, return) with descending order preserved. A protected move
// never takes one of the `keep` slots. Returns the new live count. With no
// protected moves this is just the plain top-`keep`. Each survivor is rotated
// forward past the dropped candidates rather than copied over one, so every
// dropped candidate stays in [return, live_count), still in rank order, for
// the graded ranking.
static int peg_select_survivors(PegRankedCand *ranked, int live_count, int keep,
                                const Move *const *protect_moves,
                                int n_protect) {
  if (n_protect <= 0) {
    return keep < live_count ? keep : live_count;
  }
  int write_idx = 0;
  int unprotected_kept = 0;
  for (int read_idx = 0; read_idx < live_count; read_idx++) {
    if (!peg_move_protected(&ranked[read_idx].move, protect_moves, n_protect)) {
      if (unprotected_kept >= keep) {
        continue;
      }
      unprotected_kept++;
    }
    if (read_idx != write_idx) {
      const PegRankedCand survivor = ranked[read_idx];
      memmove(&ranked[write_idx + 1], &ranked[write_idx],
              (size_t)(read_idx - write_idx) * sizeof(PegRankedCand));
      ranked[write_idx] = survivor;
    }
    write_idx++;
  }
  return write_idx;
}

// Moves any protected candidate to the front of ranked[0..count). A stage's
// live-mode evaluation (see the sequential per-candidate loop in peg_solve)
// scores candidates strictly in array order and stops as soon as the
// deadline passes, so whatever peg_select_survivors appended after the
// top-K cut is evaluated last and is the first thing dropped under a tight
// time budget. Moving it to the front instead guarantees it gets scored
// before the deadline can cut the stage short, mirroring how the endgame
// solver pins its protected actual move to root index 0.
static void peg_force_protected_to_front(PegRankedCand *ranked, int count,
                                         const Move *const *protect_moves,
                                         int n_protect) {
  if (n_protect <= 0 || count <= 1) {
    return;
  }
  int write_idx = 0;
  for (int idx = 0; idx < count; idx++) {
    if (peg_move_protected(&ranked[idx].move, protect_moves, n_protect)) {
      if (idx != write_idx) {
        const PegRankedCand tmp = ranked[write_idx];
        ranked[write_idx] = ranked[idx];
        ranked[idx] = tmp;
      }
      write_idx++;
    }
  }
}

// Same idea as peg_force_protected_to_front, but for stage 0's plain Move
// pointer array (built before any PegRankedCand results exist).
static void
peg_force_protected_move_ptrs_to_front(const Move **moves, int count,
                                       const Move *const *protect_moves,
                                       int n_protect) {
  if (n_protect <= 0 || count <= 1) {
    return;
  }
  int write_idx = 0;
  for (int idx = 0; idx < count; idx++) {
    if (peg_move_protected(moves[idx], protect_moves, n_protect)) {
      if (idx != write_idx) {
        const Move *tmp = moves[write_idx];
        moves[write_idx] = moves[idx];
        moves[idx] = tmp;
      }
      write_idx++;
    }
  }
}

// Append src[from..to) to the graded list, each tagged with the fidelity (ply
// count) at which it was last scored — i.e. the deepest stage it reached.
static void peg_graded_append(PegRankedCand *graded, int *graded_fidelity,
                              int *n_graded, const PegRankedCand *src, int from,
                              int to, int fidelity) {
  for (int src_idx = from; src_idx < to; src_idx++) {
    graded[*n_graded] = src[src_idx];
    graded_fidelity[*n_graded] = fidelity;
    (*n_graded)++;
  }
}

// Evaluate `n` candidate moves at `fidelity_plies`, writing ranked[i] for each.
// Parallel across candidates when a pool is present, else inline.
typedef struct PegSavedCandidate {
  int depth;
  PegRankedCand value;
  PegCandOutcomes *outcomes;
} PegSavedCandidate;

struct PegCheckpoint {
  char *key;
  PegSavedCandidate *entries;
  int count;
};

PegCheckpoint *peg_checkpoint_create(void) {
  PegCheckpoint *checkpoint = calloc_or_die(1, sizeof(*checkpoint));
  return checkpoint;
}

static void peg_checkpoint_clear(PegCheckpoint *checkpoint) {
  for (int index = 0; index < checkpoint->count; index++) {
    peg_cand_outcomes_destroy_array(checkpoint->entries[index].outcomes, 1);
  }
  free(checkpoint->entries);
  free(checkpoint->key);
  memset(checkpoint, 0, sizeof(*checkpoint));
}

void peg_checkpoint_destroy(PegCheckpoint *checkpoint) {
  if (checkpoint) {
    peg_checkpoint_clear(checkpoint);
    free(checkpoint);
  }
}

static const PegSavedCandidate *
peg_checkpoint_find(const PegCheckpoint *checkpoint, int depth,
                    const Move *move) {
  if (checkpoint) {
    for (int index = 0; index < checkpoint->count; index++) {
      const PegSavedCandidate *saved = &checkpoint->entries[index];
      if (saved->depth == depth && peg_move_eq(&saved->value.move, move)) {
        return saved;
      }
    }
  }
  return NULL;
}

static void peg_checkpoint_save(PegCheckpoint *checkpoint, int depth,
                                const PegRankedCand *value,
                                const PegCandOutcomes *outcomes) {
  if (!checkpoint || value->win_pct < 0 || value->weight_sum <= 0 ||
      peg_checkpoint_find(checkpoint, depth, &value->move)) {
    return;
  }
  checkpoint->entries =
      realloc_or_die(checkpoint->entries, (size_t)(checkpoint->count + 1) *
                                              sizeof(*checkpoint->entries));
  PegSavedCandidate *saved = &checkpoint->entries[checkpoint->count++];
  saved->depth = depth;
  saved->value = *value;
  saved->outcomes = peg_cand_outcomes_dup_array(outcomes, 1);
}

// Time budgets and worker counts can change on resume; search semantics cannot.
static void peg_checkpoint_prepare(const PegArgs *args) {
  PegCheckpoint *checkpoint = args->checkpoint;
  if (!checkpoint) {
    return;
  }
  StringBuilder *key = string_builder_create();
  char *cgp = game_get_cgp(args->game, false);
  string_builder_add_string(key, cgp);
  free(cgp);
  for (int player = 0; player < 2; player++) {
    string_builder_add_formatted_string(
        key, "|%s",
        kwg_get_name(player_get_kwg(game_get_player(args->game, player))));
  }
  string_builder_add_formatted_string(
      key, "|%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", args->max_stage,
      args->greedy_seed_only, args->inner_top_k, args->opp_model,
      args->scenario_stride, args->nested_enabled, args->nested_cand_cap,
      args->nested_stride, args->nested_emptier_ply_cap, args->nested_max_depth,
      args->include_per_scenario, game_get_player_on_turn_index(args->game),
      args->nested_score_actual, args->nested_pessimistic, args->first_win);
  string_builder_add_formatted_string(
      key, "|%d,%d,%d", args->first_win_intermediate,
      args->skip_noncutting_stages, args->final_stage_cutoff);
  string_builder_add_formatted_string(
      key, "|%d,%d,%d,%d", args->early_cutoff, args->opp_rack_known,
      args->limit_tiles_left, args->max_tiles_left);
  string_builder_add_formatted_string(
      key, "|%llu,%d,%d,%d",
      (unsigned long long)game_get_ld(args->game)->content_fingerprint,
      game_get_bingo_bonus(args->game),
      game_get_max_scoreless_turns(args->game), game_get_variant(args->game));
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      string_builder_add_formatted_string(
          key, ",%u",
          board_get_bonus_square(game_get_board(args->game), row, col).raw);
    }
  }
  const int *arrays[] = {args->stage_top_k, args->nested_cand_caps,
                         args->nested_strides_by_bag};
  const int lengths[] = {args->num_stages, args->nested_n_cand_caps,
                         args->nested_n_strides_by_bag};
  for (int array = 0; array < 3; array++) {
    string_builder_add_string(key, "|");
    for (int index = 0; arrays[array] && index < lengths[array]; index++) {
      string_builder_add_formatted_string(key, "%d,", arrays[array][index]);
    }
  }
  string_builder_add_string(key, "|");
  for (int index = 0; index < args->eval_bag_order_len; index++) {
    string_builder_add_formatted_string(key, "%d,",
                                        args->eval_bag_order[index]);
  }
  const Move *const *sets[] = {args->only_moves, args->protect_moves};
  const int sizes[] = {args->n_only_moves, args->n_protect_moves};
  for (int set = 0; set < 2; set++) {
    string_builder_add_string(key, "|");
    for (int index = 0; index < sizes[set]; index++) {
      string_builder_add_move(key, game_get_board(args->game), sets[set][index],
                              game_get_ld(args->game), false);
      string_builder_add_string(key, ",");
    }
  }
  char *text = string_builder_dump_and_destroy(key, NULL);
  if (!args->resume || !checkpoint->key ||
      !strings_equal(checkpoint->key, text)) {
    peg_checkpoint_clear(checkpoint);
    checkpoint->key = text;
  } else {
    free(text);
  }
}

static void peg_eval_candidates(
    PegPool *pool, PegWorker *workers, const Game *game, int mover_idx,
    const uint8_t *unseen, int ld_size, int bag_size, const Move *const *cands,
    int n, PegOppModel opp_model, int inner_top_k, int fidelity_plies,
    int scenario_stride, int64_t deadline_ns, ThreadControl *thread_control,
    PegPoll *poll, const PegProgress *progress,
    const MachineLetter *eval_bag_order, int eval_bag_order_len,
    PegRankedCand *ranked, PegCheckpoint *checkpoint) {
  PegCandJob *jobs = malloc_or_die((size_t)n * sizeof(PegCandJob));
  for (int i = 0; i < n; i++) {
    jobs[i].base_game = game;
    jobs[i].mover_idx = mover_idx;
    jobs[i].unseen = unseen;
    jobs[i].ld_size = ld_size;
    jobs[i].cand = cands[i];
    jobs[i].bag_size = bag_size;
    jobs[i].opp_model = opp_model;
    jobs[i].inner_top_k = inner_top_k;
    jobs[i].fidelity_plies = fidelity_plies;
    jobs[i].scenario_stride = scenario_stride;
    jobs[i].deadline_ns = deadline_ns;
    jobs[i].thread_control = thread_control;
    jobs[i].workers = workers;
    jobs[i].out = &ranked[i];
    jobs[i].poll = poll;
    jobs[i].progress = progress;
    jobs[i].cand_rank = i;
    jobs[i].eval_bag_order = eval_bag_order;
    jobs[i].eval_bag_order_len = eval_bag_order_len;
  }
  void **ptrs = malloc_or_die((size_t)n * sizeof(void *));
  int pending = 0;
  for (int index = 0; index < n; index++) {
    const PegSavedCandidate *saved =
        peg_checkpoint_find(checkpoint, 0, cands[index]);
    if (saved) {
      ranked[index] = saved->value;
      peg_poll_upsert(poll, &ranked[index]);
    } else {
      ptrs[pending++] = &jobs[index];
    }
  }
  if (pool) {
    // Helper (main) thread uses the scratch slot past the worker range.
    peg_pool_submit_and_wait(pool, peg_cand_worker_fn, ptrs, pending,
                             peg_pool_num_workers(pool));
  } else {
    for (int index = 0; index < pending; index++) {
      peg_cand_worker_fn(ptrs[index], 0);
    }
  }
  free(ptrs);
  for (int index = 0; index < n; index++) {
    peg_checkpoint_save(checkpoint, 0, &ranked[index], NULL);
  }
  free(jobs);
}

// Starts tracking the scenario jobs of the candidate in progress, list->count
// of them, each a mover draw, all queued, in the order they are solved.
static void peg_poll_start_scenarios(PegPoll *poll,
                                     const PegScenarioJobList *list) {
  if (poll == NULL) {
    return;
  }
  cpthread_mutex_lock(&poll->mutex);
  PegStageSnapshot *cur = peg_poll_open_stage(&poll->s);
  if (cur != NULL) {
    cur->scenarios_done = 0;
    cur->scenarios_total = list->count;
  }
  if (list->count > poll->draws_cap) {
    poll->draws_cap = list->count;
    poll->draws = realloc_or_die(poll->draws, (size_t)poll->draws_cap *
                                                  sizeof(PegDrawStatus));
  }
  poll->n_draws = list->count;
  for (int job_idx = 0; job_idx < list->count; job_idx++) {
    const PegScenarioJob *job = &list->jobs[job_idx];
    PegDrawStatus *draw = &poll->draws[job_idx];
    memset(draw, 0, sizeof(*draw));
    draw->n_drawn = job->k_drawn;
    memcpy(draw->drawn, job->mover_drawn,
           (size_t)job->k_drawn * sizeof(MachineLetter));
    draw->n_bag = job->n_bag_remaining;
    memcpy(draw->bag, job->bag_remaining,
           (size_t)job->n_bag_remaining * sizeof(MachineLetter));
    draw->state = PEG_DRAW_QUEUED;
    draw->worker_idx = -1;
  }
  poll->s.version++;
  cpthread_mutex_unlock(&poll->mutex);
}

// Marks draw `draw_idx` of the candidate in progress running on worker
// `worker_idx`. Called from worker threads, so it locks; a job is a whole
// mover draw, so this is rare next to the work it marks.
static void peg_poll_start_draw(PegPoll *poll, int draw_idx, int worker_idx) {
  if (poll == NULL) {
    return;
  }
  cpthread_mutex_lock(&poll->mutex);
  if (draw_idx >= 0 && draw_idx < poll->n_draws) {
    PegDrawStatus *draw = &poll->draws[draw_idx];
    draw->state = PEG_DRAW_RUNNING;
    draw->worker_idx = worker_idx;
    draw->start_ns = ctimer_monotonic_ns();
  }
  poll->s.version++;
  cpthread_mutex_unlock(&poll->mutex);
}

// Records how `job`'s draw finished: solved, with its results, or stopped
// (`solved` false), and counts it among the candidate's solved (or skipped)
// scenario jobs.
static void peg_poll_finish_draw(PegPoll *poll, const PegScenarioJob *job,
                                 bool solved, bool spread_unknown) {
  if (poll == NULL) {
    return;
  }
  cpthread_mutex_lock(&poll->mutex);
  PegStageSnapshot *cur = peg_poll_open_stage(&poll->s);
  if (cur != NULL) {
    cur->scenarios_done++;
  }
  if (job->draw_idx >= 0 && job->draw_idx < poll->n_draws) {
    PegDrawStatus *draw = &poll->draws[job->draw_idx];
    draw->state = solved ? PEG_DRAW_DONE : PEG_DRAW_STOPPED;
    draw->end_ns = ctimer_monotonic_ns();
    if (draw->start_ns == 0) {
      draw->start_ns = draw->end_ns;
    }
    draw->weight_sum = job->weight_sum;
    draw->win_count = job->win_count;
    draw->tie_count = job->tie_count;
    draw->mean_spread =
        job->total_weight > 0 ? job->spread_weight / job->total_weight : 0.0;
    draw->spread_unknown = spread_unknown;
  }
  poll->s.version++;
  cpthread_mutex_unlock(&poll->mutex);
}

// Marks `job`'s draw as the one worker_idx is solving for its candidate solved
// at once with others, or (finished) counts it solved.
static void peg_poll_cand_draw(PegPoll *poll, int worker_idx,
                               const PegScenarioJob *job, bool finished) {
  if (poll == NULL) {
    return;
  }
  cpthread_mutex_lock(&poll->mutex);
  if (worker_idx < poll->n_cands && poll->cands[worker_idx].start_ns != 0) {
    PegCandStatus *cand = &poll->cands[worker_idx];
    PegDrawStatus *draw = &cand->draw;
    if (finished) {
      cand->draws_done++;
      draw->state = PEG_DRAW_QUEUED;
    } else {
      draw->n_drawn = job->k_drawn;
      memcpy(draw->drawn, job->mover_drawn,
             (size_t)job->k_drawn * sizeof(MachineLetter));
      draw->n_bag = job->n_bag_remaining;
      memcpy(draw->bag, job->bag_remaining,
             (size_t)job->n_bag_remaining * sizeof(MachineLetter));
      draw->state = PEG_DRAW_RUNNING;
      draw->start_ns = ctimer_monotonic_ns();
    }
  }
  poll->s.version++;
  cpthread_mutex_unlock(&poll->mutex);
}

// Evaluate one scenario job (a single cand x mover-draw split), folding its
// orderings into the job's own result fields.
static void peg_scenario_worker_fn(void *arg, int worker_idx) {
  PegScenarioJob *job = (PegScenarioJob *)arg;
  const bool track_candidate_completion =
      job->progress != NULL && job->progress->on_cand_done != NULL;
  // Past the deadline: skip this scenario entirely (its result fields are
  // already zero from job creation) so a candidate whose evaluation straddles
  // the cutoff winds down within one in-flight job instead of running every
  // remaining scenario to completion. The caller drops such partial candidates.
  if (peg_should_stop(job->deadline_ns, job->thread_control) ||
      peg_cutoff_is_cut(job->cutoff)) {
    if (track_candidate_completion) {
      job->completed = false;
      job->completed_ns = ctimer_monotonic_ns();
    }
    peg_poll_finish_draw(job->scenario_poll, job, /*solved=*/false,
                         /*spread_unknown=*/true);
    return;
  }
  peg_poll_start_draw(job->scenario_poll, job->draw_idx, worker_idx);
  peg_poll_cand_draw(job->cand_poll, worker_idx, job, /*finished=*/false);
  PegEvalCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.template_src = job->template_src;
  ctx.mover_idx = job->mover_idx;
  ctx.unseen = job->unseen;
  ctx.ld_size = job->ld_size;
  ctx.k_drawn = job->k_drawn;
  ctx.opp_model = job->opp_model;
  ctx.inner_top_k = job->inner_top_k;
  ctx.fidelity_plies = job->fidelity_plies;
  ctx.injection_cap = job->injection_cap;
  ctx.deadline_ns = job->deadline_ns;
  ctx.thread_control = job->thread_control;
  ctx.worker = &job->workers[worker_idx];
  // Progress: cand_idx carries this cand's rank for on_scenario_done.
  ctx.progress = job->progress;
  ctx.cand_idx = job->cand_idx;
  ctx.cutoff = job->cutoff;
  // Per-ordering capture into the job's own sink (single-threaded per job).
  if (job->do_capture) {
    job->cap.ld = job->ld;
    ctx.capture = &job->cap;
  }
  // peg_eval_split reads bag_remaining (permuting a local copy), so passing the
  // job's own copy directly is fine.
  peg_eval_split(&ctx, job->mover_drawn, job->n_bag_remaining,
                 job->bag_remaining, job->weight);
  job->total_weight = ctx.total_weight;
  job->win_weight = ctx.win_weight;
  job->spread_weight = ctx.spread_weight;
  job->weight_sum = ctx.weight_sum;
  job->win_count = ctx.win_count;
  job->tie_count = ctx.tie_count;
  job->n_scenarios = ctx.n_scenarios;
  if (track_candidate_completion) {
    job->completed = !ctx.interrupted;
    job->completed_ns = ctimer_monotonic_ns();
  }
  peg_poll_finish_draw(job->scenario_poll, job, /*solved=*/!ctx.interrupted,
                       /*spread_unknown=*/ctx.worker->first_win &&
                           job->fidelity_plies > 0);
  peg_poll_cand_draw(job->cand_poll, worker_idx, job, /*finished=*/true);
}

// True when two moves are the same play (type, position, tiles).
// Insert or replace a candidate's captured outcomes in the store, keyed by
// move. Since the halving stages re-score survivors at deeper fidelity, a later
// (deeper) capture for the same move replaces the earlier one, so each move
// ends up with its deepest-fidelity rows. Takes ownership of `one`'s rows.
static void peg_outcomes_store_upsert(PegCandOutcomes **store, int *n, int *cap,
                                      const PegCandOutcomes *one) {
  for (int store_idx = 0; store_idx < *n; store_idx++) {
    if (peg_move_eq(&(*store)[store_idx].move, &one->move)) {
      peg_scenario_rows_destroy((*store)[store_idx].rows,
                                (*store)[store_idx].n_rows);
      (*store)[store_idx] = *one;
      return;
    }
  }
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 16;
    *store = realloc_or_die(*store, (size_t)*cap * sizeof(PegCandOutcomes));
  }
  (*store)[(*n)++] = *one;
}

static int peg_scenario_job_greedy_cmp(const void *lhs, const void *rhs) {
  const int32_t a = ((const PegScenarioJob *)lhs)->greedy_value;
  const int32_t b = ((const PegScenarioJob *)rhs)->greedy_value;
  return (a > b) - (a < b);
}

// Orders a cutoff solve's scenario jobs likeliest losses first, by a greedy
// playout of each split's first ordering, so a candidate that cannot reach
// the best is cut after as few solves as possible. Runs on `worker`, the
// calling thread's scratch.
static void peg_order_jobs_losses_first(PegScenarioJobList *list,
                                        PegWorker *worker, int mover_idx,
                                        int64_t deadline_ns,
                                        ThreadControl *thread_control) {
  for (int job_idx = 0; job_idx < list->count; job_idx++) {
    PegScenarioJob *job = &list->jobs[job_idx];
    MachineLetter first_ordering[PEG_SCENARIO_ARRAY_CAP + 1] = {0};
    for (int tile_idx = 0; tile_idx < job->n_bag_remaining; tile_idx++) {
      first_ordering[tile_idx] = job->bag_remaining[tile_idx];
    }
    for (int tile_idx = 1; tile_idx < job->n_bag_remaining; tile_idx++) {
      const MachineLetter key = first_ordering[tile_idx];
      int insert_idx = tile_idx - 1;
      while (insert_idx >= 0 && first_ordering[insert_idx] > key) {
        first_ordering[insert_idx + 1] = first_ordering[insert_idx];
        insert_idx--;
      }
      first_ordering[insert_idx + 1] = key;
    }
    Game *game = peg_make_post_cand_game(
        worker, job->template_src, mover_idx, job->unseen, job->ld_size,
        job->k_drawn, job->mover_drawn, job->n_bag_remaining, first_ordering);
    job->greedy_value =
        peg_greedy_playout(game, mover_idx, worker->playout_ml, deadline_ns,
                           thread_control, /*interrupted=*/NULL,
                           /*shared_plies=*/NULL);
  }
  stable_sort(list->jobs, (size_t)list->count, sizeof(PegScenarioJob),
              peg_scenario_job_greedy_cmp);
}

// Scenario-level evaluation: build one shared post-cand template per candidate,
// expand every (cand, mover-draw split) into a job, run them all across the
// pool, then reduce per candidate. This keeps all cores busy even when a stage
// has only a few candidates (the deep stages), where per-candidate parallelism
// would leave most cores idle. caller_worker_idx is the calling thread's
// scratch worker: the pool's helper slot, or with no pool the worker that runs
// every job. injection_cap (the pool's size, 0 for none) opens each leaf
// endgame's window so the monitor can lend idle cores to the long ones.
static void peg_eval_candidates_scenario(
    PegPool *pool, PegWorker *workers, const Game *prepared_base, int mover_idx,
    const uint8_t *unseen, int ld_size, const LetterDistribution *ld,
    int bag_size, const Move *const *cands, int n, PegOppModel opp_model,
    int inner_top_k, int fidelity_plies, int scenario_stride,
    int64_t deadline_ns, ThreadControl *thread_control,
    const PegProgress *progress, PegPoll *poll, PegRankedCand *ranked,
    PegCandOutcomes *out_outcomes, PegCutoff *cutoff, PegPoll *scenario_poll,
    PegPoll *cand_poll, int caller_worker_idx, int injection_cap) {
  // Shared per-cand templates: post-cand board + cross-sets + pruned override,
  // built once and read concurrently by the scenario workers.
  Game **templates = malloc_or_die((size_t)n * sizeof(Game *));
  for (int i = 0; i < n; i++) {
    templates[i] = game_duplicate(prepared_base);
    play_move_without_drawing_tiles(cands[i], templates[i]);
    game_gen_all_cross_sets(templates[i]);
  }

  // Expand all (cand, split) jobs.
  PegScenarioJobList list = {0};
  for (int i = 0; i < n; i++) {
    PegEvalCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.template_src = templates[i];
    ctx.mover_idx = mover_idx;
    ctx.unseen = unseen;
    ctx.ld_size = ld_size;
    ctx.opp_model = opp_model;
    ctx.inner_top_k = inner_top_k;
    ctx.fidelity_plies = fidelity_plies;
    ctx.injection_cap = injection_cap;
    ctx.scenario_stride = scenario_stride;
    ctx.deadline_ns = deadline_ns;
    ctx.thread_control = thread_control;
    ctx.workers = workers;
    ctx.out_jobs = &list;
    ctx.cand_idx = i;
    ctx.progress = progress;
    ctx.cutoff = cutoff;
    const int tiles_played = move_get_tiles_played(cands[i]);
    ctx.k_drawn = tiles_played < bag_size ? tiles_played : bag_size;
    const int n_bag_remaining = bag_size - ctx.k_drawn;
    MachineLetter mover_drawn[PEG_SCENARIO_ARRAY_CAP + 1];
    MachineLetter bag_remaining[PEG_SCENARIO_ARRAY_CAP + 1];
    peg_enum_splits(&ctx, /*ml=*/0, ctx.k_drawn, n_bag_remaining, /*weight=*/1,
                    mover_drawn, 0, bag_remaining, 0);
  }

  if (cutoff != NULL) {
    peg_order_jobs_losses_first(&list, &workers[caller_worker_idx], mover_idx,
                                deadline_ns, thread_control);
  }
  for (int job_idx = 0; job_idx < list.count; job_idx++) {
    list.jobs[job_idx].scenario_poll = scenario_poll;
    list.jobs[job_idx].draw_idx = job_idx;
    list.jobs[job_idx].cand_poll = cand_poll;
  }
  peg_poll_start_scenarios(scenario_poll, &list);
  peg_poll_set_cand_draws(cand_poll, caller_worker_idx, list.count);

  // Enable per-ordering capture on every job when the caller asked for it.
  if (out_outcomes != NULL) {
    for (int job_idx = 0; job_idx < list.count; job_idx++) {
      list.jobs[job_idx].do_capture = true;
      list.jobs[job_idx].ld = ld;
    }
  }

  // Run every scenario job.
  if (pool && list.count > 0) {
    void **ptrs = malloc_or_die((size_t)list.count * sizeof(void *));
    for (int j = 0; j < list.count; j++) {
      ptrs[j] = &list.jobs[j];
    }
    peg_pool_submit_and_wait(pool, peg_scenario_worker_fn, ptrs, list.count,
                             peg_pool_num_workers(pool));
    free(ptrs);
  } else {
    for (int j = 0; j < list.count; j++) {
      peg_scenario_worker_fn(&list.jobs[j], caller_worker_idx);
    }
  }

  // Reduce the per-split results back into per-candidate rankings.
  double *total_w = calloc_or_die((size_t)n, sizeof(double));
  double *win_w = calloc_or_die((size_t)n, sizeof(double));
  double *spread_w = calloc_or_die((size_t)n, sizeof(double));
  const bool track_candidate_completion =
      progress != NULL && progress->on_cand_done != NULL;
  bool *candidate_completed = track_candidate_completion
                                  ? malloc_or_die((size_t)n * sizeof(bool))
                                  : NULL;
  int64_t *candidate_completed_ns =
      track_candidate_completion ? calloc_or_die((size_t)n, sizeof(int64_t))
                                 : NULL;
  for (int i = 0; i < n; i++) {
    ranked[i].move = *cands[i];
    ranked[i].weight_sum = 0;
    ranked[i].win_count = 0;
    ranked[i].tie_count = 0;
    ranked[i].n_scenarios = 0;
    ranked[i].eval_seconds = 0; // set per candidate by the live caller
    ranked[i].spread_unknown = workers[0].first_win && fidelity_plies > 0;
    ranked[i].cut = false;
    if (track_candidate_completion) {
      candidate_completed[i] = true;
    }
  }
  for (int j = 0; j < list.count; j++) {
    const PegScenarioJob *job = &list.jobs[j];
    const int i = job->cand_idx;
    total_w[i] += job->total_weight;
    win_w[i] += job->win_weight;
    spread_w[i] += job->spread_weight;
    ranked[i].weight_sum += job->weight_sum;
    ranked[i].win_count += job->win_count;
    ranked[i].tie_count += job->tie_count;
    ranked[i].n_scenarios += job->n_scenarios;
    if (track_candidate_completion) {
      candidate_completed[i] = candidate_completed[i] && job->completed;
      if (job->completed_ns > candidate_completed_ns[i]) {
        candidate_completed_ns[i] = job->completed_ns;
      }
    }
  }
  // Concatenate each candidate's per-ordering capture rows (in enumeration
  // order) into out_outcomes, renumbering scenario_idx sequentially per cand.
  // Then release the job-local capture buffers.
  if (out_outcomes != NULL) {
    int *nrows = calloc_or_die((size_t)n, sizeof(int));
    for (int job_idx = 0; job_idx < list.count; job_idx++) {
      nrows[list.jobs[job_idx].cand_idx] += list.jobs[job_idx].cap.count;
    }
    for (int cand_idx = 0; cand_idx < n; cand_idx++) {
      out_outcomes[cand_idx].move = *cands[cand_idx];
      out_outcomes[cand_idx].fidelity = fidelity_plies;
      out_outcomes[cand_idx].n_rows = 0;
      out_outcomes[cand_idx].rows =
          nrows[cand_idx] > 0
              ? malloc_or_die((size_t)nrows[cand_idx] * sizeof(PegPerScenario))
              : NULL;
    }
    free(nrows);
    for (int job_idx = 0; job_idx < list.count; job_idx++) {
      const PegScenarioJob *job = &list.jobs[job_idx];
      PegCandOutcomes *oc = &out_outcomes[job->cand_idx];
      for (int row_idx = 0; row_idx < job->cap.count; row_idx++) {
        PegPerScenario *dst = &oc->rows[oc->n_rows];
        *dst = job->cap.rows[row_idx];
        dst->scenario_idx = oc->n_rows;
        oc->n_rows++;
      }
    }
  }
  for (int job_idx = 0; job_idx < list.count; job_idx++) {
    free(list.jobs[job_idx].cap.rows);
  }
  for (int i = 0; i < n; i++) {
    ranked[i].win_pct = total_w[i] > 0 ? win_w[i] / total_w[i] : 0.0;
    ranked[i].mean_spread = total_w[i] > 0 ? spread_w[i] / total_w[i] : 0.0;
    if (track_candidate_completion && candidate_completed[i] &&
        ranked[i].n_scenarios > 0) {
      // This barrier path (benchmarks) has no incremental sorted insert, so
      // there is no append-vs-reorder distinction: treat each as a full redraw.
      progress->on_cand_done(progress->stage_idx, i, &ranked[i].move,
                             ranked[i].win_pct, ranked[i].mean_spread,
                             ranked[i].n_scenarios, candidate_completed_ns[i],
                             /*reordered=*/true, progress->user_data);
    }
    peg_poll_bump_cand_done(poll);
  }
  free(total_w);
  free(win_w);
  free(spread_w);
  free(candidate_completed);
  free(candidate_completed_ns);
  free(list.jobs);
  for (int i = 0; i < n; i++) {
    game_destroy(templates[i]);
  }
  free(templates);
}

// ----- live stages ---------------------------------------------------------

// A live-mode stage (a poll, a checkpoint or a cutoff): each candidate is
// published to the poll, the checkpoint and the caller as soon as it is
// scored. Solved one after another, each gets the whole pool for its
// scenarios. With a cutoff and a pool, only the first is: it sets the bar,
// and the rest are solved at once, one per thread, each its draws in turn, so
// a thread spends little more on a losing candidate than the draws that prove
// it loses (spread over the pool, every draw starts before the first loss is
// found, and the draws a candidate wins are the expensive ones).
typedef struct PegLiveStage {
  const PegArgs *args;
  PegWorker *workers;
  const Game *prepared_base;
  int mover_idx;
  const uint8_t *unseen;
  int ld_size;
  const LetterDistribution *ld;
  int bag_size;
  int injection_cap; // the pool's size, so idle threads can join endgames
  int fidelity;
  int scenario_stride;
  int64_t deadline_ns;
  const Move *const *protect_moves;
  int n_protect;
  bool cutoff_on;
  const PegProgress *inner; // per-scenario progress; no on_cand_done
  int stage_idx;
  // moves[cand_idx] is ranked[cand_idx].move: the candidate as the previous
  // stage scored it, whose weight_sum a complete score here must reach.
  const Move **moves;
  const PegRankedCand *ranked;
  PegRankedCand *restaged;         // this stage's scores
  PegCandOutcomes *stage_outcomes; // NULL unless include_per_scenario
  bool *complete;                  // scored in full within budget
  // The best finished, uncut candidate's score in half-orderings (2 * wins +
  // draws); -1 before the first.
  _Atomic int64_t best_score;
  // Serializes the checkpoint and the publishing of candidates solved at once.
  cpthread_mutex_t mutex;
} PegLiveStage;

// Scores candidate cand_idx into restaged[cand_idx]: from the checkpoint when
// it is saved there, else solving it, with the pool on its scenarios or, with
// no pool, on scratch worker_idx a draw at a time. A candidate solved `alone`
// is the poll's candidate in progress. Publishes it if complete, and returns
// whether it is.
static bool peg_live_solve_cand(PegLiveStage *stage, int cand_idx,
                                PegPool *pool, int worker_idx, bool alone) {
  const PegArgs *args = stage->args;
  PegRankedCand *result = &stage->restaged[cand_idx];
  PegCandOutcomes *outcomes =
      stage->stage_outcomes ? &stage->stage_outcomes[cand_idx] : NULL;
  PegPoll *alone_poll = alone ? args->poll : NULL;
  Timer cand_timer;
  ctimer_start(&cand_timer);
  peg_poll_set_evaluating(alone_poll, cand_idx, ctimer_monotonic_ns(),
                          stage->moves[cand_idx]);
  cpthread_mutex_lock(&stage->mutex);
  const PegSavedCandidate *saved = peg_checkpoint_find(
      args->checkpoint, stage->fidelity, stage->moves[cand_idx]);
  if (saved) {
    *result = saved->value;
    if (outcomes) {
      PegCandOutcomes *copy = peg_cand_outcomes_dup_array(saved->outcomes, 1);
      *outcomes = *copy;
      free(copy);
    }
  }
  cpthread_mutex_unlock(&stage->mutex);
  const int64_t full_weight = stage->ranked[cand_idx].weight_sum;
  if (!saved) {
    // A protected move (analyze's played move) is always solved in full: its
    // exact win% is the point.
    const bool can_cut =
        stage->cutoff_on &&
        !peg_move_protected(stage->moves[cand_idx], stage->protect_moves,
                            stage->n_protect);
    PegCutoff cutoff;
    atomic_init(&cutoff.deficit, 0);
    atomic_init(&cutoff.cut, false);
    cutoff.full_score = 2 * full_weight;
    cutoff.best_score = &stage->best_score;
    peg_eval_candidates_scenario(
        pool, stage->workers, stage->prepared_base, stage->mover_idx,
        stage->unseen, stage->ld_size, stage->ld, stage->bag_size,
        &stage->moves[cand_idx], 1, args->opp_model, args->inner_top_k,
        stage->fidelity, stage->scenario_stride, stage->deadline_ns,
        args->thread_control, stage->inner, /*poll=*/NULL, result, outcomes,
        can_cut ? &cutoff : NULL, alone_poll, alone ? NULL : args->poll,
        worker_idx, stage->injection_cap);
    result->eval_seconds = ctimer_elapsed_seconds(&cand_timer);
    // A candidate whose last draw cut it has solved every ordering anyway,
    // often losing them all, so it keeps its exact counts.
    if (can_cut && atomic_load(&cutoff.cut) &&
        result->weight_sum < full_weight) {
      // Cut: it cannot reach the best. Report the most it could have reached,
      // and drop its partial outcome rows.
      result->cut = true;
      result->spread_unknown = true;
      result->weight_sum = full_weight;
      result->win_pct =
          (double)(cutoff.full_score - atomic_load(&cutoff.deficit)) /
          (double)cutoff.full_score;
      if (outcomes) {
        peg_scenario_rows_destroy(outcomes->rows, outcomes->n_rows);
        outcomes->rows = NULL;
        outcomes->n_rows = 0;
      }
    }
  }
  if (!result->cut && result->weight_sum >= full_weight) {
    const int64_t score = 2 * result->win_count + result->tie_count;
    int64_t best = atomic_load(&stage->best_score);
    while (score > best &&
           !atomic_compare_exchange_weak(&stage->best_score, &best, score)) {
    }
  }
  peg_poll_set_evaluating(alone_poll, -1, 0, NULL);
  // A deadline or user interrupt can leave scenarios unscored. Require the
  // full weight carried from this candidate's preceding stage before
  // publishing its score or captured outcomes.
  if (peg_should_stop(stage->deadline_ns, args->thread_control) ||
      result->weight_sum < full_weight) {
    if (outcomes) {
      peg_scenario_rows_destroy(outcomes->rows, outcomes->n_rows);
      outcomes->rows = NULL;
      outcomes->n_rows = 0;
    }
    return false;
  }
  cpthread_mutex_lock(&stage->mutex);
  peg_checkpoint_save(args->checkpoint, stage->fidelity, result, outcomes);
  peg_poll_upsert_outcomes(args->poll, outcomes);
  // Surface this finished candidate into the leaderboard, then stream the
  // updated ranking to the caller right away -- every candidate's result
  // prints as soon as it finishes, so the deep stages fill in row by row.
  // reordered tells the renderer whether the candidate slotted in above the
  // bottom (redraw the whole list) or sorted to the bottom as the new worst
  // (just append its row).
  const bool reordered = peg_poll_upsert(args->poll, result);
  if (args->on_cand_done != NULL) {
    args->on_cand_done(stage->stage_idx, cand_idx, &result->move,
                       result->win_pct, result->mean_spread,
                       result->n_scenarios, ctimer_monotonic_ns(), reordered,
                       args->user_data);
  }
  cpthread_mutex_unlock(&stage->mutex);
  return true;
}

typedef struct PegLiveJob {
  PegLiveStage *stage;
  int cand_idx;
} PegLiveJob;

static void peg_live_job_fn(void *arg, int worker_idx) {
  const PegLiveJob *job = (const PegLiveJob *)arg;
  PegLiveStage *stage = job->stage;
  if (peg_should_stop(stage->deadline_ns, stage->args->thread_control)) {
    return;
  }
  peg_poll_set_cand(stage->args->poll, worker_idx, stage->moves[job->cand_idx]);
  stage->complete[job->cand_idx] =
      peg_live_solve_cand(stage, job->cand_idx, /*pool=*/NULL, worker_idx,
                          /*alone=*/false);
  peg_poll_set_cand(stage->args->poll, worker_idx, NULL);
}

// Solves candidates [first, count) at once, one per pool thread (the caller
// helping), and moves every complete one, in order, ahead of the rest, with
// its previous-stage record (ranked) and outcomes. Returns how many complete
// candidates lead, counting the first `first`, which must be complete.
static int peg_live_solve_at_once(PegLiveStage *stage, PegPool *pool,
                                  PegRankedCand *ranked, int first, int count) {
  PegLiveJob *jobs = malloc_or_die((size_t)count * sizeof(PegLiveJob));
  void **ptrs = malloc_or_die((size_t)count * sizeof(void *));
  for (int cand_idx = first; cand_idx < count; cand_idx++) {
    jobs[cand_idx].stage = stage;
    jobs[cand_idx].cand_idx = cand_idx;
    ptrs[cand_idx - first] = &jobs[cand_idx];
  }
  peg_pool_submit_and_wait(pool, peg_live_job_fn, ptrs, count - first,
                           peg_pool_num_workers(pool));
  free(ptrs);
  free(jobs);
  int done = first;
  for (int cand_idx = first; cand_idx < count; cand_idx++) {
    if (!stage->complete[cand_idx]) {
      continue;
    }
    // Rotate it down to slot `done`, keeping the incomplete ones in order.
    const PegRankedCand old_rec = ranked[cand_idx];
    const PegRankedCand new_rec = stage->restaged[cand_idx];
    PegCandOutcomes oc = {0};
    if (stage->stage_outcomes) {
      oc = stage->stage_outcomes[cand_idx];
    }
    for (int slot = cand_idx; slot > done; slot--) {
      ranked[slot] = ranked[slot - 1];
      stage->restaged[slot] = stage->restaged[slot - 1];
      if (stage->stage_outcomes) {
        stage->stage_outcomes[slot] = stage->stage_outcomes[slot - 1];
      }
    }
    ranked[done] = old_rec;
    stage->restaged[done] = new_rec;
    if (stage->stage_outcomes) {
      stage->stage_outcomes[done] = oc;
    }
    done++;
  }
  return done;
}

// ----- result publishing ---------------------------------------------------

static void peg_publish(PegResult *out, const PegRankedCand *ranked, int count,
                        int stage) {
  free(out->top_cands);
  out->top_cands = malloc_or_die((size_t)count * sizeof(PegRankedCand));
  memcpy(out->top_cands, ranked, (size_t)count * sizeof(PegRankedCand));
  out->n_top_cands = count;
  out->best_move = ranked[0].move;
  out->best_win = ranked[0].win_pct;
  out->best_spread = ranked[0].mean_spread;
  out->last_completed_stage = stage;
}

// ----- injection monitor ---------------------------------------------------

typedef struct PegInjector {
  PegPool *pool;
  PegWorker *workers;
  int n_workers;
  int core_target; // cap on total live ABDADA workers across active endgames
  atomic_int stop;
} PegInjector;

// Background monitor: while pool workers are idle, lend a core to the
// least-parallelized in-flight leaf endgame by injecting an ABDADA helper.
// Keeps the total live ABDADA worker count (masters + helpers) near the core
// target, so the few long deep-stage endgames soak up the otherwise-idle cores.
static void *peg_injector_main(void *arg) {
  // Only help endgames that have been running at least this long: the many
  // sub-millisecond leaf solves close before this, so we never pay ABDADA
  // spawn/coordination overhead on them — only the few long "monster" endgames
  // (the deep-stage realized lines) get helpers.
  const int64_t min_age_ns = 30LL * 1000 * 1000; // 30 ms
  PegInjector *inj = (PegInjector *)arg;
  while (!atomic_load(&inj->stop)) {
    if (peg_pool_idle_workers(inj->pool) > 0) {
      const int64_t now = ctimer_monotonic_ns();
      int total_live = 0;
      EndgameCtx *least = NULL;
      int least_live = INT_MAX;
      for (int worker_idx = 0; worker_idx < inj->n_workers; worker_idx++) {
        EndgameCtx *ctx = inj->workers[worker_idx].eg_ctx;
        if (ctx == NULL || !endgame_injecting(ctx)) {
          continue;
        }
        const int live = endgame_live_workers(ctx);
        total_live += live;
        if (live < least_live &&
            now - endgame_window_open_ns(ctx) >= min_age_ns) {
          least_live = live;
          least = ctx;
        }
      }
      if (least != NULL && total_live < inj->core_target) {
        // The injected worker draws its MoveGen cache slot on demand from
        // get_movegen() (per-pthread), so no slot bookkeeping is needed here.
        endgame_add_worker(least);
      }
    }
    ctime_nap(0.002);
  }
  return NULL;
}

// The effective PEG bag size for `game`: the real remaining bag tiles plus
// any opponent tiles unknown to the mover, i.e. raw_bag_size - opp_unknown,
// where opp_unknown = RACK_SIZE - opp_rack_size (the game bag holds the real
// remaining bag tiles plus any opponent tiles unknown to the mover; tiles
// explicitly on the opponent's rack are already known and not counted here).
// Shared by peg_solve and by callers (e.g. the pegonly "empty" positional
// value) that need to filter candidate moves by the same bag size peg_solve
// itself will use.
int peg_compute_bag_size(const Game *game) {
  const int mover_idx = game_get_player_on_turn_index(game);
  const int raw_bag_size = bag_get_letters(game_get_bag(game));
  const Rack *opp_rack_in_game =
      player_get_rack(game_get_player(game, 1 - mover_idx));
  const int opp_rack_size = (int)rack_get_total_letters(opp_rack_in_game);
  const int opp_unknown = RACK_SIZE - opp_rack_size;
  return raw_bag_size - opp_unknown;
}

// See peg.h for the contract.
int peg_move_bag_after(const Move *move, int bag_size) {
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return bag_size;
  }
  const int left = bag_size - move_get_tiles_played(move);
  return left > 0 ? left : 0;
}

// See peg.h for the contract.
bool peg_move_empties_bag(const Move *move, int bag_size) {
  return move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
         move_get_tiles_played(move) >= bag_size;
}

// ----- public entry --------------------------------------------------------

void peg_solve(const PegArgs *args, PegResult *out, ErrorStack *error_stack) {
  memset(out, 0, sizeof(*out));
  out->last_completed_stage = -1;
  peg_checkpoint_prepare(args);

  // Anchor the wall-clock deadline at the very start so the whole solve —
  // including KWG pruning and the initial greedy move generation, not just the
  // halving stages — counts against the time budget (0 = unbounded). Each
  // endgame leaf is also capped by this so a single deep solve cannot overrun.
  // (The result timer itself is started after validation below, so an
  // invalid-args early return leaves it stopped rather than running forever.)
  const double budget = args->time_budget_seconds;
  const int64_t deadline_ns =
      budget > 0.0 ? ctimer_monotonic_ns() + (int64_t)(budget * 1.0e9) : 0;
  const Game *game = args->game;
  const int mover_idx = game_get_player_on_turn_index(game);
  const int bag_size = peg_compute_bag_size(game);
  if (bag_size < PEG_MIN_BAG || bag_size > PEG_MAX_BAG) {
    // Above PEG_MAX_BAG is still solvable as long as every move under
    // consideration leaves at most PEG_MAX_BAG tiles in the bag: then each
    // candidate hands the opponent a pre-endgame the solver handles (or an
    // endgame), and the root enumeration's arrays are sized for a full rack.
    // A pass or exchange leaves the whole bag, and no tile placement can play
    // more than RACK_SIZE tiles, so this is only reachable for bag_size in
    // (PEG_MAX_BAG, RACK_SIZE + PEG_MAX_BAG], capped at RACK_SIZE below.
    //
    // It needs a caller-supplied only_moves set, or limit_tiles_left of at
    // most PEG_MAX_BAG, with every protected move leaving that few too: the
    // full root move list always includes a pass.
    bool bag_emptying_guaranteed =
        bag_size > PEG_MAX_BAG && bag_size <= RACK_SIZE &&
        (args->n_only_moves > 0 ||
         (args->limit_tiles_left && args->max_tiles_left <= PEG_MAX_BAG));
    for (int cand_idx = 0;
         bag_emptying_guaranteed && cand_idx < args->n_only_moves; cand_idx++) {
      if (peg_move_bag_after(args->only_moves[cand_idx], bag_size) >
          PEG_MAX_BAG) {
        bag_emptying_guaranteed = false;
      }
    }
    for (int protect_idx = 0;
         bag_emptying_guaranteed && protect_idx < args->n_protect_moves;
         protect_idx++) {
      if (peg_move_bag_after(args->protect_moves[protect_idx], bag_size) >
          PEG_MAX_BAG) {
        bag_emptying_guaranteed = false;
      }
    }
    if (!bag_emptying_guaranteed) {
      error_stack_push(
          error_stack, ERROR_STATUS_PEG_BAG_OUT_OF_RANGE,
          get_formatted_string(
              "PEG requires a bag of %d..%d tiles, but found %d (a larger "
              "bag, up to %d, is only allowed when every candidate move "
              "leaves at most %d tiles in it)",
              PEG_MIN_BAG, PEG_MAX_BAG, bag_size, RACK_SIZE, PEG_MAX_BAG));
      peg_poll_finish(args->poll,
                      false); // so a waiting poller's read loop terminates
      return;
    }
  }
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  uint8_t unseen[MAX_ALPHABET_SIZE];
  peg_compute_unseen(game, mover_idx, unseen);
  // Known opponent tiles (PegArgs.opp_rack_known) are not dealt: every
  // scenario's opponent holds them, beside what it is dealt from the rest.
  uint8_t known_opp[MAX_ALPHABET_SIZE] = {0};
  bool any_known_opp = false;
  if (args->opp_rack_known) {
    const Rack *opp_rack =
        player_get_rack(game_get_player(game, 1 - mover_idx));
    for (int ml = 0; ml < ld_size; ml++) {
      known_opp[ml] = (uint8_t)rack_get_letter(opp_rack, (MachineLetter)ml);
      if (known_opp[ml] > unseen[ml]) {
        known_opp[ml] = unseen[ml];
      }
      unseen[ml] -= known_opp[ml];
      any_known_opp = any_known_opp || known_opp[ml] > 0;
    }
  }

  // Validate an optional pinned bag ordering: it must be a non-NULL array of
  // exactly bag_size tiles, all drawable from the unseen pool (the opponent
  // gets the remainder).
  if (args->eval_bag_order_len > 0) {
    bool order_ok =
        args->eval_bag_order != NULL && args->eval_bag_order_len == bag_size;
    int order_counts[MAX_ALPHABET_SIZE] = {0};
    for (int i = 0; order_ok && i < args->eval_bag_order_len; i++) {
      const MachineLetter ml = args->eval_bag_order[i];
      if (ml >= MAX_ALPHABET_SIZE || ++order_counts[ml] > unseen[ml]) {
        order_ok = false;
      }
    }
    if (!order_ok) {
      error_stack_push(error_stack, ERROR_STATUS_PEG_INVALID_BAG_ORDER,
                       get_formatted_string(
                           "PEG eval_bag_order must list exactly the %d bag "
                           "tiles, all drawable from the unseen pool",
                           bag_size));
      peg_poll_finish(args->poll, false);
      return;
    }
  }

  // Validate an optional stage schedule. Each halving stage re-ranks a set, so
  // a top-K below 2 is meaningless; the array length must be a positive
  // num_stages.
  if (args->stage_top_k != NULL) {
    bool counts_ok = args->num_stages > 0;
    for (int i = 0; counts_ok && i < args->num_stages; i++) {
      if (args->stage_top_k[i] < 2) {
        counts_ok = false;
      }
    }
    if (!counts_ok) {
      error_stack_push(
          error_stack, ERROR_STATUS_PEG_INVALID_STAGE_COUNTS,
          get_formatted_string("PEG stage_top_k must list num_stages (> 0) "
                               "per-stage counts, each >= 2"));
      peg_poll_finish(args->poll, false);
      return;
    }
  }

  int n_threads = args->num_threads > 1 ? args->num_threads : 1;
  const double total_fraction = args->tt_fraction_of_mem > 0
                                    ? args->tt_fraction_of_mem
                                    : DEFAULT_TT_FRACTION;
  // Every worker shares one endgame table (see PegSharedEndgameTT), so a
  // bounded budget has to fit only that table: it no longer caps the workers.
  int max_tables = INT_MAX;
  if (search_memory_is_bounded()) {
    max_tables = peg_memory_max_tables(total_fraction);
    if (max_tables < 1) {
      error_stack_push(
          error_stack, ERROR_STATUS_PEG_MEMORY_BUDGET_TOO_SMALL,
          string_duplicate("PEG memory budget is too small for one table"));
      peg_poll_finish(args->poll, false);
      return;
    }
  }

  // Validation passed: start the result timer here, after the early-return
  // error checks above, so an invalid-args return leaves it stopped (is_running
  // stays false) rather than running forever. The deadline (above) still
  // anchors at the very start so the budget covers pruning/movegen too.
  ctimer_start(&out->timer);

  // Build the root pruned KWG once and install it on a prepared base game with
  // cross-sets generated a single time. The pre-cand board's playable words are
  // a superset of any post-cand position's, so this one pruned KWG is valid for
  // every (cand, scenario) leaf. Each leaf game_copy's this prepared base —
  // carrying the override KWG and pruned cross-sets — and play_move only
  // incrementally updates the cross-sets, so no leaf rebuilds a pruned KWG or
  // regenerates all cross-sets (the dominant per-leaf cost).
  KWG *pruned_kwg = NULL;
  if (peg_prunes_words(game)) {
    DictionaryWordList *word_list = dictionary_word_list_create();
    const KWG *full_kwg = player_get_kwg(game_get_player(game, mover_idx));
    peg_generate_possible_words(game, full_kwg, word_list);
    pruned_kwg = make_kwg_from_words_small(word_list, KWG_MAKER_OUTPUT_GADDAG,
                                           KWG_MAKER_MERGE_EXACT);
    dictionary_word_list_destroy(word_list);
  }
  Game *prepared_base = game_duplicate(game);
  game_set_endgame_solving_mode(prepared_base);
  game_set_backup_mode(prepared_base, BACKUP_MODE_OFF);
  if (pruned_kwg != NULL) {
    game_set_override_kwgs(prepared_base, pruned_kwg, NULL,
                           DUAL_LEXICON_MODE_IGNORANT);
  }
  game_gen_all_cross_sets(prepared_base);

  // Per-stage halving counts: the caller override, else the built-in default
  // schedule. The number of stages is just the schedule length — there is no
  // fixed cap, so a caller may pass a longer stage_top_k.
  const int default_num_stages = (int)(sizeof(PEG_DEFAULT_HALVING_COUNTS) /
                                       sizeof(PEG_DEFAULT_HALVING_COUNTS[0]));
  const int *counts = (args->stage_top_k && args->num_stages > 0)
                          ? args->stage_top_k
                          : PEG_DEFAULT_HALVING_COUNTS;
  int num_stages = (args->stage_top_k && args->num_stages > 0)
                       ? args->num_stages
                       : default_num_stages;
  if (args->max_stage > 0 && args->max_stage < num_stages) {
    num_stages = args->max_stage;
  }
  if (args->eval_bag_order_len > 0) {
    // A pinned single scenario has nothing for the halving stages to re-rank.
    num_stages = 0;
  }
  if (args->greedy_seed_only) {
    // Greedy seed only: rank the full field by the stage-0 greedy-playout
    // win%, skipping the halving stages' exact endgame refinement. Bounded and
    // deterministic (full enumeration, deterministic playout, no deep solves).
    num_stages = 0;
  }

  // Exhaustive mode: a single uncapped stage (-pegtopk all / 0). With no
  // narrowing the per-stage fidelity ramp is pointless — re-evaluating the
  // whole field at 2,3,4,... ply just to keep all of it every time — so
  // collapse to one deep stage that runs the full-depth endgame over the entire
  // field with full scenario enumeration. The greedy stage 0 still seeds the
  // ranking.
  const bool exhaustive = num_stages == 1 && counts[0] == INT_MAX;
  // Its endgame depth: PegArgs.exhaustive_plies, or to the end of the game.
  const int exhaustive_plies =
      args->exhaustive_plies > 0 &&
              args->exhaustive_plies < PEG_EXHAUSTIVE_PLIES
          ? args->exhaustive_plies
          : PEG_EXHAUSTIVE_PLIES;

  // Scenario sampling: opt-in via scenario_stride > 1, and only for bag >= 3
  // (the bag <= 2 scenario space is too small to sample without destroying
  // coverage). Default (<= 1) is full enumeration — exact, just slower.
  // Applied to every stage including the greedy seed (stage 0), whose full
  // bag-3+ enumeration over thousands of candidates is the dominant cost.
  // Exhaustive mode always enumerates fully (no stride), regardless of
  // -pegstride.
  const int scenario_stride =
      (!exhaustive && bag_size >= 3 && args->scenario_stride > 1)
          ? args->scenario_stride
          : 1;

  // Per-worker scratch. One extra slot for the main thread when it helps the
  // pool drain the queue (helper index == num_workers).
  PegPool *pool = n_threads > 1 ? peg_pool_create(n_threads, 0) : NULL;
  if (pool) {
    // Leaf endgames at the deep stages legitimately run for minutes; the
    // no-progress watchdog would false-positive on them (work proceeds, but a
    // single job doesn't complete within the window). Disable it for peg.
    peg_pool_set_stuck_timeout_seconds(pool, 0);
  }
  const int n_scratch = pool ? n_threads + 1 : 1;
  // Endgame TT size: first what each worker's own table would be, as when
  // every worker had one. Shallow PEG endgames need little, and the total
  // across workers stays well under the 50%-RAM ceiling. A bounded budget
  // divides among at most the tables it can hold at the minimum size.
  double tt_fraction = 0.25 / (double)n_scratch;
  if (tt_fraction > 0.05) {
    tt_fraction = 0.05;
  }
  int n_tables = n_scratch;
  if (search_memory_is_bounded()) {
    if (n_tables > max_tables) {
      n_tables = max_tables;
    }
    const bool fits =
        peg_memory_worker_fraction(total_fraction, n_tables, &tt_fraction);
    assert(fits); // max_tables tables fit at the minimum size by construction.
    (void)fits;
  }
  // Then one table shared by every worker, no larger than those tables
  // combined. A single worker gets exactly its own table.
  PegSharedEndgameTT shared_eg_tt;
  cpthread_mutex_init(&shared_eg_tt.mutex);
  shared_eg_tt.tt = args->shared_endgame_tt;
  shared_eg_tt.fraction = peg_memory_shared_fraction(tt_fraction, n_tables);
  shared_eg_tt.zero_threads = n_threads;
  shared_eg_tt.owned = args->shared_endgame_tt == NULL;
  // One prune cache shared by every worker (cross-worker board reuse).
  PegPruneCache *prune_cache = peg_prune_cache_create();
  PegWorker *workers = malloc_or_die((size_t)n_scratch * sizeof(PegWorker));
  for (int worker_idx = 0; worker_idx < n_scratch; worker_idx++) {
    workers[worker_idx].playout_ml = move_list_create(1);
    workers[worker_idx].eg_results = endgame_results_create();
    // Pre-created so the injection monitor reads a stable, non-NULL ctx pointer
    // (no race with lazy creation inside endgame_solve_inline).
    workers[worker_idx].eg_ctx = endgame_ctx_create();
    workers[worker_idx].template_game = NULL;
    workers[worker_idx].scratch_game = NULL;
    workers[worker_idx].eg_tt = args->shared_endgame_tt;
    workers[worker_idx].shared_eg_tt = &shared_eg_tt;
    workers[worker_idx].prune_cache = prune_cache;
    workers[worker_idx].disable_best_reply_shortcuts =
        args->disable_best_reply_shortcuts;
    workers[worker_idx].first_win = args->first_win;
    workers[worker_idx].reply_cache = calloc_or_die(1, sizeof(PegReplyCache));
    // Nested-PEG lookahead config + free-list scratch.
    workers[worker_idx].thread_control = args->thread_control;
    // Inner-peg scenarios fan out across the shared pool (NULL when single-
    // threaded => inline). Safe because inner scenario jobs are submitted
    // re-entrantly: a worker that blocks on an inner batch only help-drains
    // other re-entrant items, never an outer job that would clobber the
    // per-worker scratch it is mid-evaluation on. This keeps the cores busy on
    // positions with few outer candidates, where outer parallelism alone would
    // leave most cores idle.
    workers[worker_idx].nest_pool = pool;
    workers[worker_idx].nest_workers = workers;
    workers[worker_idx].nest_worker_idx = worker_idx;
    workers[worker_idx].nested_enabled = args->nested_enabled;
    workers[worker_idx].nested_cand_cap = args->nested_cand_cap;
    workers[worker_idx].nested_cand_caps = args->nested_cand_caps;
    workers[worker_idx].nested_n_cand_caps = args->nested_n_cand_caps;
    workers[worker_idx].nested_stride = args->nested_stride;
    workers[worker_idx].nested_strides_by_bag = args->nested_strides_by_bag;
    workers[worker_idx].nested_n_strides_by_bag = args->nested_n_strides_by_bag;
    workers[worker_idx].nested_emptier_ply_cap = args->nested_emptier_ply_cap;
    workers[worker_idx].nested_max_depth = args->nested_max_depth;
    workers[worker_idx].nested_score_actual = args->nested_score_actual;
    workers[worker_idx].nested_pessimistic =
        args->nested_pessimistic && args->opp_model == PEG_OPP_PESSIMISTIC;
    workers[worker_idx].inner_top_k = args->inner_top_k;
    workers[worker_idx].root_mover_idx = mover_idx;
    workers[worker_idx].known_opp = any_known_opp ? known_opp : NULL;
    workers[worker_idx].nest_free = NULL;
    workers[worker_idx].nest_all = NULL;
  }

  // Injection monitor: lends idle cores to in-flight leaf endgames. Only
  // meaningful with a pool; the deep stages have few candidates, so their long
  // endgames would otherwise leave most cores idle.
  PegInjector injector;
  cpthread_t injector_thread;
  bool injector_running = false;
  if (pool) {
    injector.pool = pool;
    injector.workers = workers;
    injector.n_workers = n_scratch;
    injector.core_target = n_threads;
    atomic_init(&injector.stop, 0);
    cpthread_create(&injector_thread, peg_injector_main, &injector);
    injector_running = true;
  }
  peg_poll_attach_workers(args->poll, workers, n_scratch);

  // Candidate set: either the caller-supplied "only solve" list (used as-is),
  // or the generated, equity-sorted move list, cut to the moves that leave at
  // most max_tiles_left tiles when limit_tiles_left is set. Protected moves
  // join it wherever it left them out. Stage 0 re-ranks by win% regardless, so
  // the input order does not matter.
  MoveList *cand_ml = NULL;
  int n_listed = args->n_only_moves;
  if (args->n_only_moves == 0) {
    cand_ml = move_list_create(PEG_CAND_LIST_CAP);
    const MoveGenArgs gen_args = {
        .game = game,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .move_list = cand_ml,
        .tiles_played_bv = NULL,
        .initial_tiles_bv = 0,
    };
    generate_moves(&gen_args);
    n_listed = move_list_get_count(cand_ml);
  }
  const Move **cand_moves = malloc_or_die(
      (size_t)(n_listed + args->n_protect_moves + 1) * sizeof(Move *));
  int n_cands = 0;
  for (int listed_idx = 0; listed_idx < n_listed; listed_idx++) {
    const Move *move = args->n_only_moves > 0
                           ? args->only_moves[listed_idx]
                           : move_list_get_move(cand_ml, listed_idx);
    if (args->n_only_moves == 0 && args->limit_tiles_left &&
        peg_move_bag_after(move, bag_size) > args->max_tiles_left) {
      continue;
    }
    cand_moves[n_cands++] = move;
  }
  for (int protect_idx = 0; protect_idx < args->n_protect_moves;
       protect_idx++) {
    const Move *protect = args->protect_moves[protect_idx];
    bool present = false;
    for (int cand_idx = 0; !present && cand_idx < n_cands; cand_idx++) {
      present = compare_moves_without_equity(cand_moves[cand_idx], protect,
                                             true) == -1;
    }
    if (!present) {
      cand_moves[n_cands++] = protect;
    }
  }
  if (n_cands == 0 && args->n_only_moves == 0 && args->limit_tiles_left) {
    error_stack_push(
        error_stack, ERROR_STATUS_PEG_EMPTY_NO_MOVES,
        args->max_tiles_left == 0
            ? get_formatted_string("no generated move empties the %d-tile bag",
                                   bag_size)
            : get_formatted_string(
                  "no generated move leaves at most %d of the %d-tile bag",
                  args->max_tiles_left, bag_size));
  }

  if (n_cands > 0) {
    // Protected ("never prune") moves: each stage carries these past its
    // top-K cut so they always end up in the final results.
    const int n_protect = args->n_protect_moves;
    const Move *const *protect_moves = args->protect_moves;

    PegRankedCand *ranked =
        malloc_or_die((size_t)n_cands * sizeof(PegRankedCand));
    const Move **moves = malloc_or_die((size_t)n_cands * sizeof(Move *));

    // Progress callbacks (NULL members are simply not fired). stage_idx is
    // updated before each stage's dispatch and read by the workers.
    PegProgress progress = {
        .on_cand_done = args->on_cand_done,
        .on_scenario_done = args->on_scenario_done,
        .user_data = args->user_data,
        .stage_idx = 0,
    };

    // Stage 0: greedy evaluation of every candidate.
    for (int cand_idx = 0; cand_idx < n_cands; cand_idx++) {
      moves[cand_idx] = cand_moves[cand_idx];
    }
    // See peg_force_protected_to_front: give protected candidates the best
    // chance of finishing within the time budget by having a free worker
    // pick them up first, rather than leaving their queue position to
    // whatever movegen's equity sort happened to produce.
    peg_force_protected_move_ptrs_to_front(moves, n_cands, protect_moves,
                                           n_protect);
    peg_poll_begin_stage(args->poll, /*stage=*/0, /*fidelity_plies=*/0,
                         n_cands);
    if (args->on_stage_start != NULL) {
      args->on_stage_start(/*stage_idx=*/0, n_cands, /*inner_d=*/0,
                           /*emptier_plies=*/0, args->user_data);
    }
    // The seed uses the cheap greedy playout, whatever the opponent model, in
    // a complete solve (the exact early cutoff), where it only orders the
    // candidates, and wherever nesting defines the opponent: the pessimistic
    // playout tries every reply on every draw, so under a time limit it could
    // spend the whole budget here, for a model the later stages replace.
    const bool seed_orders_only =
        (args->early_cutoff && args->first_win && exhaustive) ||
        (args->nested_enabled && args->nested_pessimistic);
    peg_eval_candidates(
        pool, workers, prepared_base, mover_idx, unseen, ld_size, bag_size,
        moves, n_cands, seed_orders_only ? PEG_OPP_RATIONAL : args->opp_model,
        args->inner_top_k, /*fidelity_plies=*/0, scenario_stride, deadline_ns,
        args->thread_control, args->poll, &progress, args->eval_bag_order,
        args->eval_bag_order_len, ranked, args->checkpoint);
    stable_sort(ranked, (size_t)n_cands, sizeof(PegRankedCand), peg_rank_cmp);
    // Drop budget-skipped sentinels (win_pct < 0) from the carried set: they
    // sort to the bottom, so the real candidates occupy [0, n_real).
    int n_real = 0;
    while (n_real < n_cands && ranked[n_real].win_pct >= 0.0) {
      n_real++;
    }
    if (n_real == 0) {
      n_real = n_cands; // nothing finished in budget; keep what we have
    }
    const int num_kept_after_stage0 = n_real < counts[0] ? n_real : counts[0];
    // Survivors carried forward = top-K unprotected plus every protected move.
    // Tracked as a shrinking live_count so protected moves never leave a stale
    // copy in the unscanned tail.
    int live_count = peg_select_survivors(ranked, n_real, num_kept_after_stage0,
                                          protect_moves, n_protect);
    peg_publish(out, ranked, live_count, /*stage=*/0);
    peg_poll_replace(args->poll, ranked, live_count, /*stage=*/0,
                     /*fidelity_plies=*/0, live_count);

    // Graded ranking: as the cascade narrows, each candidate is recorded with
    // the deepest fidelity it reached. Drops at each stage are captured here
    // (shallowest tier first); the final survivors are captured after the loop.
    // The kept-after-stage-0 field is the upper bound on entries. prev_fidelity
    // tracks the ply count the current `ranked` set was last scored at (0 =
    // greedy, so the stage-1 set carries no graded entry until it is
    // re-scored).
    const int graded_cap = live_count;
    PegRankedCand *graded =
        malloc_or_die((size_t)graded_cap * sizeof(PegRankedCand));
    int *graded_fidelity = malloc_or_die((size_t)graded_cap * sizeof(int));
    int n_graded = 0;
    int prev_fidelity = 0;

    // Per-candidate per-ordering outcome store (only when capture requested).
    // Keyed by move; deeper stages overwrite shallower captures so each move
    // ends up at its deepest fidelity. Handed to out->cand_outcomes at the end.
    PegCandOutcomes *oc_store = NULL;
    int oc_n = 0;
    int oc_cap = 0;

    // Halving stages. Stage s re-evaluates the surviving top counts[s-1] (plus
    // protected stragglers) at one more ply of fidelity, then re-ranks; a stage
    // needs >= 2 candidates to be meaningful, and is skipped once the budget is
    // spent. In exhaustive mode the lone stage runs at exhaustive_plies
    // instead of the stage_idx+1 ramp (see `exhaustive` above).
    for (int stage_idx = 1; stage_idx <= num_stages; stage_idx++) {
      const int stage_fidelity = exhaustive ? exhaustive_plies : stage_idx + 1;
      const bool last_stage = stage_idx == num_stages;
      // A stage that keeps every live candidate decides nothing; with
      // skip_noncutting_stages the next one that cuts (or the last) runs in
      // its place, at its own depth.
      if (args->skip_noncutting_stages && !exhaustive && !last_stage &&
          counts[stage_idx - 1] >= live_count) {
        continue;
      }
      // first_win_intermediate: first-win before the last stage, a full
      // window at it.
      const bool stage_first_win =
          args->first_win ||
          (args->first_win_intermediate && !exhaustive && !last_stage);
      for (int worker_idx = 0; worker_idx < n_scratch; worker_idx++) {
        workers[worker_idx].first_win = stage_first_win;
      }
      int keep = live_count < counts[stage_idx - 1] ? live_count
                                                    : counts[stage_idx - 1];
      // Under first-win the previous stage's ranking knows no spreads, so the
      // candidates tied at the cut line all go on rather than an arbitrary
      // few of them.
      while (keep > 0 && keep < live_count && ranked[keep - 1].spread_unknown &&
             peg_cand_compare(&ranked[keep - 1], &ranked[keep]) == 0) {
        keep++;
      }
      const int eval_count = peg_select_survivors(ranked, live_count, keep,
                                                  protect_moves, n_protect);
      // A halving stage re-ranks a set, so it needs two candidates. The
      // exhaustive stage solves each in full, which is worth doing for one.
      const int min_stage_count = exhaustive ? 1 : 2;
      if (eval_count < min_stage_count) {
        break;
      }
      // The sequential live-mode loop below scores ranked[0..eval_count) in
      // array order and stops as soon as the deadline passes; a protected
      // straggler peg_select_survivors appended after the top-K cut would
      // otherwise be scored last, making it the first thing a tight time
      // budget drops. Move it to the front so it's scored before that can
      // happen.
      peg_force_protected_to_front(ranked, eval_count, protect_moves,
                                   n_protect);
      if (deadline_ns != 0 && ctimer_monotonic_ns() >= deadline_ns) {
        break;
      }
      if (thread_control_search_checkpoint(args->thread_control) ==
          THREAD_CONTROL_STATUS_USER_INTERRUPT) {
        break;
      }
      // Candidates that don't advance past this stage reached only
      // prev_fidelity (their last scoring). Record them before they fall out of
      // `ranked`. Saved n_graded so the capture can be undone if the stage ends
      // up contributing nothing (fewer than 2 candidates finished).
      const int n_graded_before_stage = n_graded;
      peg_graded_append(graded, graded_fidelity, &n_graded, ranked, eval_count,
                        live_count, prev_fidelity);
      // Save entries as baseline BEFORE begin_stage updates fidelity_plies, so
      // baseline_fidelity captures the previous stage's depth (not the new
      // one).
      peg_poll_clear_entries(args->poll);
      peg_poll_begin_stage(args->poll, stage_idx, stage_fidelity, eval_count);
      progress.stage_idx = stage_idx;
      if (args->on_stage_start != NULL) {
        args->on_stage_start(stage_idx, eval_count, /*inner_d=*/0,
                             /*emptier_plies=*/stage_fidelity, args->user_data);
      }
      for (int i = 0; i < eval_count; i++) {
        moves[i] = &ranked[i].move;
      }
      // Evaluate into a separate buffer: moves[] aliases ranked[].move, so the
      // worker must not overwrite ranked[i] while later moves still point into
      // it. done_count tracks how many finished — the whole stage normally, but
      // fewer if the budget/interrupt cuts it off mid-stage (live mode only).
      PegRankedCand *restaged =
          malloc_or_die((size_t)eval_count * sizeof(PegRankedCand));
      int done_count = eval_count;
      // Keep this stage's captures separate until the stage is accepted. A
      // stage with fewer than two completed candidates is rolled back to the
      // preceding fidelity, so publishing its captures early would pair deeper
      // outcomes with the restored shallower ranking.
      PegCandOutcomes *stage_outcomes =
          args->include_per_scenario
              ? calloc_or_die((size_t)eval_count, sizeof(PegCandOutcomes))
              : NULL;
      // The exact early cutoff (PegArgs.early_cutoff) needs the candidates
      // scored separately, against the best finished one's score.
      const bool cutoff_on =
          (args->early_cutoff && args->first_win && exhaustive) ||
          (args->final_stage_cutoff && !exhaustive && last_stage);
      if (args->poll != NULL || args->checkpoint != NULL || cutoff_on) {
        // Live mode: score candidates separately so each completion updates
        // the pollable leaderboard (for `sta`/`shpeg`) and so we can record
        // each candidate's own wall-clock time (see PegLiveStage for how the
        // pool is spread over them). Greedy stage 0 is not routed here.
        // Publish the whole stage's moves so a live renderer can fix the move
        // column width up front (moves[] aliases ranked[].move for [0,eval)).
        peg_poll_set_stage_moves(args->poll, moves, eval_count);
        PegProgress inner = progress;
        inner.on_cand_done = NULL; // fired once per candidate, when published
        PegLiveStage live = {
            .args = args,
            .workers = workers,
            .prepared_base = prepared_base,
            .mover_idx = mover_idx,
            .unseen = unseen,
            .ld_size = ld_size,
            .ld = ld,
            .bag_size = bag_size,
            .injection_cap = peg_pool_num_workers(pool),
            .fidelity = stage_fidelity,
            .scenario_stride = scenario_stride,
            .deadline_ns = deadline_ns,
            .protect_moves = protect_moves,
            .n_protect = n_protect,
            .cutoff_on = cutoff_on,
            .inner = &inner,
            .stage_idx = stage_idx,
            .moves = moves,
            .ranked = ranked,
            .restaged = restaged,
            .stage_outcomes = stage_outcomes,
            .complete = calloc_or_die((size_t)eval_count, sizeof(bool)),
        };
        atomic_init(&live.best_score, -1);
        cpthread_mutex_init(&live.mutex);
        // With a cutoff and a pool, the first candidate alone sets the bar
        // and the rest are solved at once (see PegLiveStage).
        const bool at_once = cutoff_on && pool != NULL;
        const int n_alone = at_once ? 1 : eval_count;
        done_count = 0;
        for (int cand_idx = 0; cand_idx < n_alone; cand_idx++) {
          // Stop deepening once the budget or a user interrupt hits. Checked
          // before starting a candidate so done_count counts only candidates
          // evaluated within budget; the finished ones (if >= 2) still form a
          // usable, if partial, tier at this stage's depth.
          if (peg_should_stop(deadline_ns, args->thread_control) ||
              !peg_live_solve_cand(&live, cand_idx, pool,
                                   peg_pool_num_workers(pool),
                                   /*alone=*/true)) {
            break;
          }
          done_count = cand_idx + 1;
        }
        if (at_once && done_count == n_alone && n_alone < eval_count) {
          done_count =
              peg_live_solve_at_once(&live, pool, ranked, n_alone, eval_count);
        }
        free(live.complete);
      } else {
        // Fast path (no live poll, e.g. benchmarks): scenario-level parallelism
        // across all candidates at once — a halving stage has few candidates,
        // so pooling their scenarios keeps all cores busy with a single
        // barrier.
        peg_eval_candidates_scenario(
            pool, workers, prepared_base, mover_idx, unseen, ld_size, ld,
            bag_size, moves, eval_count, args->opp_model, args->inner_top_k,
            stage_fidelity, scenario_stride, deadline_ns, args->thread_control,
            &progress, args->poll, restaged, stage_outcomes, /*cutoff=*/NULL,
            /*scenario_poll=*/NULL, /*cand_poll=*/NULL,
            peg_pool_num_workers(pool), peg_pool_num_workers(pool));
        // A deadline can cut the stage mid-flight: a candidate whose scenario
        // jobs bailed has a short weight_sum, so keep only the fully-scored
        // ones (as the live path does) instead of ranking partial scores.
        // Stable-partition the complete candidates — with their carried-in
        // (prev-fidelity) records and outcome captures — to the front so the
        // shared partial-stage handling below sees the right done_count. The
        // complete weight_sum is ranked[0].weight_sum: carried candidates were
        // fully scored at the previous stage and weight_sum (= perm(unseen,
        // bag_size)) is constant across plays.
        const int64_t full_weight = ranked[0].weight_sum;
        PegRankedCand *part_new =
            malloc_or_die((size_t)eval_count * sizeof(PegRankedCand));
        PegRankedCand *part_old =
            malloc_or_die((size_t)eval_count * sizeof(PegRankedCand));
        PegCandOutcomes *part_oc =
            stage_outcomes != NULL
                ? malloc_or_die((size_t)eval_count * sizeof(PegCandOutcomes))
                : NULL;
        int placed = 0;
        for (int pass = 0; pass < 2; pass++) {
          const bool want_complete = pass == 0;
          for (int cand_idx = 0; cand_idx < eval_count; cand_idx++) {
            const bool complete = restaged[cand_idx].weight_sum >= full_weight;
            if (complete != want_complete) {
              continue;
            }
            part_new[placed] = restaged[cand_idx];
            part_old[placed] = ranked[cand_idx];
            if (part_oc != NULL) {
              part_oc[placed] = stage_outcomes[cand_idx];
            }
            placed++;
          }
          if (pass == 0) {
            done_count = placed;
          }
        }
        memcpy(restaged, part_new, (size_t)eval_count * sizeof(PegRankedCand));
        memcpy(ranked, part_old, (size_t)eval_count * sizeof(PegRankedCand));
        free(part_new);
        free(part_old);
        if (stage_outcomes != NULL) {
          memcpy(stage_outcomes, part_oc,
                 (size_t)eval_count * sizeof(PegCandOutcomes));
          free(part_oc);
          // Discard the cut candidates' partial captures. The complete prefix
          // remains stage-local until the shared acceptance check below.
          for (int cand_idx = done_count; cand_idx < eval_count; cand_idx++) {
            peg_scenario_rows_destroy(stage_outcomes[cand_idx].rows,
                                      stage_outcomes[cand_idx].n_rows);
          }
        }
      }
      // Too few finished: nothing to compare at this depth, so discard the
      // partial work and undo this stage's drop capture, leaving every
      // candidate at its previous depth (as if the stage never ran).
      if (done_count < min_stage_count) {
        n_graded = n_graded_before_stage;
        free(restaged);
        // Frees memory this stage still owns: stage_outcomes[0, done_count)
        // are completed captures that were never upserted, since the store
        // only takes them on the accept path below. The cut tail
        // [done_count, eval_count) was already freed by whichever evaluation
        // path cut it -- the live path frees the deadline-cut candidate on its
        // break, the batch path frees the tail after stable-partitioning. With
        // accept upserting exactly [0, done_count), every rows allocation is
        // freed exactly once across all three exits.
        peg_cand_outcomes_destroy_array(stage_outcomes, done_count);
        // This stage cleared the live poll at its start but contributed
        // nothing, so restore the previous stage's ranking (still in `ranked`)
        // instead of leaving the final snapshot empty.
        peg_poll_replace(args->poll, ranked, live_count, stage_idx - 1,
                         prev_fidelity, live_count);
        break;
      }
      if (stage_outcomes != NULL) {
        for (int cand_idx = 0; cand_idx < done_count; cand_idx++) {
          peg_outcomes_store_upsert(&oc_store, &oc_n, &oc_cap,
                                    &stage_outcomes[cand_idx]);
        }
        // Ownership of each rows allocation moved into oc_store.
        free(stage_outcomes);
      }
      stable_sort(restaged, (size_t)done_count, sizeof(PegRankedCand),
                  peg_rank_cmp);
      // Partial stage: candidates [done_count, eval_count) were selected but
      // the cutoff arrived before they were scored at this depth, so record
      // them at the previous depth before narrowing to the finished set.
      if (done_count < eval_count) {
        peg_graded_append(graded, graded_fidelity, &n_graded, ranked,
                          done_count, eval_count, prev_fidelity);
      }
      memcpy(ranked, restaged, (size_t)done_count * sizeof(PegRankedCand));
      free(restaged);
      live_count = done_count;
      prev_fidelity = stage_fidelity; // `ranked` is now scored at this depth
      peg_publish(out, ranked, done_count, stage_idx);
      out->last_stage_partial = done_count < eval_count;
      peg_poll_replace(args->poll, ranked, done_count, stage_idx,
                       stage_fidelity, done_count);
      // Surface this stage's per-candidate outcomes to the live poll so a
      // status query can render the per-multiset W/L/T column (refreshed each
      // stage).
      peg_poll_set_outcomes(args->poll, oc_store, oc_n);
      // A partial stage means the budget/interrupt already hit, so stop the
      // cascade rather than starting another (deeper, costlier) stage.
      if (done_count < eval_count) {
        break;
      }
    }

    // The deepest survivors form the top tier. Publish the graded list only if
    // a halving stage actually scored them (prev_fidelity > 0); otherwise there
    // is nothing past greedy to grade and the flat top_cands view is used.
    if (prev_fidelity > 0) {
      peg_graded_append(graded, graded_fidelity, &n_graded, ranked, 0,
                        live_count, prev_fidelity);
      out->graded_cands = graded;
      out->graded_fidelity = graded_fidelity;
      out->n_graded = n_graded;
    } else {
      free(graded);
      free(graded_fidelity);
    }

    // Per-candidate per-ordering outcomes, captured inline during the halving
    // stages above (no re-evaluation). Hand the store to the result.
    out->cand_outcomes = oc_store;
    out->n_cand_outcomes = oc_n;

    // Per-scenario detail for the published best cand (the "outcomes (best)"
    // line). Prefer the rows already captured inline at the best cand's deepest
    // fidelity. Only when the best cand was never captured inline — a
    // greedy-only or pinned result, where no halving stage ran — fall back to a
    // dedicated single-cand capture pass. That fallback runs at greedy
    // fidelity: one candidate at greedy depth, not a deep re-evaluation, so it
    // is cheap.
    if (args->include_per_scenario && out->n_top_cands > 0) {
      const PegCandOutcomes *best_oc = NULL;
      for (int store_idx = 0; store_idx < oc_n; store_idx++) {
        if (peg_move_eq(&oc_store[store_idx].move, &out->top_cands[0].move)) {
          best_oc = &oc_store[store_idx];
          break;
        }
      }
      if (best_oc != NULL && best_oc->n_rows > 0) {
        out->per_scenario =
            peg_scenario_rows_copy(best_oc->rows, best_oc->n_rows);
        out->n_per_scenario = best_oc->n_rows;
      } else {
        int capture_fidelity = 0;
        if (out->last_completed_stage > 0) {
          capture_fidelity =
              exhaustive ? exhaustive_plies : out->last_completed_stage + 1;
        }
        PegScenarioCapture capture = {0};
        capture.ld = ld;
        PegEvalCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.mover_idx = mover_idx;
        ctx.unseen = unseen;
        ctx.ld_size = ld_size;
        // At greedy fidelity, the seed's own playout: these rows then are the
        // score they detail, and cheap, as the pessimistic one is not.
        ctx.opp_model = capture_fidelity == 0 && seed_orders_only
                            ? PEG_OPP_RATIONAL
                            : args->opp_model;
        ctx.inner_top_k = args->inner_top_k;
        ctx.fidelity_plies = capture_fidelity;
        ctx.scenario_stride = scenario_stride;
        ctx.thread_control = args->thread_control;
        ctx.worker = &workers[0];
        ctx.capture = &capture;
        peg_build_template(ctx.worker, prepared_base, &out->top_cands[0].move);
        ctx.template_src = ctx.worker->template_game;
        ctx.reply_cache = ctx.worker->reply_cache;
        peg_reply_cache_reset(ctx.reply_cache);
        const int tiles_played = move_get_tiles_played(&out->top_cands[0].move);
        ctx.k_drawn = tiles_played < bag_size ? tiles_played : bag_size;
        const int n_bag_remaining = bag_size - ctx.k_drawn;
        MachineLetter mover_drawn[PEG_SCENARIO_ARRAY_CAP + 1];
        MachineLetter bag_remaining[PEG_SCENARIO_ARRAY_CAP + 1];
        peg_enum_splits(&ctx, /*ml=*/0, ctx.k_drawn, n_bag_remaining,
                        /*weight=*/1, mover_drawn, 0, bag_remaining, 0);
        out->per_scenario = capture.rows;
        out->n_per_scenario = capture.count;
      }
    }

    free(moves);
    free(ranked);
  }

  out->can_resume = args->checkpoint && args->checkpoint->count > 0 &&
                    peg_should_stop(deadline_ns, args->thread_control);
  // Mark the live poll done so a poller's read loop can terminate.
  peg_poll_finish(args->poll, out->can_resume);

  if (args->poll) {
    PegPollSnapshot poll_snap;
    peg_poll_read(args->poll, &poll_snap);
    out->n_stage_history = poll_snap.n_stage_history;
    memcpy(out->stage_history, poll_snap.stage_history,
           (size_t)poll_snap.n_stage_history * sizeof(PegStageSnapshot));
  }

  // A status read must not reach the workers once they are freed.
  peg_poll_attach_workers(args->poll, NULL, 0);
  // Stop the injection monitor before tearing down the workers it observes.
  if (injector_running) {
    atomic_store(&injector.stop, 1);
    cpthread_join(injector_thread);
  }

  free(cand_moves);
  if (cand_ml) {
    move_list_destroy(cand_ml);
  }
  for (int worker_idx = 0; worker_idx < n_scratch; worker_idx++) {
    move_list_destroy(workers[worker_idx].playout_ml);
    free(workers[worker_idx].reply_cache);
    endgame_ctx_destroy(workers[worker_idx].eg_ctx);
    endgame_results_destroy(workers[worker_idx].eg_results);
    if (workers[worker_idx].template_game) {
      game_destroy(workers[worker_idx].template_game);
    }
    if (workers[worker_idx].scratch_game) {
      game_destroy(workers[worker_idx].scratch_game);
    }
    for (PegNestFrame *frame = workers[worker_idx].nest_all; frame != NULL;) {
      PegNestFrame *next = frame->all_next;
      if (frame->game) {
        game_destroy(frame->game);
      }
      if (frame->moves) {
        move_list_destroy(frame->moves);
      }
      free(frame);
      frame = next;
    }
  }
  free(workers);
  if (shared_eg_tt.owned) {
    transposition_table_destroy(shared_eg_tt.tt);
  }
  // Safe now that every worker's scratch game (which referenced cached KWGs via
  // its override pointer) has been destroyed.
  peg_prune_cache_destroy(prune_cache);
  if (pool) {
    peg_pool_destroy(pool);
  }
  // Destroy the games that reference the pruned KWG (override pointer) before
  // freeing the KWG itself.
  game_destroy(prepared_base);
  kwg_destroy(pruned_kwg);
  ctimer_stop(&out->timer);
}

void peg_result_destroy(PegResult *r) {
  if (!r) {
    return;
  }
  free(r->top_cands);
  r->top_cands = NULL;
  r->n_top_cands = 0;
  free(r->graded_cands);
  r->graded_cands = NULL;
  free(r->graded_fidelity);
  r->graded_fidelity = NULL;
  r->n_graded = 0;
  peg_scenario_rows_destroy(r->per_scenario, r->n_per_scenario);
  r->per_scenario = NULL;
  r->n_per_scenario = 0;
  if (r->cand_outcomes != NULL) {
    for (int oc_idx = 0; oc_idx < r->n_cand_outcomes; oc_idx++) {
      peg_scenario_rows_destroy(r->cand_outcomes[oc_idx].rows,
                                r->cand_outcomes[oc_idx].n_rows);
    }
    free(r->cand_outcomes);
    r->cand_outcomes = NULL;
    r->n_cand_outcomes = 0;
  }
}
