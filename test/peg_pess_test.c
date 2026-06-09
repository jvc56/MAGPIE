#include "peg_pess_test.h"

#include "../src/compat/cpthread.h"
#include "../src/compat/ctime.h"
#include "../src/compat/memory_info.h"
#include "../src/def/board_defs.h"
#include "../src/def/cpthread_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/move_undo.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/transposition_table.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/peg_pool.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// clang-tidy (cert-err34-c) flags atoi/atof for not reporting conversion
// errors; these wrappers keep call sites terse while using strtol/strtod.
static int passpeg_str_to_int(const char *str) {
  return (int)strtol(str, NULL, 10);
}
static double passpeg_str_to_double(const char *str) {
  return strtod(str, NULL);
}

// Recursive enumeration of ordered draws of `remaining` tiles from
// `tile_counts` (a multiset over `num_types`). For each TYPE-prefix the cb
// is called once with a multiplicity = product over steps of count-before-
// decrement, matching the number of distinguishable-tile orderings that map
// to this type sequence.
static void peg_pess_enum_ordered_draws(
    const MachineLetter *tile_types, int *tile_counts, int num_types,
    int remaining, MachineLetter *draw_buf, int draw_buf_len,
    int64_t multiplicity,
    void (*cb)(const MachineLetter *draw, int n, int64_t weight, void *user),
    void *user) {
  if (remaining == 0) {
    cb(draw_buf, draw_buf_len, multiplicity, user);
    return;
  }
  for (int ti = 0; ti < num_types; ti++) {
    if (tile_counts[ti] <= 0) {
      continue;
    }
    const int count_before = tile_counts[ti];
    tile_counts[ti]--;
    draw_buf[draw_buf_len] = tile_types[ti];
    peg_pess_enum_ordered_draws(tile_types, tile_counts, num_types,
                                remaining - 1, draw_buf, draw_buf_len + 1,
                                multiplicity * count_before, cb, user);
    tile_counts[ti]++;
  }
}

// ---------------------------------------------------------------------------
// Pessimistic full eval — Phase 2 port of macondo's recursive PEG solver.
//
// Mirrors macondo's peg_generic.recursiveSolve for non-emptier cands. At
// each node:
//   - Base case (game over OR bag empty): run endgame_solve, classify the
//     spread as W/L/D for mover.
//   - Bag non-empty, opp's turn: enumerate ALL legal opp moves, recurse.
//     Take the WORST outcome (opp picks the move that hurts mover most).
//     First-loss cutoff: as soon as some opp reply gives mover a loss,
//     return loss (subsequent replies can't make it worse).
//   - Bag non-empty, mover's turn: enumerate ALL legal mover moves, recurse.
//     Take the BEST outcome (mover plays optimally). First-win cutoff: if
//     some reply gives a win, return win.
//
// For each (cand, bag-ordering) pair, accumulate W/L/D. Report per-cand
// win% = (wins + 0.5*draws) / total_orderings.
//
// Env vars (all required for the test):
//   PASSPEG_PESSFULL_CGP       CGP string (with -lex)
//   PASSPEG_PESSFULL_MOVE      mover's cand in MAGPIE notation
//   PASSPEG_PESSFULL_PLIES     endgame_solve plies at the bag-empty leaf
//                              (default 12)
//   PASSPEG_PESSFULL_TIME      per-solve time budget (default 5)
//   PASSPEG_PESSFULL_MAX_OPP_K cap opp move enumeration at top-K by score
//                              (default 0 = no cap). Loose pessimistic if
//                              set — opp may not be playing globally-best.
// ---------------------------------------------------------------------------

typedef enum {
  PEG_OUT_LOSS = 0,
  PEG_OUT_DRAW = 1,
  PEG_OUT_WIN = 2,
} PegPessOut;

// Endgame-position cache. At the bag-empty leaf, multiple recursion paths can
// converge to identical (board, racks, on-turn, lead, scoreless) states. The
// endgame_solve at the leaf is expensive (~ms each) and deterministic in that
// state, so caching by hash-of-state is correct and high-leverage.
typedef struct PegPessCacheEntry {
  uint64_t key;
  PegPessOut outcome;
  bool valid;
} PegPessCacheEntry;

typedef struct PegPessCache {
  PegPessCacheEntry *entries;
  size_t capacity; // power of 2
  size_t mask;
  cpthread_mutex_t mutex;
  atomic_long hits;
  atomic_long misses;
} PegPessCache;

// Forward declaration; full definition appears further down with the other
// nested-cache helpers.
typedef struct PegNestedCache PegNestedCache;
// Forward decl: the solver holds a back-pointer to its job (for recursive
// forking, which needs the pool/arena/slot arrays/config). Only a pointer is
// stored here, so the incomplete type is fine; PessJob is defined later.
typedef struct PessJob PessJob;
// Forward decls: the solver holds an arena pointer and the fork/parallel gates
// query free-slot availability (defined later, with the arena helpers).
typedef struct PessSlotArena PessSlotArena;
static int pess_arena_free(PessSlotArena *a);

typedef struct PegPessSolver {
  Game *game;
  int mover_idx;
  int opp_idx;
  int ld_size;
  ThreadControl *thread_control;
  int endgame_plies;
  double endgame_time;
  double tt_fraction_of_mem;     // per-worker endgame TT size (0 = disabled)
  TranspositionTable *shared_tt; // if set, all workers share this TT
  // Move order key: 0=score, 1=equity, 2=tiles-then-score, 3=tiles-then-equity.
  // Ordering affects cutoff/leaf-reach speed only, never the W/L/D verdict.
  int opp_sort_mode;   // opp-turn nodes (first-loss cutoff on LOSS orderings)
  int mover_sort_mode; // our-turn nodes (first-win cutoff on WIN orderings)
  bool subperm_sort;   // sort nested sub-perms by opp danger (hardest first)
  double
      slow_solve_log_s; // log CGP when one endgame_solve exceeds this (0=off)
  double idle_probe_s;  // sample pool idle count when a leaf solve exceeds
                        // this many seconds (0=off); rung-5 instrumentation
  double rung4_probe_s; // sample idle count when a nested cand loop exceeds
                        // this many seconds (0=off); rung-4 instrumentation
  bool first_win; // endgame [-1,+1] window: resolve sign only (verdict-safe)
  int endgame_threads; // per-solve thread count (1=default; >1 for ONLY_DRAW)
  bool skip_word_pruning; // pass-through to endgame_solve_inline
  int max_opp_k;          // 0 = no cap
  bool nested_our_turn;   // imperfect-info mover at mid-bag nodes
  int nested_depth_limit; // -1 = unbounded; 0 = no nesting; 1 = first only; ...
  int nested_depth;       // current nested-recursion depth
  int nested_mover_k;     // cap on mover cand enumeration inside nested (0 = no
                          // cap)
  EndgameCtx **eg_ctx_p;  // caller-owned slot; endgame_solve_inline may alloc
  EndgameResults *eg_results;
  PegPessCache *cache;          // shared across workers; NULL = disabled
  PegNestedCache *nested_cache; // info-state → per-bag-sig verdict map
  // Per-worker MoveList freelist. recursive_solve borrows one on entry, returns
  // on exit. Fixed-size pool; allocates fresh if exhausted. Sized larger than
  // expected max recursion depth.
  MoveList *ml_pool[16];
  int ml_pool_count;
  // Reusable scratch game for endgame_solve at the base case. Allocated once
  // per worker (lazily), refreshed with game_copy each solve — avoids a
  // game_duplicate/game_destroy per leaf (hundreds of thousands on big runs).
  Game *eg_scratch;
  // Recursive-split forking state. job back-pointer gives the fork helper the
  // pool/arena/slots/config; recursive_split gates forking; fork_depth bounds
  // it. Set by init_solver_from_job + the fork worker; the non-split path
  // leaves recursive_split=false so it never forks.
  const PessJob *job;
  PegPool
      *pool; // direct copy of job->pool (PessJob is incomplete in nested_solve)
  PessSlotArena *arena; // direct copy of job->arena, for the fork/parallel gate
  bool recursive_split;
  bool force_nested_perm; // debug: bypass the queue gate (tsan/testing only)
  int fork_depth;
  int64_t n_endgame_solves;
  int64_t n_leaf_visits; // bag-empty-game-on leaves, counted even on cache hit
  int64_t n_recursive_calls;
  int64_t n_first_loss_cutoffs;
  int64_t n_first_win_cutoffs;
  int64_t n_nested_calls; // diagnostic
} PegPessSolver;

static PegPessCache *peg_pess_cache_create(size_t cap_request) {
  size_t cap = 1;
  while (cap < cap_request) {
    cap *= 2;
  }
  PegPessCache *c = malloc_or_die(sizeof(*c));
  c->entries = calloc_or_die(cap, sizeof(PegPessCacheEntry));
  c->capacity = cap;
  c->mask = cap - 1;
  cpthread_mutex_init(&c->mutex);
  atomic_init(&c->hits, 0);
  atomic_init(&c->misses, 0);
  return c;
}

static void peg_pess_cache_destroy(PegPessCache *c) {
  if (!c) {
    return;
  }
  free(c->entries);
  free(c);
}

static inline uint64_t peg_pess_fnv1a(uint64_t hash, const void *data,
                                      size_t len) {
  const uint8_t *bytes = (const uint8_t *)data;
  for (size_t i = 0; i < len; i++) {
    hash ^= bytes[i];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

// Hash the bag-empty game state. Called only when bag is empty, so bag
// layout is irrelevant. Includes lead (mover - opp) and scoreless turns.
static uint64_t peg_pess_hash_endgame_state(const Game *g, int mover_idx,
                                            int opp_idx) {
  uint64_t hash = 0xcbf29ce484222325ULL;
  const Board *b = game_get_board(g);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      MachineLetter ml = board_get_letter(b, row, col);
      hash = peg_pess_fnv1a(hash, &ml, sizeof(ml));
    }
  }
  const Rack *mr = player_get_rack(game_get_player(g, mover_idx));
  const Rack *opp_r = player_get_rack(game_get_player(g, opp_idx));
  const int dist_size = ld_get_size(game_get_ld(g));
  for (int i = 0; i < dist_size; i++) {
    uint8_t mc = rack_get_letter(mr, i);
    uint8_t oc = rack_get_letter(opp_r, i);
    hash = peg_pess_fnv1a(hash, &mc, sizeof(mc));
    hash = peg_pess_fnv1a(hash, &oc, sizeof(oc));
  }
  const int32_t lead =
      equity_to_int(player_get_score(game_get_player(g, mover_idx))) -
      equity_to_int(player_get_score(game_get_player(g, opp_idx)));
  hash = peg_pess_fnv1a(hash, &lead, sizeof(lead));
  const int turn = game_get_player_on_turn_index(g);
  hash = peg_pess_fnv1a(hash, &turn, sizeof(turn));
  // Avoid 0 as a sentinel.
  if (hash == 0) {
    hash = 1;
  }
  return hash;
}

// Returns true on hit (and writes *out). Mutex-protected.
static bool peg_pess_cache_lookup(PegPessCache *c, uint64_t key,
                                  PegPessOut *out) {
  if (!c) {
    return false;
  }
  cpthread_mutex_lock(&c->mutex);
  size_t idx = (size_t)key & c->mask;
  for (size_t probe = 0; probe < c->capacity; probe++) {
    const PegPessCacheEntry *e = &c->entries[(idx + probe) & c->mask];
    if (!e->valid) {
      cpthread_mutex_unlock(&c->mutex);
      atomic_fetch_add(&c->misses, 1);
      return false;
    }
    if (e->key == key) {
      *out = e->outcome;
      cpthread_mutex_unlock(&c->mutex);
      atomic_fetch_add(&c->hits, 1);
      return true;
    }
  }
  cpthread_mutex_unlock(&c->mutex);
  return false;
}

static void peg_pess_cache_store(PegPessCache *c, uint64_t key,
                                 PegPessOut outcome) {
  if (!c) {
    return;
  }
  cpthread_mutex_lock(&c->mutex);
  size_t idx = (size_t)key & c->mask;
  for (size_t probe = 0; probe < c->capacity; probe++) {
    PegPessCacheEntry *e = &c->entries[(idx + probe) & c->mask];
    if (!e->valid || e->key == key) {
      e->key = key;
      e->outcome = outcome;
      e->valid = true;
      cpthread_mutex_unlock(&c->mutex);
      return;
    }
  }
  cpthread_mutex_unlock(&c->mutex);
}

static PegPessOut peg_pess_classify_spread(int32_t spread) {
  if (spread > 0) {
    return PEG_OUT_WIN;
  }
  if (spread < 0) {
    return PEG_OUT_LOSS;
  }
  return PEG_OUT_DRAW;
}

// Move ordering key. Higher = tried first. Modes 2/3 pack tiles_played into
// the high bits so longer plays sort first (shallower cascades), score/equity
// as the low-bits tiebreak (offset to unsigned for negative equities).
static int64_t peg_pess_opp_sort_key(const Move *m, int mode) {
  const int32_t score = (int32_t)move_get_score(m);
  const int32_t eq = (int32_t)move_get_equity(m);
  switch (mode) {
  case 1:
    return (int64_t)eq;
  case 2:
    return ((int64_t)move_get_tiles_played(m) << 32) +
           ((int64_t)score + 0x80000000LL);
  case 3:
    return ((int64_t)move_get_tiles_played(m) << 32) +
           ((int64_t)eq + 0x80000000LL);
  default:
    return (int64_t)score;
  }
}

// Forward decl for recursion.
static PegPessOut peg_pess_recursive_solve(PegPessSolver *s);
// Forward decl: parallel min-reduction over opp replies at an opp node, used
// by recursive_solve when recursive_split is enabled. Defined after PessJob /
// the fork worker (it needs the job's pool/arena/slots). Returns the opp
// node's outcome (min over replies); the caller frees order/ml.
static PegPessOut peg_pess_fork_opp_node(PegPessSolver *s, const MoveList *ml,
                                         const int *order, int cand_n);

// Per-pthread fork-nesting depth, incremented around each nested submit-and-
// wait. Two independent bounds govern forking (see the gates): the arena-free
// check manages *slots* (resource/capacity, dynamic), while this counter caps
// *C-stack depth* — cross-unit help-draining can stack many forking frames on
// one pthread, and without a depth bound a deep tail could overflow a worker
// thread's stack. The cap is set high (PEG_MAX_FORK_NESTING) so it never
// constrains the real search depth (~10-20); it's purely a stack-overflow
// backstop. The earlier value of 2 was the bug — it throttled the tail.
static __thread int g_peg_fork_nesting = 0;
enum { PEG_MAX_FORK_NESTING = 32 };

// Forward decl: evaluate one nested cand's sub-perms in parallel across the
// pool. Fills out_arr[0..pc->count-1] with per-perm outcomes and sets
// *cand_score (2*wins+draws) / *cand_losses (weighted). Defined after PessJob /
// the perm worker. Verdict-invariant vs the sequential perm loop — it just
// drops the within-cand minLossesSoFar early-break (computes all perms) to
// parallelize, which never changes the leader or its outcome row.
typedef struct NestedPermCollect NestedPermCollect;
static void peg_pess_parallel_perms(const PegPessSolver *s, MoveList *ml,
                                    int cand_move_idx, const uint8_t *unseen,
                                    const NestedPermCollect *pc,
                                    PegPessOut *out_arr, int64_t *cand_score,
                                    int64_t *cand_losses);

// --- Rung-5 probe: "heavy leaf endgame while cores idle" -------------------
// When idle_probe_s > 0, each leaf endgame_solve is timed; one exceeding the
// threshold samples how many pool workers were idle at that moment. This
// measures how often a single long leaf solve coincides with spare cores —
// i.e. how much a multithreaded (injected-worker) endgame would have helped.
// Counters are global (one solve per process) and affect only the summary,
// never verdicts. Reset at the start of each run; printed at the end.
static atomic_llong g_probe_slow_solves = 0;
static atomic_llong g_probe_slow_idle_sum =
    0; // sum of idle counts at slow solves
static atomic_llong g_probe_slow_with_idle = 0; // slow solves with idle >= 2

// --- Rung-4 probe: "heavy sequential candidate loop while cores idle" -------
// Rung 4 would parallelize nested_solve's candidate loop. It is unbuilt
// because the loop carries the cross-cand minLossesSoFar leader + early-WIN
// cutoff. To decide whether it is worth the (pruning-delicate) build, time
// each cand loop; when one exceeds rung4_probe_s, record whether it ran with
// NO parallelism at this level (parallel_perms == false — a rung-4 opportunity)
// and how many cores were idle. If heavy sequential cand loops frequently
// coincide with idle cores, rung 4 matters. Counts overlap across nesting
// (an outer slow loop contains inner ones); the idle-coincidence ratio is the
// distortion-free signal. Reporting only; never affects the verdict.
static atomic_llong g_rung4_slow = 0;         // cand loops >= threshold
static atomic_llong g_rung4_slow_seq = 0;     // ...with parallel_perms == false
static atomic_llong g_rung4_seq_idle_sum = 0; // sum idle at slow-seq loops
static atomic_llong g_rung4_seq_with_idle = 0; // slow-seq loops with idle >= 2
static atomic_llong g_rung4_seq_cands_sum = 0; // sum n_cands at slow-seq loops

// Evaluate at a base case: bag empty or game over. Returns mover's outcome.
static PegPessOut peg_pess_base_case(PegPessSolver *s) {
  const Game *g = s->game;
  if (game_get_game_end_reason(g) != GAME_END_REASON_NONE) {
    const int32_t mover_score =
        equity_to_int(player_get_score(game_get_player(g, s->mover_idx)));
    const int32_t opp_score =
        equity_to_int(player_get_score(game_get_player(g, s->opp_idx)));
    return peg_pess_classify_spread(mover_score - opp_score);
  }
  // Cache lookup: same endgame position from different recursion paths
  // produces the same outcome — endgame_solve is deterministic in
  // (board, racks, on-turn, lead).
  // Count every bag-empty-game-on leaf, including cache hits — mirrors
  // macondo's numEndgamesSolved counting point so the total is comparable.
  s->n_leaf_visits++;
  uint64_t cache_key = 0;
  if (s->cache) {
    cache_key = peg_pess_hash_endgame_state(g, s->mover_idx, s->opp_idx);
    PegPessOut cached;
    if (peg_pess_cache_lookup(s->cache, cache_key, &cached)) {
      return cached;
    }
  }

  // Bag empty, game still on. endgame_solve_inline works off its own internal
  // worker game copy and only reads our input as a template, but we still
  // isolate it from the live recursion game `g` via a scratch copy. Reuse a
  // per-worker scratch (game_copy = memcpy-ish) instead of game_duplicate so
  // we don't malloc/free a whole Game per leaf. game_copy already resets
  // backup_mode to OFF.
  if (!s->eg_scratch) {
    s->eg_scratch = game_duplicate(g); // one-time structural alloc per worker
  } else {
    game_copy(s->eg_scratch, g);
  }
  Game *scratch = s->eg_scratch;
  game_set_endgame_solving_mode(scratch);
  EndgameArgs ea = {
      .thread_control = s->thread_control,
      .game = scratch,
      .plies = s->endgame_plies,
      // If a shared TT is provided, all workers use it (lockless Hyatt-XOR,
      // so concurrent access is safe). Otherwise fall back to a per-worker TT
      // sized by tt_fraction_of_mem (0 = no TT). shared_tt wins when both set.
      .shared_tt = s->shared_tt,
      .tt_fraction_of_mem = s->shared_tt ? 0.0 : s->tt_fraction_of_mem,
      .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      // Per-solve thread count. Default 1: the full run parallelizes across
      // orderings (one solve per worker). For single-ordering investigation
      // (ONLY_DRAW) set endgame_threads >1 to parallelize each solve across
      // cores. NOTE: >1 uses ABDADA (timing-dependent, non-deterministic) and
      // assumes a single active pess worker (thread_index_offset=0).
      .num_threads = s->endgame_threads,
      .use_heuristics = true,
      .num_top_moves = 1,
      .first_win = s->first_win,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .skip_word_pruning = s->skip_word_pruning,
      .soft_time_limit = s->endgame_time,
      .hard_time_limit = s->endgame_time,
      // Strict mid-search interruption: soft/hard_time_limit only gate between
      // IDS depth iterations, so a single pathological iteration can run
      // unbounded. external_deadline_ns is checked inside alpha-beta, giving a
      // hard wall-clock cap per solve.
      .external_deadline_ns =
          s->endgame_time > 0.0
              ? ctimer_monotonic_ns() + (int64_t)(s->endgame_time * 1.0e9)
              : 0,
  };
  endgame_results_reset(s->eg_results);
  Timer slow_t;
  const bool time_this_solve =
      s->slow_solve_log_s > 0.0 || s->idle_probe_s > 0.0;
  if (time_this_solve) {
    ctimer_start(&slow_t);
  }
  if (s->slow_solve_log_s > 0.0) {
    // Pre-solve capture: overwrite a temp file with the CGP we're about to
    // solve. If a solve hangs (never returns), this file holds the culprit
    // position. PASSPEG_PESSFULL_PRESOLVE_FILE selects the path.
    const char *presolve_file = getenv("PASSPEG_PESSFULL_PRESOLVE_FILE");
    if (presolve_file) {
      char *pre_cgp = game_get_cgp(scratch, true);
      FILE *pf = fopen(presolve_file, "we");
      if (pf) {
        (void)fprintf(pf, "%s\n", pre_cgp);
        (void)fclose(pf);
      }
      free(pre_cgp);
    }
  }
  endgame_solve_inline(s->eg_ctx_p, &ea, s->eg_results);
  if (time_this_solve) {
    const double solve_s = ctimer_elapsed_seconds(&slow_t);
    if (s->slow_solve_log_s > 0.0 && solve_s >= s->slow_solve_log_s) {
      char *eg_cgp = game_get_cgp(scratch, true);
      (void)fprintf(stderr, "[pessfull] SLOW ENDGAME %.2fs plies=%d: %s\n",
                    solve_s, s->endgame_plies, eg_cgp);
      free(eg_cgp);
    }
    // Rung-5 probe: a leaf solve that ran long is a candidate for a
    // multithreaded (injected-worker) endgame — but only if cores were
    // actually idle to lend. Sample the pool's idle count at this moment and
    // accumulate, so the end-of-run summary shows how often "long leaf +
    // spare cores" coincided. Reporting only; never affects the verdict.
    if (s->idle_probe_s > 0.0 && solve_s >= s->idle_probe_s) {
      const int idle_now = s->pool ? peg_pool_idle_workers(s->pool) : 0;
      atomic_fetch_add(&g_probe_slow_solves, 1);
      atomic_fetch_add(&g_probe_slow_idle_sum, (long long)idle_now);
      if (idle_now >= 2) {
        atomic_fetch_add(&g_probe_slow_with_idle, 1);
      }
    }
  }
  s->n_endgame_solves++;
  const int eg_val =
      endgame_results_get_value(s->eg_results, ENDGAME_RESULT_BEST);
  const int turn = game_get_player_on_turn_index(scratch);
  const int32_t mover_lead =
      equity_to_int(player_get_score(game_get_player(scratch, s->mover_idx))) -
      equity_to_int(player_get_score(game_get_player(scratch, s->opp_idx)));
  const int32_t mover_total =
      (turn == s->mover_idx) ? mover_lead + eg_val : mover_lead - eg_val;
  // No game_destroy: scratch is the reused per-worker eg_scratch, freed at
  // teardown.
  const PegPessOut out = peg_pess_classify_spread(mover_total);
  if (s->cache) {
    peg_pess_cache_store(s->cache, cache_key, out);
  }
  return out;
}

// ---- Nested verdict-map cache (macondo-style) -----------------------------
// Key on info-state: (board, our_rack multiset, unseen multiset, scoreless,
// on-turn). Value is a per-bag-sig outcome map for the leader cand at that
// info-state. Two callers with the same info-state but different actual bag
// orderings each retrieve their own bag's verdict without re-solving.
typedef struct VerdictEntry {
  uint64_t bag_sig;
  PegPessOut outcome;
} VerdictEntry;

typedef struct VerdictMap {
  VerdictEntry *entries; // owned; freed when entry is overwritten
  int count;
  int cap;
} VerdictMap;

typedef struct PegNestedCacheEntry {
  uint64_t key;
  VerdictMap map;
  bool valid;
} PegNestedCacheEntry;

struct PegNestedCache {
  PegNestedCacheEntry *entries;
  size_t capacity;
  size_t mask;
  cpthread_mutex_t mutex;
  atomic_long hits;       // info-state lookup hit (verdict found)
  atomic_long misses;     // info-state lookup miss
  atomic_long bag_misses; // info-state hit but bag_sig absent (should be rare)
};

static PegNestedCache *peg_nested_cache_create(size_t cap_request) {
  size_t cap = 1;
  while (cap < cap_request) {
    cap *= 2;
  }
  PegNestedCache *c = malloc_or_die(sizeof(*c));
  c->entries = calloc_or_die(cap, sizeof(PegNestedCacheEntry));
  c->capacity = cap;
  c->mask = cap - 1;
  cpthread_mutex_init(&c->mutex);
  atomic_init(&c->hits, 0);
  atomic_init(&c->misses, 0);
  atomic_init(&c->bag_misses, 0);
  return c;
}

static void peg_nested_cache_destroy(PegNestedCache *c) {
  if (!c) {
    return;
  }
  for (size_t i = 0; i < c->capacity; i++) {
    if (c->entries[i].valid) {
      free(c->entries[i].map.entries);
    }
  }
  free(c->entries);
  free(c);
}

// Encode a bag's ordered content into a uint64. 5 bits per letter, lo→hi
// position. Caller must ensure bag_size * 5 ≤ 64 (i.e., bag_size ≤ 12).
// Assumes ml fits in 5 bits (≤ 31).
static uint64_t peg_pess_encode_bag_sig(const MachineLetter *tiles, int n) {
  uint64_t sig = (uint64_t)(n & 0xF); // include length so empties of
                                      // different sizes don't collide
  for (int i = 0; i < n; i++) {
    sig |= ((uint64_t)(tiles[i] & 0x1F)) << (4 + (size_t)i * 5);
  }
  return sig;
}

// Hash the info-state visible to the mover under imperfect info:
// (board, our rack multiset, unseen multiset, scoreless turns, on-turn).
// Bag content is NOT in this hash — it's the bag-sig used as a sub-index.
static uint64_t peg_nested_hash_info_state(const Game *g, int mover_idx,
                                           const uint8_t *unseen, int ld_size) {
  uint64_t hash = 0xcbf29ce484222325ULL;
  const Board *b = game_get_board(g);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      MachineLetter ml = board_get_letter(b, row, col);
      hash = peg_pess_fnv1a(hash, &ml, sizeof(ml));
    }
  }
  const Rack *our_r = player_get_rack(game_get_player(g, mover_idx));
  for (int ml_i = 0; ml_i < ld_size; ml_i++) {
    const uint8_t our_c = rack_get_letter(our_r, ml_i);
    hash = peg_pess_fnv1a(hash, &our_c, sizeof(our_c));
    const uint8_t un_c = unseen[ml_i];
    hash = peg_pess_fnv1a(hash, &un_c, sizeof(un_c));
  }
  const int sc = game_get_consecutive_scoreless_turns(g);
  hash = peg_pess_fnv1a(hash, &sc, sizeof(sc));
  hash = peg_pess_fnv1a(hash, &mover_idx, sizeof(mover_idx));
  if (hash == 0) {
    hash = 1;
  }
  return hash;
}

// Lookup: returns true and writes *out if (key, bag_sig) was cached.
// outcome_bag_miss is true if the info-state was cached but bag_sig wasn't
// in the map (defensive — fall through to recompute).
static bool peg_nested_cache_lookup(PegNestedCache *c, uint64_t key,
                                    uint64_t bag_sig, PegPessOut *out,
                                    bool *info_state_hit) {
  *info_state_hit = false;
  if (!c) {
    return false;
  }
  cpthread_mutex_lock(&c->mutex);
  size_t idx = (size_t)key & c->mask;
  for (size_t probe = 0; probe < c->capacity; probe++) {
    PegNestedCacheEntry *e = &c->entries[(idx + probe) & c->mask];
    if (!e->valid) {
      break;
    }
    if (e->key == key) {
      *info_state_hit = true;
      for (int i = 0; i < e->map.count; i++) {
        if (e->map.entries[i].bag_sig == bag_sig) {
          *out = e->map.entries[i].outcome;
          cpthread_mutex_unlock(&c->mutex);
          atomic_fetch_add(&c->hits, 1);
          return true;
        }
      }
      // Info-state hit but bag_sig absent.
      cpthread_mutex_unlock(&c->mutex);
      atomic_fetch_add(&c->bag_misses, 1);
      return false;
    }
  }
  cpthread_mutex_unlock(&c->mutex);
  atomic_fetch_add(&c->misses, 1);
  return false;
}

// Store: takes ownership of `entries` (caller must not free). Overwrites any
// existing entry at this key.
static void peg_nested_cache_store(PegNestedCache *c, uint64_t key,
                                   VerdictEntry *entries, int count) {
  if (!c) {
    free(entries);
    return;
  }
  cpthread_mutex_lock(&c->mutex);
  size_t idx = (size_t)key & c->mask;
  for (size_t probe = 0; probe < c->capacity; probe++) {
    PegNestedCacheEntry *e = &c->entries[(idx + probe) & c->mask];
    if (!e->valid || e->key == key) {
      if (e->valid) {
        free(e->map.entries);
      }
      e->key = key;
      e->map.entries = entries;
      e->map.count = count;
      e->map.cap = count;
      e->valid = true;
      cpthread_mutex_unlock(&c->mutex);
      return;
    }
  }
  // Table full — drop the new entry. (Could LRU-evict if needed.)
  free(entries);
  cpthread_mutex_unlock(&c->mutex);
}

// One materialized sub-perm: a specific ordered draw from the unseen multiset
// (first n_drawn tiles go into the bag for this scenario; the rest become opp's
// rack). The weight is the number of distinct draw orderings this perm
// represents — used so equal-multiset perms aggregate correctly.
typedef struct NestedPerm {
  MachineLetter draw[16];
  int n;
  int64_t weight;
  int64_t est; // opp danger estimate (top opp-move equity), for sub-perm sort
} NestedPerm;

struct NestedPermCollect {
  NestedPerm *perms;
  int cap;
  int count;
};

static void peg_pess_nested_collect_perm_cb(const MachineLetter *draw, int n,
                                            int64_t weight, void *user) {
  NestedPermCollect *c = (NestedPermCollect *)user;
  if (c->count >= c->cap) {
    c->cap = c->cap ? c->cap * 2 : 64;
    c->perms = realloc_or_die(c->perms, (size_t)c->cap * sizeof(NestedPerm));
  }
  NestedPerm *p = &c->perms[c->count++];
  p->n = n;
  p->weight = weight;
  memcpy(p->draw, draw, (size_t)n * sizeof(MachineLetter));
}

// Run one (cand, perm) scenario: rebuild alt-game with the perm's bag/rack,
// play the cand, recurse. Returns the outcome.
static PegPessOut
peg_pess_nested_run_scenario(PegPessSolver *s, const Game *base_state,
                             const MoveList *ml, int cand_move_idx,
                             const uint8_t *unseen, const MachineLetter *draw,
                             int n_drawn) {
  Game *alt = game_duplicate(base_state);
  game_set_backup_mode(alt, BACKUP_MODE_OFF);
  Bag *bag = game_get_bag(alt);
  // Deterministically place the perm's bag tiles (no PRNG). bag_add_letter
  // would randomize the order via the bag's time-seeded PRNG, which is both
  // non-deterministic across processes and breaks per-ordering enumeration.
  bag_set_to_tiles(bag, draw, n_drawn);
  Rack *opp_rack = player_get_rack(game_get_player(alt, s->opp_idx));
  rack_reset(opp_rack);
  uint8_t leftover[MAX_ALPHABET_SIZE];
  memcpy(leftover, unseen, (size_t)s->ld_size);
  for (int i = 0; i < n_drawn; i++) {
    leftover[draw[i]]--;
  }
  for (int ml_i = 0; ml_i < s->ld_size; ml_i++) {
    for (int k = 0; k < (int)leftover[ml_i]; k++) {
      rack_add_letter(opp_rack, (MachineLetter)ml_i);
    }
  }
  const Move *m = move_list_get_move(ml, cand_move_idx);
  play_move(m, alt, NULL);
  game_set_game_end_reason(alt, GAME_END_REASON_NONE);
  Game *saved = s->game;
  s->game = alt;
  PegPessOut out = peg_pess_recursive_solve(s);
  s->game = saved;
  game_destroy(alt);
  return out;
}

// Imperfect-info our-turn solve under macondo semantics. Returns the leader
// cand's outcome at the actual current bag ordering. The leader is the cand
// with minimum losses across all consistent splits of the unseen multiset.
//
// Cand-outer / perm-inner: cands are tried in score-descending order; per-cand
// running losses are tracked, and the perm loop breaks when cand losses exceed
// minLossesSoFar (strict >). Per-perm outcomes are recorded only for cands
// that complete every perm, so the leader's per-perm outcome row is intact.
//
// Result is cached by info-state (board, our rack, unseen multiset, scoreless,
// on-turn); value is the per-bag-sig verdict map for the leader cand.
static PegPessOut peg_pess_nested_solve(PegPessSolver *s, MoveList *ml,
                                        const int *order, int n_cands) {
  s->n_nested_calls++;
  Game *g = s->game;
  const Rack *opp_r = player_get_rack(game_get_player(g, s->opp_idx));
  const Bag *bag = game_get_bag(g);
  const int bag_size = bag_get_letters(bag);

  uint8_t unseen[MAX_ALPHABET_SIZE] = {0};
  for (int ml_i = 0; ml_i < s->ld_size; ml_i++) {
    unseen[ml_i] =
        rack_get_letter(opp_r, ml_i) + bag_get_letter(bag, (MachineLetter)ml_i);
  }

  // Compute the actual bag's sig — needed both to short-circuit on cache hit
  // and to return the verdict at the actual bag ordering.
  MachineLetter actual_bag[16] = {0};
  const int actual_bag_n = bag_peek_tiles(bag, actual_bag);
  const uint64_t actual_bag_sig =
      peg_pess_encode_bag_sig(actual_bag, actual_bag_n);

  // Cache lookup by info-state hash + bag_sig.
  const uint64_t info_key =
      peg_nested_hash_info_state(g, s->mover_idx, unseen, s->ld_size);
  if (s->nested_cache) {
    PegPessOut cached;
    bool info_hit = false;
    if (peg_nested_cache_lookup(s->nested_cache, info_key, actual_bag_sig,
                                &cached, &info_hit)) {
      return cached;
    }
    // info_hit but bag_sig absent ⇒ fall through and recompute. Rare; we'll
    // overwrite the cached entry with a freshly-built map below.
    (void)info_hit;
  }

  MachineLetter tile_types[MAX_ALPHABET_SIZE] = {0};
  int tile_counts[MAX_ALPHABET_SIZE];
  int num_types = 0;
  for (int ml_i = 0; ml_i < s->ld_size; ml_i++) {
    if (unseen[ml_i] > 0) {
      tile_types[num_types] = (MachineLetter)ml_i;
      tile_counts[num_types] = (int)unseen[ml_i];
      num_types++;
    }
  }

  // Materialize sub-perms up front so we can iterate cand-outer / perm-inner.
  NestedPermCollect pc = {0};
  MachineLetter draw_buf[16];
  peg_pess_enum_ordered_draws(tile_types, tile_counts, num_types, bag_size,
                              draw_buf, 0, /*multiplicity=*/1,
                              peg_pess_nested_collect_perm_cb, &pc);

  // Optional sub-perm ordering (macondo parity): estimate each perm's danger
  // by opp's top-move equity given opp's rack = leftover, sort hardest-first
  // so minLossesSoFar prunes losing cands after fewer perms. generate_moves
  // only reads board/rack/on-turn (cross-sets valid here), so save/restore on
  // g rather than duplicate. Pure speed heuristic; verdict is perm-invariant.
  if (s->subperm_sort && pc.count > 1) {
    const int saved_turn = game_get_player_on_turn_index(g);
    Rack *opp_rack = player_get_rack(game_get_player(g, s->opp_idx));
    Rack saved_opp;
    rack_copy(&saved_opp, opp_rack);
    game_set_player_on_turn_index(g, s->opp_idx);
    MoveList *est_ml;
    if (s->ml_pool_count > 0) {
      est_ml = s->ml_pool[--s->ml_pool_count];
      move_list_reset(est_ml);
    } else {
      est_ml = move_list_create(16384);
    }
    for (int pi = 0; pi < pc.count; pi++) {
      uint8_t leftover[MAX_ALPHABET_SIZE];
      memcpy(leftover, unseen, (size_t)s->ld_size);
      for (int i = 0; i < pc.perms[pi].n; i++) {
        leftover[pc.perms[pi].draw[i]]--;
      }
      rack_reset(opp_rack);
      for (int ml_i = 0; ml_i < s->ld_size; ml_i++) {
        for (int k = 0; k < (int)leftover[ml_i]; k++) {
          rack_add_letter(opp_rack, (MachineLetter)ml_i);
        }
      }
      move_list_reset(est_ml);
      const MoveGenArgs est_ga = {
          .game = g,
          .move_list = est_ml,
          .move_record_type = MOVE_RECORD_ALL,
          .move_sort_type = MOVE_SORT_EQUITY,
          .override_kwg = NULL,
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
          .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      };
      generate_moves(&est_ga);
      int64_t top = INT64_MIN;
      const int nm = move_list_get_count(est_ml);
      for (int i = 0; i < nm; i++) {
        const int64_t e = move_get_equity(move_list_get_move(est_ml, i));
        if (e > top) {
          top = e;
        }
      }
      pc.perms[pi].est = top;
    }
    if (s->ml_pool_count < (int)(sizeof(s->ml_pool) / sizeof(s->ml_pool[0]))) {
      s->ml_pool[s->ml_pool_count++] = est_ml;
    } else {
      move_list_destroy(est_ml);
    }
    rack_copy(opp_rack, &saved_opp);
    game_set_player_on_turn_index(g, saved_turn);
    for (int i = 1; i < pc.count; i++) {
      const NestedPerm key = pc.perms[i];
      int j = i - 1;
      while (j >= 0 && pc.perms[j].est < key.est) {
        pc.perms[j + 1] = pc.perms[j];
        j--;
      }
      pc.perms[j + 1] = key;
    }
  }

  // Per-perm outcome row for the current leader (the only cand we'd cache).
  // Rebuilt every time we adopt a new leader so the row matches that cand.
  PegPessOut *leader_outcomes = NULL;
  PegPessOut *cur_outcomes =
      malloc_or_die((size_t)pc.count * sizeof(PegPessOut));

  int64_t min_losses = INT64_MAX;
  int64_t best_score = INT64_MIN;
  bool best_set = false;

  // Parallelize each cand's sub-perms across the pool when recursive_split is
  // on (and enough perms / nesting headroom). The cand loop stays sequential so
  // the cross-cand minLossesSoFar leader-finding + early-WIN exit are preserved
  // exactly; only the within-cand perm loop is parallelized (and its early-
  // break dropped — verdict-invariant, just more work traded for cores).
  // Only parallelize perms when the pool has spare capacity. Parallel perms
  // drop the within-cand early-break (extra work), so they only pay off when
  // workers would otherwise idle. When the queue is backed up (capped runs, or
  // early/middle of a full run where orderings + opp-forks keep cores busy),
  // run sequentially and keep the cutoff. This self-tunes: nested-perm parallel
  // engages exactly in the drained tail where the single-core collapse
  // happened. Two signals, both required: queue_count < num_workers means cores
  // would otherwise idle (so the redundant perm work pays off — this keeps
  // nested- perm OFF mid-run when the queue is backed up), and arena_free >=
  // num_workers means there's slot headroom to nest safely. Together they
  // engage nested- perm exactly in the drained tail, and let it nest as deep as
  // slots allow (no fixed cap), which is what fixes the single-core tail
  // collapse. arena_free >= num_workers is a SAFETY invariant (slot headroom) —
  // always required, never bypassed. queue_count < num_workers is the capacity
  // heuristic (only worth the redundant perm work when cores would idle) —
  // force_nested_perm bypasses just that, for tsan/testing.
  enum { PEG_NESTED_PERM_MIN = 4 };
  const bool parallel_perms =
      s->recursive_split && s->pool && s->arena &&
      pc.count >= PEG_NESTED_PERM_MIN &&
      g_peg_fork_nesting < PEG_MAX_FORK_NESTING &&
      pess_arena_free(s->arena) >= peg_pool_num_workers(s->pool) &&
      (s->force_nested_perm ||
       peg_pool_queue_count(s->pool) < peg_pool_num_workers(s->pool));
  Timer rung4_t = {0};
  if (s->rung4_probe_s > 0.0) {
    ctimer_start(&rung4_t);
  }
  for (int ci = 0; ci < n_cands; ci++) {
    int64_t cand_score = 0; // 2*wins + draws
    int64_t cand_losses = 0;
    bool completed_all = true;
    if (parallel_perms) {
      peg_pess_parallel_perms(s, ml, order[ci], unseen, &pc, cur_outcomes,
                              &cand_score, &cand_losses);
      // All perms computed (no within-cand break).
    } else {
      for (int pi = 0; pi < pc.count; pi++) {
        const NestedPerm *p = &pc.perms[pi];
        PegPessOut out = peg_pess_nested_run_scenario(s, g, ml, order[ci],
                                                      unseen, p->draw, p->n);
        cur_outcomes[pi] = out;
        if (out == PEG_OUT_WIN) {
          cand_score += 2 * p->weight;
        } else if (out == PEG_OUT_DRAW) {
          cand_score += p->weight;
        } else {
          cand_losses += p->weight;
        }
        // Strict-greater: tied cands survive to the next perm so we don't lose
        // info-state coverage for the across-tie pick.
        if (cand_losses > min_losses) {
          completed_all = false;
          break;
        }
      }
    }
    if (!completed_all) {
      continue; // cand was pruned mid-loop
    }
    // Cand finished every perm. Update the leader.
    if (!best_set || cand_losses < min_losses ||
        (cand_losses == min_losses && cand_score > best_score)) {
      min_losses = cand_losses;
      best_score = cand_score;
      best_set = true;
      // Adopt this cand as leader; swap cur_outcomes ↔ leader_outcomes.
      PegPessOut *tmp = leader_outcomes;
      leader_outcomes = cur_outcomes;
      cur_outcomes =
          tmp ? tmp : malloc_or_die((size_t)pc.count * sizeof(PegPessOut));
      // Early-WIN exit: leader has zero losses → cannot be beaten.
      if (cand_losses == 0) {
        break;
      }
    }
  }
  if (s->rung4_probe_s > 0.0) {
    const double loop_s = ctimer_elapsed_seconds(&rung4_t);
    if (loop_s >= s->rung4_probe_s) {
      const int idle_now = s->pool ? peg_pool_idle_workers(s->pool) : 0;
      atomic_fetch_add(&g_rung4_slow, 1);
      // parallel_perms == false means this cand loop ran with no parallelism at
      // this level — the entire cand x perm subtree was sequential. If cores
      // were idle, parallelizing the cand loop (rung 4) would have filled them.
      if (!parallel_perms) {
        atomic_fetch_add(&g_rung4_slow_seq, 1);
        atomic_fetch_add(&g_rung4_seq_idle_sum, (long long)idle_now);
        atomic_fetch_add(&g_rung4_seq_cands_sum, (long long)n_cands);
        if (idle_now >= 2) {
          atomic_fetch_add(&g_rung4_seq_with_idle, 1);
        }
      }
    }
  }

  // Build the leader's verdict map. If no cand completed (all were pruned by
  // an outer minLossesSoFar that doesn't exist at this level — shouldn't
  // happen since min_losses starts at INT64_MAX), fall back to LOSS.
  PegPessOut verdict = PEG_OUT_LOSS;
  if (best_set && leader_outcomes) {
    VerdictEntry *map = malloc_or_die((size_t)pc.count * sizeof(VerdictEntry));
    for (int pi = 0; pi < pc.count; pi++) {
      map[pi].bag_sig =
          peg_pess_encode_bag_sig(pc.perms[pi].draw, pc.perms[pi].n);
      map[pi].outcome = leader_outcomes[pi];
      if (map[pi].bag_sig == actual_bag_sig) {
        verdict = map[pi].outcome;
      }
    }
    if (s->nested_cache) {
      peg_nested_cache_store(s->nested_cache, info_key, map, pc.count);
    } else {
      free(map);
    }
  }

  free(leader_outcomes);
  free(cur_outcomes);
  free(pc.perms);
  return verdict;
}

// Fill order[0..n_moves-1] with move indices sorted descending by the turn's
// ordering key (peg_pess_opp_sort_key; mover_sort_mode on our turn, opp_sort_
// mode on opp turn). Returns cand_n = the number of candidates to consider
// (max_opp_k cap on opp turns, else all). Shared by recursive_solve and the
// opp-split worker so their candidate ordering can never diverge.
static int peg_pess_sort_order(const PegPessSolver *s, const MoveList *ml,
                               int n_moves, bool our_turn, int *order) {
  for (int i = 0; i < n_moves; i++) {
    order[i] = i;
  }
  int cand_n = n_moves;
  if (!our_turn && s->max_opp_k > 0 && s->max_opp_k < cand_n) {
    cand_n = s->max_opp_k;
  }
  const int sort_mode = our_turn ? s->mover_sort_mode : s->opp_sort_mode;
  for (int i = 0; i < cand_n; i++) {
    int best = i;
    int64_t best_key =
        peg_pess_opp_sort_key(move_list_get_move(ml, order[i]), sort_mode);
    for (int j = i + 1; j < n_moves; j++) {
      const int64_t kj =
          peg_pess_opp_sort_key(move_list_get_move(ml, order[j]), sort_mode);
      if (kj > best_key) {
        best = j;
        best_key = kj;
      }
    }
    if (best != i) {
      const int tmp = order[i];
      order[i] = order[best];
      order[best] = tmp;
    }
  }
  return cand_n;
}

// Recursively solve from the current game state.
static PegPessOut peg_pess_recursive_solve(PegPessSolver *s) {
  Game *g = s->game;
  s->n_recursive_calls++;

  if (game_get_game_end_reason(g) != GAME_END_REASON_NONE ||
      bag_get_letters(game_get_bag(g)) == 0) {
    return peg_pess_base_case(s);
  }

  // Generate all legal moves for the player on turn. Borrow a MoveList from
  // the per-worker pool; fall back to malloc if exhausted.
  MoveList *ml;
  if (s->ml_pool_count > 0) {
    ml = s->ml_pool[--s->ml_pool_count];
    move_list_reset(ml);
  } else {
    ml = move_list_create(16384);
  }
  const MoveGenArgs ga = {
      .game = g,
      .move_list = ml,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&ga);
  const int n_moves = move_list_get_count(ml);
  const int turn = game_get_player_on_turn_index(g);
  const bool our_turn = (turn == s->mover_idx);

  // Build an explicit move-index order, sorted descending by the turn's key.
  // Shared with the opp-split worker via peg_pess_sort_order so candidate
  // ordering can never diverge between the two paths.
  int *order = malloc_or_die((size_t)n_moves * sizeof(int));
  const int cand_n = peg_pess_sort_order(s, ml, n_moves, our_turn, order);

  // Imperfect-info our-turn: mover doesn't know which tiles are in the bag.
  // Pick the cand that wins most across consistent splits of the unseen
  // multiset, then return that cand's outcome at the ACTUAL bag. Bounded by
  // nested_depth_limit (-1 = unbounded; 0 = disabled; N = only at depths < N).
  const bool nested_eligible =
      our_turn && s->nested_our_turn &&
      (s->nested_depth_limit < 0 || s->nested_depth < s->nested_depth_limit);
  if (nested_eligible) {
    // Cap mover cand enumeration inside nested if requested.
    int nested_cand_n = cand_n;
    if (s->nested_mover_k > 0 && s->nested_mover_k < nested_cand_n) {
      nested_cand_n = s->nested_mover_k;
    }
    s->nested_depth++;
    const PegPessOut out = peg_pess_nested_solve(s, ml, order, nested_cand_n);
    s->nested_depth--;
    free(order);
    if (s->ml_pool_count < (int)(sizeof(s->ml_pool) / sizeof(s->ml_pool[0]))) {
      s->ml_pool[s->ml_pool_count++] = ml;
    } else {
      move_list_destroy(ml);
    }
    return out;
  }

  // Recursive split: at an opp node with enough replies, fan the opp replies
  // out as parallel sub-tasks (min-reduction with first-loss cancel) instead of
  // the sequential loop below. Same verdict (min over replies); just
  // parallelized. The helper frees order + returns ml. Gate purely on arena
  // slot headroom (arena_free >= num_workers): forking is safe and useful
  // whenever slots are available, and the gate throttles nesting as slots
  // deplete under load — no fixed depth cap (which over-constrained the tail).
  enum { PEG_FORK_MIN_CANDS = 8 };
  if (!our_turn && s->recursive_split && s->job && s->arena && s->pool &&
      cand_n >= PEG_FORK_MIN_CANDS &&
      g_peg_fork_nesting < PEG_MAX_FORK_NESTING &&
      pess_arena_free(s->arena) >= peg_pool_num_workers(s->pool)) {
    const PegPessOut out = peg_pess_fork_opp_node(s, ml, order, cand_n);
    free(order);
    if (s->ml_pool_count < (int)(sizeof(s->ml_pool) / sizeof(s->ml_pool[0]))) {
      s->ml_pool[s->ml_pool_count++] = ml;
    } else {
      move_list_destroy(ml);
    }
    return out;
  }

  PegPessOut best_or_worst;
  if (our_turn) {
    best_or_worst = PEG_OUT_LOSS;
  } else {
    best_or_worst = PEG_OUT_WIN;
  }
  bool cutoff = false;

  // Mutate-and-undo recursion: avoid game_duplicate on the hot path. The
  // generated MoveUndo lives on the stack of this frame. After unplay the
  // game state (board, racks, bag, scores, turn, game_end_reason, cross-set
  // validity flag) is exactly as it was before this iteration's play.
  MoveUndo undo;
  for (int mi = 0; mi < cand_n; mi++) {
    const Move *m = move_list_get_move(ml, order[mi]);
    play_move_incremental(m, g, &undo);
    // Eagerly recompute cross-sets so the recursive call's generate_moves
    // sees a valid board. The updates are tracked in `undo` and reversed by
    // unplay_move_incremental.
    Board *b = game_get_board(g);
    if (!board_get_cross_sets_valid(b)) {
      update_cross_set_for_move_from_undo(&undo, g);
      board_set_cross_sets_valid(b, true);
    }
    PegPessOut sub = peg_pess_recursive_solve(s);
    unplay_move_incremental(g, &undo);

    if (our_turn) {
      if (sub > best_or_worst) {
        best_or_worst = sub;
      }
      if (best_or_worst == PEG_OUT_WIN) {
        s->n_first_win_cutoffs++;
        cutoff = true;
      }
    } else {
      if (sub < best_or_worst) {
        best_or_worst = sub;
      }
      if (best_or_worst == PEG_OUT_LOSS) {
        s->n_first_loss_cutoffs++;
        cutoff = true;
      }
    }
    if (cutoff) {
      break;
    }
  }

  free(order);
  if (s->ml_pool_count < (int)(sizeof(s->ml_pool) / sizeof(s->ml_pool[0]))) {
    s->ml_pool[s->ml_pool_count++] = ml;
  } else {
    move_list_destroy(ml);
  }
  return best_or_worst;
}

typedef struct PegPessFullAccum {
  int64_t wins;
  int64_t losses;
  int64_t draws;
  int64_t total;
} PegPessFullAccum;

// One materialized bag-draw permutation: the ordered tile sequence, its
// weight (# of distinguishable-tile orderings it represents), and after
// processing, the recursive solver's outcome.
typedef struct PegPessOrdering {
  MachineLetter draw[16];
  int n;
  int64_t weight;
  int opp_top_score;  // pre-computed for sorting (opp's best score after cand)
  PegPessOut outcome; // filled in by worker
} PegPessOrdering;

// Materialize all orderings via the existing enum.
typedef struct PessMaterializeCtx {
  PegPessOrdering *orderings;
  int n_orderings;
  int capacity;
} PessMaterializeCtx;

static void peg_pess_materialize_cb(const MachineLetter *draw, int n,
                                    int64_t weight, void *user) {
  PessMaterializeCtx *mc = (PessMaterializeCtx *)user;
  if (mc->n_orderings >= mc->capacity) {
    log_fatal("pessfull: ordering buffer too small (cap=%d)", mc->capacity);
  }
  PegPessOrdering *o = &mc->orderings[mc->n_orderings++];
  for (int i = 0; i < n; i++) {
    o->draw[i] = draw[i];
  }
  o->n = n;
  o->weight = weight;
  o->opp_top_score = 0;
  o->outcome = PEG_OUT_DRAW;
}

// Build the scenario for a given ordering. Caller owns the returned Game.
static Game *peg_pess_build_scenario(const Game *base_game,
                                     const PegPessOrdering *o,
                                     const uint8_t *unseen, int ld_size,
                                     int opp_idx, const Move *cand) {
  Game *scenario = game_duplicate(base_game);
  game_set_backup_mode(scenario, BACKUP_MODE_OFF);
  // Deterministically set the bag to exactly the ordering's tiles (no PRNG).
  Bag *bag = game_get_bag(scenario);
  bag_set_to_tiles(bag, o->draw, o->n);
  Rack *opp_rack = player_get_rack(game_get_player(scenario, opp_idx));
  rack_reset(opp_rack);
  uint8_t leftover[MAX_ALPHABET_SIZE];
  memcpy(leftover, unseen, (size_t)ld_size);
  for (int i = 0; i < o->n; i++) {
    leftover[o->draw[i]]--;
  }
  for (int ml = 0; ml < ld_size; ml++) {
    for (int k = 0; k < (int)leftover[ml]; k++) {
      rack_add_letter(opp_rack, (MachineLetter)ml);
    }
  }
  play_move(cand, scenario, NULL);
  game_set_game_end_reason(scenario, GAME_END_REASON_NONE);
  return scenario;
}

// Solver-scratch arena. Decouples scratch (solver state, eg_ctx, eg_results,
// ml_pool, eg_scratch) from worker_idx so a task can acquire a FRESH slot on
// entry and release on exit. This is the safety foundation for recursive
// splitting: the pool's help-while-waiting means a worker can run a nested
// task while its own frame is suspended — both must use DIFFERENT scratch, or
// they corrupt each other. Acquiring per-task (not per-pthread) guarantees
// that. Slots' ml_pool/eg_scratch persist across acquisitions (reused scratch);
// per-task counters are reset by the caller. Sized larger than the max number
// of concurrently-active tasks (n_threads * (fork_depth+1) + margin).
struct PessSlotArena {
  int n_slots;
  int *free_stack; // indices of currently-free slots
  int free_top;    // count of free slots on the stack
  cpthread_mutex_t mutex;
};

// Returns a free slot index, or -1 if exhausted (caller must handle; sizing
// should make this impossible for the configured fork depth).
static int pess_arena_acquire(PessSlotArena *a) {
  cpthread_mutex_lock(&a->mutex);
  const int idx = (a->free_top > 0) ? a->free_stack[--a->free_top] : -1;
  cpthread_mutex_unlock(&a->mutex);
  return idx;
}

static void pess_arena_release(PessSlotArena *a, int idx) {
  cpthread_mutex_lock(&a->mutex);
  a->free_stack[a->free_top++] = idx;
  cpthread_mutex_unlock(&a->mutex);
}

// Current free-slot count (snapshot under the mutex). The fork/parallel gates
// use this to decide whether there's headroom to spawn more sub-tasks: forking
// makes a frame hold its slot while help-draining nested tasks (each holding
// theirs), so demand tracks fork nesting. Gating on free slots >= num_workers
// self-balances — deep nesting is fine when slots are plentiful (the drained
// tail), and forking throttles as slots deplete under load (no exhaustion),
// without a fixed nesting cap that over-constrains the tail.
static int pess_arena_free(PessSlotArena *a) {
  cpthread_mutex_lock(&a->mutex);
  const int n = a->free_top;
  cpthread_mutex_unlock(&a->mutex);
  return n;
}

// Per-worker job + state shared by ordering. Each worker also owns its own
// EndgameCtx + EndgameResults to avoid races inside endgame_solve_inline.
// Fields are grouped for readability over packing; only a handful of these are
// allocated, so the padding is immaterial.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct PessJob {
  const Game *base_game;
  const Move *cand;
  const uint8_t *unseen;
  int ld_size;
  int mover_idx;
  int opp_idx;
  ThreadControl *thread_control;
  int endgame_plies;
  double endgame_time;
  double tt_fraction_of_mem;
  TranspositionTable *shared_tt; // if set, all workers share this TT
  int opp_sort_mode;
  int mover_sort_mode;
  bool subperm_sort;
  bool recursive_split;   // fork opp nodes across the pool (split path only)
  bool force_nested_perm; // debug: bypass nested-perm queue gate
  PegPool *pool;          // set in the split path; needed for recursive forking
  double slow_solve_log_s;
  double idle_probe_s;  // rung-5 instrumentation threshold (0=off)
  double rung4_probe_s; // rung-4 instrumentation threshold (0=off)
  bool first_win;
  int endgame_threads;
  bool skip_word_pruning;
  int max_opp_k;
  PegPessOrdering *ordering; // start of contiguous slice this job owns
  int n_orderings;           // 1 = single, >1 = group of consecutive orderings
  // Per-worker scratch (each worker_idx gets its own slot).
  EndgameCtx **eg_ctxs; // length = num_workers + 1 (helper)
  EndgameResults **eg_results;
  PegPessSolver *solvers; // length = num_workers + 1 (helper)
  int n_worker_slots;
  // Scratch arena (split path only). When set, the split worker acquires a
  // slot from here instead of worker_idx→slot, so recursive forking can run
  // nested tasks on distinct slots without colliding with a suspended frame.
  PessSlotArena *arena;
  // Shared atomic counters for diagnostics.
  atomic_long *shared_n_solves;
  atomic_long *shared_n_leaf_visits;
  atomic_long *shared_n_recursive;
  atomic_long *shared_n_loss_cutoffs;
  atomic_long *shared_n_win_cutoffs;
  atomic_long *shared_n_nested_calls;
  PegPessCache *cache;          // shared endgame-state cache (may be NULL)
  PegNestedCache *nested_cache; // shared nested verdict-map cache (may be NULL)
  bool nested_our_turn;
  int nested_depth_limit;
  int nested_mover_k;
  // Per-ordering progress logging.
  atomic_int *shared_orderings_done;
  int total_orderings;
  // Per-worker telemetry. Indexed by slot (= worker_idx - 100, or n_slots-1
  // for helper). Each worker accumulates into its own slot — no contention.
  int64_t *per_worker_orderings;
  int64_t *per_worker_solves;
  int64_t *per_worker_recursive;
  int64_t *per_worker_nested;
  int64_t *per_worker_busy_ns; // wall ns spent in worker_fn body
};

// Map worker_idx → slot. peg_pool hands in worker_idx in
// [thread_index_offset, thread_index_offset + num_workers); the calling thread
// uses helper_worker_idx (we map all worker_idxs into the per-worker scratch
// array with modulo).
static int peg_pess_worker_slot(const PessJob *j, int worker_idx) {
  // worker_idxs come from the pool [100, 100+num_workers) or the helper (=0);
  // map negative or out-of-range to 0.
  int slot = worker_idx - 100;
  if (slot < 0 || slot >= j->n_worker_slots) {
    slot = j->n_worker_slots - 1; // last slot = helper
  }
  return slot;
}

// Initialize a worker's solver scratch slot from the shared job config. Shared
// by worker_fn and the opp-split worker so neither can miss a field when
// PessJob grows. Does NOT touch per-call counters (n_*) or game/nested_depth —
// caller resets those per ordering/unit.
static void peg_pess_init_solver_from_job(PegPessSolver *solver,
                                          const PessJob *j, int slot) {
  solver->mover_idx = j->mover_idx;
  solver->opp_idx = j->opp_idx;
  solver->ld_size = j->ld_size;
  solver->thread_control = j->thread_control;
  solver->endgame_plies = j->endgame_plies;
  solver->endgame_time = j->endgame_time;
  solver->tt_fraction_of_mem = j->tt_fraction_of_mem;
  solver->shared_tt = j->shared_tt;
  solver->opp_sort_mode = j->opp_sort_mode;
  solver->mover_sort_mode = j->mover_sort_mode;
  solver->subperm_sort = j->subperm_sort;
  solver->slow_solve_log_s = j->slow_solve_log_s;
  solver->idle_probe_s = j->idle_probe_s;
  solver->rung4_probe_s = j->rung4_probe_s;
  solver->first_win = j->first_win;
  solver->endgame_threads = j->endgame_threads;
  solver->skip_word_pruning = j->skip_word_pruning;
  solver->max_opp_k = j->max_opp_k;
  solver->nested_our_turn = j->nested_our_turn;
  solver->nested_depth_limit = j->nested_depth_limit;
  solver->nested_mover_k = j->nested_mover_k;
  solver->eg_ctx_p = &j->eg_ctxs[slot];
  solver->eg_results = j->eg_results[slot];
  solver->cache = j->cache;
  solver->nested_cache = j->nested_cache;
  // Recursive-split forking context. fork_depth defaults to 0 (top-level task);
  // the fork worker overrides it for nested sub-tasks.
  solver->job = j;
  solver->pool = j->pool;
  solver->arena = j->arena;
  solver->recursive_split = j->recursive_split;
  solver->force_nested_perm = j->force_nested_perm;
  solver->fork_depth = 0;
}

static void peg_pess_worker_fn(void *arg, int worker_idx) {
  PessJob *j = (PessJob *)arg;
  const int slot = peg_pess_worker_slot(j, worker_idx);
  PegPessSolver *solver = &j->solvers[slot];
  peg_pess_init_solver_from_job(solver, j, slot);

  // Iterate over the orderings in this job's slice. With n_orderings=1 this
  // is the legacy "one ordering per job" path. With n_orderings>1 the same
  // worker handles all orderings in the slice sequentially, which keeps
  // their endgame TT entries hot across orderings within the same group.
  for (int slice_idx = 0; slice_idx < j->n_orderings; slice_idx++) {
    PegPessOrdering *o = &j->ordering[slice_idx];
    solver->game = NULL;
    solver->nested_depth = 0;
    solver->n_endgame_solves = 0;
    solver->n_leaf_visits = 0;
    solver->n_recursive_calls = 0;
    solver->n_first_loss_cutoffs = 0;
    solver->n_first_win_cutoffs = 0;
    solver->n_nested_calls = 0;

    Timer ordering_t;
    ctimer_start(&ordering_t);

    Game *scenario = peg_pess_build_scenario(j->base_game, o, j->unseen,
                                             j->ld_size, j->opp_idx, j->cand);
    solver->game = scenario;
    o->outcome = peg_pess_recursive_solve(solver);
    game_destroy(scenario);

    const double ordering_wall = ctimer_elapsed_seconds(&ordering_t);
    const int oi_done = atomic_fetch_add(j->shared_orderings_done, 1) + 1;
    char draw_str[24] = {0};
    int do_len = 0;
    for (int i = 0; i < o->n && do_len + 3 < (int)sizeof(draw_str); i++) {
      draw_str[do_len++] = (char)('0' + (o->draw[i] / 10));
      draw_str[do_len++] = (char)('0' + (o->draw[i] % 10));
      if (i + 1 < o->n) {
        draw_str[do_len++] = '_';
      }
    }
    const char *outcome_label = "DRAW";
    if (o->outcome == PEG_OUT_WIN) {
      outcome_label = "WIN";
    } else if (o->outcome == PEG_OUT_LOSS) {
      outcome_label = "LOSS";
    }
    (void)fprintf(
        stderr,
        "[pessfull] ord %3d/%d  outcome=%s  wall=%.2fs  recursive=%lld  "
        "solves=%lld  nested=%lld  worker=%d  draw=%s\n",
        oi_done, j->total_orderings, outcome_label, ordering_wall,
        (long long)solver->n_recursive_calls,
        (long long)solver->n_endgame_solves, (long long)solver->n_nested_calls,
        worker_idx, draw_str);
    (void)fflush(stderr);

    atomic_fetch_add(j->shared_n_solves, solver->n_endgame_solves);
    atomic_fetch_add(j->shared_n_leaf_visits, solver->n_leaf_visits);
    atomic_fetch_add(j->shared_n_recursive, solver->n_recursive_calls);
    atomic_fetch_add(j->shared_n_loss_cutoffs, solver->n_first_loss_cutoffs);
    atomic_fetch_add(j->shared_n_win_cutoffs, solver->n_first_win_cutoffs);
    atomic_fetch_add(j->shared_n_nested_calls, solver->n_nested_calls);

    j->per_worker_orderings[slot]++;
    j->per_worker_solves[slot] += solver->n_endgame_solves;
    j->per_worker_recursive[slot] += solver->n_recursive_calls;
    j->per_worker_nested[slot] += solver->n_nested_calls;
    j->per_worker_busy_ns[slot] += (int64_t)(ordering_wall * 1.0e9);
  }
}

// Pre-compute opp's top-score response for a given ordering. Single-threaded;
// runs before the parallel solve. Used to sort orderings (hardest first) so
// that first-loss style cutoffs fire sooner for callers that want them.
static int peg_pess_compute_opp_top_score(const Game *base_game,
                                          const PegPessOrdering *o,
                                          const uint8_t *unseen, int ld_size,
                                          int opp_idx, const Move *cand) {
  Game *scenario =
      peg_pess_build_scenario(base_game, o, unseen, ld_size, opp_idx, cand);
  MoveList *ml = move_list_create(16384);
  const MoveGenArgs ga = {
      .game = scenario,
      .move_list = ml,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&ga);
  int top = 0;
  const int n = move_list_get_count(ml);
  for (int i = 0; i < n; i++) {
    const Move *m = move_list_get_move(ml, i);
    if (move_get_type(m) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      const int s = equity_to_int(move_get_score(m));
      if (s > top) {
        top = s;
      }
    }
  }
  move_list_destroy(ml);
  game_destroy(scenario);
  return top;
}

// ---- Opp-split parallelism (PASSPEG_PESSFULL_SPLIT_OPP) --------------------
// build_scenario plays our cand P, leaving opp to move. An ordering's outcome
// is the min (worst-for-us; LOSS=0<DRAW=1<WIN=2) over opp replies, with a
// first-loss short-circuit. We make each (ordering, opp-reply) its own peg_pool
// work unit: a single ordering then fans its replies across cores, and a full
// solve pools all replies into one fine-grained queue (better load balance than
// 1 unit per ordering). Each unit runs on its own pthread (own TLS movegen +
// own solver scratch slot) and recurses sequentially within its subtree.
typedef struct PessSplitAgg {
  atomic_int worst;    // min PegPessOut seen so far (init WIN)
  atomic_int has_loss; // set once any reply is LOSS → cancel siblings
} PessSplitAgg;

typedef struct PessSplitUnit {
  PessJob *job; // shared config (solver setup, base game, counters)
  PegPessOrdering *ordering; // the ordering this reply belongs to
  PessSplitAgg *agg;         // per-ordering aggregation to fold into
  int reply_idx;             // sorted opp-reply index; -1 = whole ordering
} PessSplitUnit;

// Atomic min-fold of an outcome into an ordering's aggregate.
static void peg_pess_split_fold(PessSplitAgg *agg, PegPessOut out) {
  int cur = atomic_load(&agg->worst);
  while ((int)out < cur) {
    if (atomic_compare_exchange_weak(&agg->worst, &cur, (int)out)) {
      break;
    }
  }
  if (out == PEG_OUT_LOSS) {
    atomic_store(&agg->has_loss, 1);
  }
}

// Count an ordering's splittable opp replies. Returns the number of opp-reply
// work units to create (cand_n), or 0 if the post-P position is terminal /
// bag-empty (→ caller makes a single whole-ordering unit, reply_idx=-1).
static int peg_pess_count_opp_replies(const PessJob *j,
                                      const PegPessOrdering *o) {
  Game *scenario = peg_pess_build_scenario(j->base_game, o, j->unseen,
                                           j->ld_size, j->opp_idx, j->cand);
  if (game_get_game_end_reason(scenario) != GAME_END_REASON_NONE ||
      bag_get_letters(game_get_bag(scenario)) == 0) {
    game_destroy(scenario);
    return 0;
  }
  MoveList *ml = move_list_create(16384);
  const MoveGenArgs ga = {
      .game = scenario,
      .move_list = ml,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&ga);
  const int n_moves = move_list_get_count(ml);
  int cand_n = n_moves;
  if (j->max_opp_k > 0 && j->max_opp_k < cand_n) {
    cand_n = j->max_opp_k;
  }
  move_list_destroy(ml);
  game_destroy(scenario);
  return cand_n;
}

static void peg_pess_split_worker_fn(void *arg, int worker_idx) {
  PessSplitUnit *u = (PessSplitUnit *)arg;
  PessJob *j = u->job;
  // Cancel: a sibling reply already proved a loss → the ordering is LOSS, so
  // this reply can't change the verdict. (Verdict-safe: min stays LOSS.)
  if (atomic_load(&u->agg->has_loss)) {
    return;
  }

  // Acquire a scratch slot from the arena (not worker_idx→slot): under the
  // pool's help-while-waiting, this task may run while another frame on this
  // pthread is suspended, so it needs its own slot. Falls back to the
  // worker_idx mapping if no arena (shouldn't happen in split mode).
  const int slot = j->arena ? pess_arena_acquire(j->arena)
                            : peg_pess_worker_slot(j, worker_idx);
  if (slot < 0) {
    log_fatal("pessfull: scratch arena exhausted (increase arena size)");
  }
  PegPessSolver *solver = &j->solvers[slot];
  peg_pess_init_solver_from_job(solver, j, slot);
  solver->nested_depth = 0;
  solver->n_endgame_solves = 0;
  solver->n_leaf_visits = 0;
  solver->n_recursive_calls = 0;
  solver->n_first_loss_cutoffs = 0;
  solver->n_first_win_cutoffs = 0;
  solver->n_nested_calls = 0;

  Game *scenario = peg_pess_build_scenario(j->base_game, u->ordering, j->unseen,
                                           j->ld_size, j->opp_idx, j->cand);
  PegPessOut out;
  if (u->reply_idx < 0) {
    // Whole-ordering unit: post-P position is terminal/bag-empty (no opp node).
    solver->game = scenario;
    out = peg_pess_recursive_solve(solver);
  } else {
    // Generate + sort opp replies exactly as recursive_solve would, then play
    // only this unit's reply and solve the resulting (our-turn) subtree.
    MoveList *ml = move_list_create(16384);
    const MoveGenArgs ga = {
        .game = scenario,
        .move_list = ml,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&ga);
    const int n_moves = move_list_get_count(ml);
    int *order = malloc_or_die((size_t)n_moves * sizeof(int));
    const int cand_n =
        peg_pess_sort_order(solver, ml, n_moves, /*our_turn=*/false, order);
    if (u->reply_idx >= cand_n) {
      // Defensive: reply count shrank since the count pre-pass; nothing to do.
      free(order);
      move_list_destroy(ml);
      game_destroy(scenario);
      if (j->arena) {
        pess_arena_release(j->arena, slot);
      }
      return;
    }
    const Move *reply = move_list_get_move(ml, order[u->reply_idx]);
    // opp plays its reply (draws deterministically via bag_set_to_tiles bag);
    // do NOT reset game_end_reason — a game-ending reply must reach base_case,
    // exactly as the whole-ordering recursive_solve opp loop would see it.
    play_move(reply, scenario, NULL);
    solver->game = scenario;
    out = peg_pess_recursive_solve(solver);
    free(order);
    move_list_destroy(ml);
  }
  game_destroy(scenario);
  peg_pess_split_fold(u->agg, out);

  atomic_fetch_add(j->shared_n_solves, solver->n_endgame_solves);
  atomic_fetch_add(j->shared_n_leaf_visits, solver->n_leaf_visits);
  atomic_fetch_add(j->shared_n_recursive, solver->n_recursive_calls);
  atomic_fetch_add(j->shared_n_loss_cutoffs, solver->n_first_loss_cutoffs);
  atomic_fetch_add(j->shared_n_win_cutoffs, solver->n_first_win_cutoffs);
  atomic_fetch_add(j->shared_n_nested_calls, solver->n_nested_calls);

  // Progress: log every 5000 completed units (reuses shared_orderings_done as
  // a units-done counter in split mode; total is printed in the dispatch line).
  const int done = atomic_fetch_add(j->shared_orderings_done, 1) + 1;
  if (done % 5000 == 0) {
    (void)fprintf(stderr, "[pessfull] split progress: %d / %d units\n", done,
                  j->total_orderings);
    (void)fflush(stderr);
  }

  if (j->arena) {
    pess_arena_release(j->arena, slot);
  }
}

// One forked opp-reply sub-task: solve `game` (the parent opp position with one
// reply already played) and fold its outcome into the shared aggregate.
typedef struct PessForkUnit {
  const PessJob *job;
  PessSplitAgg *agg;
  Game *game;       // owned: this position, one opp reply played; freed here
  int fork_depth;   // depth of THIS sub-task (parent fork_depth + 1)
  int nested_depth; // parent opp node's nested_depth, preserved for correctness
} PessForkUnit;

static void peg_pess_fork_worker_fn(void *arg, int worker_idx) {
  (void)worker_idx;
  PessForkUnit *u = (PessForkUnit *)arg;
  const PessJob *j = u->job;
  if (atomic_load(&u->agg->has_loss)) {
    game_destroy(u->game);
    return;
  }
  const int slot = pess_arena_acquire(j->arena);
  if (slot < 0) {
    log_fatal("pessfull: scratch arena exhausted in fork (increase size)");
  }
  PegPessSolver *solver = &j->solvers[slot];
  peg_pess_init_solver_from_job(solver, j, slot);
  solver->fork_depth = u->fork_depth;
  solver->nested_depth = u->nested_depth; // preserve parent's nested depth
  solver->n_endgame_solves = 0;
  solver->n_leaf_visits = 0;
  solver->n_recursive_calls = 0;
  solver->n_first_loss_cutoffs = 0;
  solver->n_first_win_cutoffs = 0;
  solver->n_nested_calls = 0;
  solver->game = u->game;
  const PegPessOut out = peg_pess_recursive_solve(solver);

  atomic_fetch_add(j->shared_n_solves, solver->n_endgame_solves);
  atomic_fetch_add(j->shared_n_leaf_visits, solver->n_leaf_visits);
  atomic_fetch_add(j->shared_n_recursive, solver->n_recursive_calls);
  atomic_fetch_add(j->shared_n_loss_cutoffs, solver->n_first_loss_cutoffs);
  atomic_fetch_add(j->shared_n_win_cutoffs, solver->n_first_win_cutoffs);
  atomic_fetch_add(j->shared_n_nested_calls, solver->n_nested_calls);

  game_destroy(u->game);
  pess_arena_release(j->arena, slot);
  peg_pess_split_fold(u->agg, out);
}

// Parallel min-reduction over opp replies at an opp node. Duplicates the game
// per reply, plays it, and dispatches each as a fork sub-task. Same verdict as
// the sequential opp loop (min over replies, LOSS<DRAW<WIN); first-loss cancel
// via the shared aggregate. The caller owns/frees `order` and `ml`.
static PegPessOut peg_pess_fork_opp_node(PegPessSolver *s, const MoveList *ml,
                                         const int *order, int cand_n) {
  const PessJob *j = s->job;
  const Game *g = s->game;
  PessSplitAgg agg;
  atomic_init(&agg.worst, PEG_OUT_WIN);
  atomic_init(&agg.has_loss, 0);

  PessForkUnit *units = malloc_or_die((size_t)cand_n * sizeof(PessForkUnit));
  void **ptrs = malloc_or_die((size_t)cand_n * sizeof(void *));
  for (int mi = 0; mi < cand_n; mi++) {
    const Move *m = move_list_get_move(ml, order[mi]);
    Game *child = game_duplicate(g);
    game_set_backup_mode(child, BACKUP_MODE_OFF);
    // opp plays its reply (draws deterministically); do NOT reset
    // game_end_reason — a game-ending reply must reach base_case, exactly as
    // the sequential opp loop sees it.
    play_move(m, child, NULL);
    units[mi] = (PessForkUnit){.job = j,
                               .agg = &agg,
                               .game = child,
                               .fork_depth = s->fork_depth + 1,
                               .nested_depth = s->nested_depth};
    ptrs[mi] = &units[mi];
  }
  // Nested submit on the shared pool. Help-while-waiting drains other tasks
  // (each on its own arena slot), so this thread never pure-blocks → no
  // deadlock. pool is guaranteed non-NULL here (recursive_split is only set in
  // the multithreaded split path). Bump the C-stack nesting counter so tasks
  // help-drained during this wait observe the deeper level and stop forking at
  // the stack-safety cap.
  g_peg_fork_nesting++;
  peg_pool_submit_and_wait(j->pool, peg_pess_fork_worker_fn, ptrs, cand_n,
                           /*helper_worker_idx=*/0);
  g_peg_fork_nesting--;
  const PegPessOut out = (PegPessOut)atomic_load(&agg.worst);
  free(units);
  free(ptrs);
  return out;
}

// One nested sub-perm sub-task: run the (cand, perm) scenario and record its
// outcome + fold its weight into the cand's shared score/loss accumulators.
typedef struct PessPermUnit {
  const PessJob *job;
  Game *base_game;   // nested node game; run_scenario duplicates it (RO)
  MoveList *ml;      // parent's move list (read-only, shared)
  int cand_move_idx; // order[ci]
  const uint8_t *unseen;
  const MachineLetter *draw;
  int draw_n;
  int64_t weight;
  int nested_depth;     // preserve the nested node's depth
  PegPessOut *out_slot; // distinct per task — no race
  atomic_llong *score;  // 2*wins + draws
  atomic_llong *losses; // weighted losses
} PessPermUnit;

static void peg_pess_perm_worker_fn(void *arg, int worker_idx) {
  (void)worker_idx;
  PessPermUnit *u = (PessPermUnit *)arg;
  const PessJob *j = u->job;
  const int slot = pess_arena_acquire(j->arena);
  if (slot < 0) {
    log_fatal("pessfull: scratch arena exhausted in perm fork (increase size)");
  }
  PegPessSolver *solver = &j->solvers[slot];
  peg_pess_init_solver_from_job(solver, j, slot);
  solver->nested_depth = u->nested_depth;
  solver->fork_depth = 0; // fresh subtree root; opp nodes may fork under cap
  solver->n_endgame_solves = 0;
  solver->n_leaf_visits = 0;
  solver->n_recursive_calls = 0;
  solver->n_first_loss_cutoffs = 0;
  solver->n_first_win_cutoffs = 0;
  solver->n_nested_calls = 0;

  const PegPessOut out = peg_pess_nested_run_scenario(
      solver, u->base_game, u->ml, u->cand_move_idx, u->unseen, u->draw,
      u->draw_n);
  *u->out_slot = out;
  if (out == PEG_OUT_WIN) {
    atomic_fetch_add(u->score, 2 * u->weight);
  } else if (out == PEG_OUT_DRAW) {
    atomic_fetch_add(u->score, u->weight);
  } else {
    atomic_fetch_add(u->losses, u->weight);
  }

  atomic_fetch_add(j->shared_n_solves, solver->n_endgame_solves);
  atomic_fetch_add(j->shared_n_leaf_visits, solver->n_leaf_visits);
  atomic_fetch_add(j->shared_n_recursive, solver->n_recursive_calls);
  atomic_fetch_add(j->shared_n_loss_cutoffs, solver->n_first_loss_cutoffs);
  atomic_fetch_add(j->shared_n_win_cutoffs, solver->n_first_win_cutoffs);
  atomic_fetch_add(j->shared_n_nested_calls, solver->n_nested_calls);

  pess_arena_release(j->arena, slot);
}

static void peg_pess_parallel_perms(
    const PegPessSolver *s, MoveList *ml, int cand_move_idx,
    const uint8_t *unseen, const NestedPermCollect *pc,
    // out_arr elements are written via &out_arr[pi] stored in worker units;
    // the indirect store is invisible to clang-tidy.
    // NOLINTNEXTLINE(readability-non-const-parameter)
    PegPessOut *out_arr, int64_t *cand_score, int64_t *cand_losses) {
  const PessJob *j = s->job;
  atomic_llong score;
  atomic_llong losses;
  atomic_init(&score, 0);
  atomic_init(&losses, 0);
  PessPermUnit *units = malloc_or_die((size_t)pc->count * sizeof(PessPermUnit));
  void **ptrs = malloc_or_die((size_t)pc->count * sizeof(void *));
  for (int pi = 0; pi < pc->count; pi++) {
    units[pi] = (PessPermUnit){.job = j,
                               .base_game = s->game,
                               .ml = ml,
                               .cand_move_idx = cand_move_idx,
                               .unseen = unseen,
                               .draw = pc->perms[pi].draw,
                               .draw_n = pc->perms[pi].n,
                               .weight = pc->perms[pi].weight,
                               .nested_depth = s->nested_depth,
                               .out_slot = &out_arr[pi],
                               .score = &score,
                               .losses = &losses};
    ptrs[pi] = &units[pi];
  }
  g_peg_fork_nesting++;
  peg_pool_submit_and_wait(j->pool, peg_pess_perm_worker_fn, ptrs, pc->count,
                           /*helper_worker_idx=*/0);
  g_peg_fork_nesting--;
  *cand_score = (int64_t)atomic_load(&score);
  *cand_losses = (int64_t)atomic_load(&losses);
  free(units);
  free(ptrs);
}

void test_pass_peg_pessimistic_full_eval(void) {
  const char *cgp = getenv("PASSPEG_PESSFULL_CGP");
  if (!cgp || !*cgp) {
    log_fatal("PASSPEG_PESSFULL_CGP must be set");
  }
  const char *move_str = getenv("PASSPEG_PESSFULL_MOVE");
  if (!move_str || !*move_str) {
    log_fatal("PASSPEG_PESSFULL_MOVE must be set");
  }
  const char *plies_env = getenv("PASSPEG_PESSFULL_PLIES");
  const int plies =
      plies_env && *plies_env ? passpeg_str_to_int(plies_env) : 12;
  const char *time_env = getenv("PASSPEG_PESSFULL_TIME");
  const double per_solve_time =
      time_env && *time_env ? passpeg_str_to_double(time_env) : 5.0;
  const char *opp_k_env = getenv("PASSPEG_PESSFULL_MAX_OPP_K");
  const int max_opp_k =
      opp_k_env && *opp_k_env ? passpeg_str_to_int(opp_k_env) : 0;
  // Endgame TT size in MB. Default 1024 MB. 0 = disabled.
  const char *tt_env = getenv("PASSPEG_PESSFULL_TT_MB");
  const int tt_mb = tt_env && *tt_env ? passpeg_str_to_int(tt_env) : 1024;
  const uint64_t total_mem = get_total_memory();
  const double tt_fraction_of_mem =
      tt_mb > 0 ? ((double)tt_mb * 1024.0 * 1024.0) / (double)total_mem : 0.0;
  // PASSPEG_PESSFULL_TT_SHARED: when set, allocate ONE TT of tt_mb total and
  // share it across all workers (lockless). Otherwise each worker gets its
  // own tt_mb-sized TT. Shared captures cross-ordering reuse without tying
  // it to worker assignment, so per-ordering dispatch stays load-balanced.
  const char *tt_shared_env = getenv("PASSPEG_PESSFULL_TT_SHARED");
  const bool tt_shared = tt_shared_env && passpeg_str_to_int(tt_shared_env) > 0;
  TranspositionTable *shared_tt =
      (tt_shared && tt_mb > 0) ? transposition_table_create(tt_fraction_of_mem)
                               : NULL;
  // Move-ordering keys (0=score default, 1=equity, 2=tiles-then-score,
  // 3=tiles-then-equity), nested sub-perm sort, word-prune skip, first_win,
  // slow-solve CGP logging. All verdict-invariant (skip_word_pruning has no
  // verdict effect).
  const char *opp_sort_env = getenv("PASSPEG_PESSFULL_OPP_SORT");
  const int opp_sort_mode = opp_sort_env ? passpeg_str_to_int(opp_sort_env) : 0;
  const char *mover_sort_env = getenv("PASSPEG_PESSFULL_MOVER_SORT");
  const int mover_sort_mode =
      mover_sort_env ? passpeg_str_to_int(mover_sort_env) : 0;
  const char *subperm_env = getenv("PASSPEG_PESSFULL_SUBPERM_SORT");
  const bool subperm_sort = subperm_env && passpeg_str_to_int(subperm_env) > 0;
  const char *skip_wp_env = getenv("PASSPEG_PESSFULL_SKIP_WORD_PRUNE");
  const bool skip_word_pruning =
      skip_wp_env && passpeg_str_to_int(skip_wp_env) > 0;
  const char *slow_env = getenv("PASSPEG_PESSFULL_SLOW_SOLVE_S");
  const double slow_solve_log_s =
      slow_env ? passpeg_str_to_double(slow_env) : 0.0;
  // Rung-5 probe threshold: leaf solves exceeding this many seconds sample the
  // pool idle count (see g_probe_* counters). 0 = off.
  const char *idle_probe_env = getenv("PASSPEG_PESSFULL_IDLE_PROBE_S");
  const double idle_probe_s =
      idle_probe_env ? passpeg_str_to_double(idle_probe_env) : 0.0;
  atomic_store(&g_probe_slow_solves, 0);
  atomic_store(&g_probe_slow_idle_sum, 0);
  atomic_store(&g_probe_slow_with_idle, 0);
  // Rung-4 probe threshold: nested cand loops exceeding this many seconds
  // sample the pool idle count (see g_rung4_* counters). 0 = off.
  const char *rung4_probe_env = getenv("PASSPEG_PESSFULL_RUNG4_PROBE_S");
  const double rung4_probe_s =
      rung4_probe_env ? passpeg_str_to_double(rung4_probe_env) : 0.0;
  atomic_store(&g_rung4_slow, 0);
  atomic_store(&g_rung4_slow_seq, 0);
  atomic_store(&g_rung4_seq_idle_sum, 0);
  atomic_store(&g_rung4_seq_with_idle, 0);
  atomic_store(&g_rung4_seq_cands_sum, 0);
  // SPLIT_OPP: dispatch each (ordering, opp-reply) as its own peg_pool unit
  // instead of one unit per ordering. Finer-grained parallelism — a single
  // ordering fans across cores; a full solve gets better load balance.
  const char *split_opp_env = getenv("PASSPEG_PESSFULL_SPLIT_OPP");
  const bool split_opp = split_opp_env && passpeg_str_to_int(split_opp_env) > 0;
  // RECURSIVE_SPLIT: additionally fork deep opp nodes inside each unit across
  // the pool (needs SPLIT_OPP + a pool). Default off.
  const char *rsplit_env = getenv("PASSPEG_PESSFULL_RECURSIVE_SPLIT");
  const bool recursive_split = rsplit_env && passpeg_str_to_int(rsplit_env) > 0;
  // Debug: force nested-perm parallelism on (bypass the queue gate). For
  // exercising the path under the thread sanitizer; not for production runs.
  const char *force_np_env = getenv("PASSPEG_PESSFULL_FORCE_NESTED_PERM");
  const bool force_nested_perm =
      force_np_env && passpeg_str_to_int(force_np_env) > 0;
  // first_win: endgame solves use a narrow [-1,+1] window (sign only). Correct
  // for guaranteed-win (we only consume the sign) and verdict-invariant.
  const char *first_win_env = getenv("PASSPEG_PESSFULL_FIRST_WIN");
  const bool first_win = first_win_env && passpeg_str_to_int(first_win_env) > 0;
  // Per-solve endgame thread count. Default 1. For single-ordering (ONLY_DRAW)
  // investigation, set >1 to parallelize each endgame solve across cores.
  const char *eg_threads_env = getenv("PASSPEG_PESSFULL_ENDGAME_THREADS");
  const int endgame_threads =
      eg_threads_env && passpeg_str_to_int(eg_threads_env) > 0
          ? passpeg_str_to_int(eg_threads_env)
          : 1;

  Config *config = config_create_or_die("set -s1 score -s2 score");
  char load_cmd[10240];
  (void)snprintf(load_cmd, sizeof(load_cmd), "cgp %s", cgp);
  load_and_exec_config_or_die(config, load_cmd);
  Game *game = config_get_game(config);

  const int mover_idx = game_get_player_on_turn_index(game);
  const int opp_idx = 1 - mover_idx;
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  const int bag_size = bag_get_letters(game_get_bag(game));

  uint8_t unseen[MAX_ALPHABET_SIZE] = {0};
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] = (uint8_t)ld_get_dist(ld, ml);
  }
  const Rack *mr = player_get_rack(game_get_player(game, mover_idx));
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] -= (uint8_t)rack_get_letter(mr, (MachineLetter)ml);
  }
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      MachineLetter on_board = board_get_letter(board, row, col);
      if (on_board == ALPHABET_EMPTY_SQUARE_MARKER) {
        continue;
      }
      MachineLetter eff =
          get_is_blanked(on_board) ? BLANK_MACHINE_LETTER : on_board;
      if (unseen[eff] > 0) {
        unseen[eff]--;
      }
    }
  }

  int total_unseen = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    total_unseen += unseen[ml];
  }
  if (total_unseen != RACK_SIZE + bag_size) {
    log_fatal("pessfull: expected %d unseen, got %d", RACK_SIZE + bag_size,
              total_unseen);
  }

  ErrorStack *parse_err = error_stack_create();
  ValidatedMoves *vms =
      validated_moves_create(game, mover_idx, move_str, false, true, parse_err);
  if (!error_stack_is_empty(parse_err)) {
    log_fatal("pessfull: failed to parse move %s", move_str);
  }
  const Move *move = validated_moves_get_move(vms, 0);

  MachineLetter tile_types[MAX_ALPHABET_SIZE] = {0};
  int tile_counts[MAX_ALPHABET_SIZE];
  int num_types = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    if (unseen[ml] > 0) {
      tile_types[num_types] = (MachineLetter)ml;
      tile_counts[num_types] = (int)unseen[ml];
      num_types++;
    }
  }

  const char *tt_mode_label = "off";
  if (shared_tt) {
    tt_mode_label = "shared";
  } else if (tt_mb > 0) {
    tt_mode_label = "per-worker";
  }
  (void)fprintf(stderr,
                "[pessfull] move=%s plies=%d soft_time=%.1fs bag=%d unseen=%d "
                "max_opp_k=%d tt_mb=%d (frac=%.6f) tt_mode=%s\n",
                move_str, plies, per_solve_time, bag_size, total_unseen,
                max_opp_k, tt_mb, tt_fraction_of_mem, tt_mode_label);

  const char *threads_env = getenv("PASSPEG_PESSFULL_THREADS");
  const int n_threads =
      threads_env && *threads_env ? passpeg_str_to_int(threads_env) : 18;
  const char *sort_env = getenv("PASSPEG_PESSFULL_SORT");
  const bool do_sort =
      sort_env && *sort_env ? passpeg_str_to_int(sort_env) > 0 : true;
  const char *nested_env = getenv("PASSPEG_PESSFULL_NESTED");
  const bool nested_our_turn =
      nested_env && *nested_env ? passpeg_str_to_int(nested_env) > 0 : false;
  const char *nested_depth_env = getenv("PASSPEG_PESSFULL_NESTED_DEPTH");
  const int nested_depth_limit = nested_depth_env && *nested_depth_env
                                     ? passpeg_str_to_int(nested_depth_env)
                                     : 1;
  const char *nested_k_env = getenv("PASSPEG_PESSFULL_NESTED_K");
  const int nested_mover_k =
      nested_k_env && *nested_k_env ? passpeg_str_to_int(nested_k_env) : 0;

  // Phase 1: materialize all orderings.
  // Upper bound on count: 10! at worst (4-peg with 11 unseen types). 1M slack.
  const int cap = 1 << 16;
  PegPessOrdering *orderings =
      malloc_or_die((size_t)cap * sizeof(PegPessOrdering));
  PessMaterializeCtx mc = {
      .orderings = orderings, .n_orderings = 0, .capacity = cap};
  Timer t;
  ctimer_start(&t);
  MachineLetter draw_buf[16];
  peg_pess_enum_ordered_draws(tile_types, tile_counts, num_types, bag_size,
                              draw_buf, 0, /*multiplicity=*/1,
                              peg_pess_materialize_cb, &mc);
  (void)fprintf(stderr, "[pessfull] materialized %d orderings (%.2fs)\n",
                mc.n_orderings, ctimer_elapsed_seconds(&t));

  // Phase 2: optional pre-pass to estimate opp's best response, sort.
  // PASSPEG_PESSFULL_GROUP_BY_FIRST_TILE: when set, sort primarily by
  // first bag tile (asc) and secondarily by opp_top_score (desc). This puts
  // same-first-tile orderings into contiguous slices so a worker can process
  // them as a single coarse job, sharing endgame TT state across orderings.
  const char *group_env = getenv("PASSPEG_PESSFULL_GROUP_BY_FIRST_TILE");
  const bool group_by_first = group_env && passpeg_str_to_int(group_env) > 0;
  if (do_sort) {
    Timer sort_t;
    ctimer_start(&sort_t);
    for (int oi = 0; oi < mc.n_orderings; oi++) {
      orderings[oi].opp_top_score = peg_pess_compute_opp_top_score(
          game, &orderings[oi], unseen, ld_size, opp_idx, move);
    }
    // Insertion sort. With group_by_first: primary key = first bag tile asc,
    // secondary = opp_top_score desc. Without: opp_top_score desc only.
    for (int i = 1; i < mc.n_orderings; i++) {
      const PegPessOrdering key = orderings[i];
      int j = i - 1;
      while (j >= 0) {
        bool swap;
        if (group_by_first) {
          if (orderings[j].draw[0] != key.draw[0]) {
            swap = orderings[j].draw[0] > key.draw[0];
          } else {
            swap = orderings[j].opp_top_score < key.opp_top_score;
          }
        } else {
          swap = orderings[j].opp_top_score < key.opp_top_score;
        }
        if (!swap) {
          break;
        }
        orderings[j + 1] = orderings[j];
        j--;
      }
      orderings[j + 1] = key;
    }
    (void)fprintf(
        stderr, "[pessfull] sorted (%.2fs) %s; top opp=%d bottom opp=%d\n",
        ctimer_elapsed_seconds(&sort_t),
        group_by_first ? "by (first_tile, opp_top desc)" : "by opp_top desc",
        mc.n_orderings > 0 ? orderings[0].opp_top_score : 0,
        mc.n_orderings > 0 ? orderings[mc.n_orderings - 1].opp_top_score : 0);
  }

  // Optional: dump materialized orderings (idx, weight, opp_top_score, draw)
  // and exit. Used to reconstruct weighted win% from an interrupted run.
  if (getenv("PASSPEG_PESSFULL_DUMP_ORDERINGS")) {
    for (int oi = 0; oi < mc.n_orderings; oi++) {
      const PegPessOrdering *o = &orderings[oi];
      (void)fprintf(stderr,
                    "[pessfull-dump] ord %3d  weight=%lld  opp_top=%d  draw=",
                    oi + 1, (long long)o->weight, o->opp_top_score);
      for (int i = 0; i < o->n; i++) {
        (void)fprintf(stderr, "%02d%s", o->draw[i], (i + 1 < o->n) ? "_" : "");
      }
      (void)fputc('\n', stderr);
    }
    free(orderings);
    config_destroy(config);
    return;
  }

  // Optional: solve only ONE ordering, selected by its draw signature
  // (underscore-joined two-digit machine letters, e.g. "05_09_01"). Lets us
  // reproduce/inspect a single straggler in isolation.
  const char *only_draw_env = getenv("PASSPEG_PESSFULL_ONLY_DRAW");
  if (only_draw_env && *only_draw_env) {
    MachineLetter want[16];
    int want_n = 0;
    const char *p = only_draw_env;
    while (*p && want_n < 16) {
      want[want_n++] = (MachineLetter)passpeg_str_to_int(p);
      const char *us = strchr(p, '_');
      if (!us) {
        break;
      }
      p = us + 1;
    }
    int found = -1;
    for (int oi = 0; oi < mc.n_orderings && found < 0; oi++) {
      if (orderings[oi].n != want_n) {
        continue;
      }
      bool eq = true;
      for (int i = 0; i < want_n; i++) {
        if (orderings[oi].draw[i] != want[i]) {
          eq = false;
          break;
        }
      }
      if (eq) {
        found = oi;
      }
    }
    if (found < 0) {
      log_fatal("PASSPEG_PESSFULL_ONLY_DRAW=%s matched no ordering",
                only_draw_env);
    }
    orderings[0] = orderings[found];
    mc.n_orderings = 1;
    (void)fprintf(stderr,
                  "[pessfull] ONLY_DRAW=%s → solving 1 ordering in isolation\n",
                  only_draw_env);
  }

  // Shared endgame-state cache. 2^20 entries × 16 bytes = ~16 MB.
  const char *cache_env = getenv("PASSPEG_PESSFULL_CACHE");
  const bool use_cache =
      !cache_env || passpeg_str_to_int(cache_env) > 0; // default on
  PegPessCache *cache = use_cache ? peg_pess_cache_create(1U << 20) : NULL;

  // Macondo-style nested verdict-map cache. Key on info-state; value is the
  // leader cand's per-bag-sig outcome map. 2^18 entries with small dynamic
  // maps inside. Toggle via PASSPEG_PESSFULL_NESTED_CACHE (default on).
  const char *ncache_env = getenv("PASSPEG_PESSFULL_NESTED_CACHE");
  const bool use_nested_cache =
      !ncache_env || passpeg_str_to_int(ncache_env) > 0;
  PegNestedCache *nested_cache =
      use_nested_cache ? peg_nested_cache_create(1U << 18) : NULL;

  // Phase 3: per-worker scratch + parallel solve via peg_pool.
  PegPool *pool = n_threads > 1 ? peg_pool_create(n_threads, 100) : NULL;
  // Slots: the non-split path needs n_threads+1 (workers + helper). The split
  // path acquires slots from an arena; forking nests as deep as free slots
  // allow, gated to stop when free < num_workers. So the bound is the arena
  // size itself — size it well past num_workers so the tail can nest deep
  // (one active pthread) while load throttles via the gate.
  const int n_slots = n_threads * 8 + 8;
  EndgameCtx **eg_ctxs = calloc_or_die((size_t)n_slots, sizeof(EndgameCtx *));
  EndgameResults **eg_results =
      malloc_or_die((size_t)n_slots * sizeof(EndgameResults *));
  // calloc so each solver's ml_pool starts zeroed (count=0, pool[]=NULL).
  // worker_fn deliberately does NOT reset ml_pool_count between calls — the
  // freelist retains MoveLists across orderings on the same slot.
  PegPessSolver *solvers =
      calloc_or_die((size_t)n_slots, sizeof(PegPessSolver));
  for (int i = 0; i < n_slots; i++) {
    eg_results[i] = endgame_results_create();
  }
  // Scratch arena over all slots (split path). free_stack holds every slot
  // index; acquire/release hand them out per task.
  PessSlotArena arena;
  arena.n_slots = n_slots;
  arena.free_stack = malloc_or_die((size_t)n_slots * sizeof(int));
  for (int i = 0; i < n_slots; i++) {
    arena.free_stack[i] = i;
  }
  arena.free_top = n_slots;
  cpthread_mutex_init(&arena.mutex);
  atomic_long total_solves = 0;
  atomic_long total_leaf_visits = 0;
  atomic_long total_recursive = 0;
  atomic_long total_loss_cutoffs = 0;
  atomic_long total_win_cutoffs = 0;
  atomic_long total_nested_calls = 0;
  atomic_int orderings_done = 0;

  // Per-worker telemetry arrays, zero-initialized.
  int64_t *pw_orderings = calloc_or_die((size_t)n_slots, sizeof(int64_t));
  int64_t *pw_solves = calloc_or_die((size_t)n_slots, sizeof(int64_t));
  int64_t *pw_recursive = calloc_or_die((size_t)n_slots, sizeof(int64_t));
  int64_t *pw_nested = calloc_or_die((size_t)n_slots, sizeof(int64_t));
  int64_t *pw_busy_ns = calloc_or_die((size_t)n_slots, sizeof(int64_t));

  // Build the job slices. With group_by_first enabled, orderings sharing the
  // first bag tile become one job. Without, each ordering is its own job.
  int n_jobs = 0;
  PessJob *jobs;
  void **job_ptrs;
  if (group_by_first && mc.n_orderings > 0) {
    // Worst case: every ordering has a different first tile → n_jobs =
    // n_orderings.
    jobs = malloc_or_die((size_t)mc.n_orderings * sizeof(PessJob));
    job_ptrs = malloc_or_die((size_t)mc.n_orderings * sizeof(void *));
    int slice_start = 0;
    for (int oi = 1; oi <= mc.n_orderings; oi++) {
      const bool at_end = (oi == mc.n_orderings);
      const bool first_changed =
          !at_end && (orderings[oi].draw[0] != orderings[slice_start].draw[0]);
      if (at_end || first_changed) {
        const int slice_n = oi - slice_start;
        jobs[n_jobs] = (PessJob){
            .base_game = game,
            .cand = move,
            .unseen = unseen,
            .ld_size = ld_size,
            .mover_idx = mover_idx,
            .opp_idx = opp_idx,
            .thread_control = config_get_thread_control(config),
            .endgame_plies = plies,
            .endgame_time = per_solve_time,
            .tt_fraction_of_mem = tt_fraction_of_mem,
            .shared_tt = shared_tt,
            .opp_sort_mode = opp_sort_mode,
            .mover_sort_mode = mover_sort_mode,
            .subperm_sort = subperm_sort,
            .slow_solve_log_s = slow_solve_log_s,
            .idle_probe_s = idle_probe_s,
            .rung4_probe_s = rung4_probe_s,
            .first_win = first_win,
            .endgame_threads = endgame_threads,
            .skip_word_pruning = skip_word_pruning,
            .max_opp_k = max_opp_k,
            .ordering = &orderings[slice_start],
            .n_orderings = slice_n,
            .eg_ctxs = eg_ctxs,
            .eg_results = eg_results,
            .solvers = solvers,
            .n_worker_slots = n_slots,
            .arena = &arena,
            .shared_n_solves = &total_solves,
            .shared_n_leaf_visits = &total_leaf_visits,
            .shared_n_recursive = &total_recursive,
            .shared_n_loss_cutoffs = &total_loss_cutoffs,
            .shared_n_win_cutoffs = &total_win_cutoffs,
            .shared_n_nested_calls = &total_nested_calls,
            .cache = cache,
            .nested_cache = nested_cache,
            .nested_our_turn = nested_our_turn,
            .nested_depth_limit = nested_depth_limit,
            .nested_mover_k = nested_mover_k,
            .shared_orderings_done = &orderings_done,
            .total_orderings = mc.n_orderings,
            .per_worker_orderings = pw_orderings,
            .per_worker_solves = pw_solves,
            .per_worker_recursive = pw_recursive,
            .per_worker_nested = pw_nested,
            .per_worker_busy_ns = pw_busy_ns,
        };
        job_ptrs[n_jobs] = &jobs[n_jobs];
        n_jobs++;
        slice_start = oi;
      }
    }
    (void)fprintf(
        stderr,
        "[pessfull] grouped %d orderings into %d (move, first_tile) jobs\n",
        mc.n_orderings, n_jobs);
  } else {
    jobs = malloc_or_die((size_t)mc.n_orderings * sizeof(PessJob));
    job_ptrs = malloc_or_die((size_t)mc.n_orderings * sizeof(void *));
    for (int oi = 0; oi < mc.n_orderings; oi++) {
      jobs[oi] = (PessJob){
          .base_game = game,
          .cand = move,
          .unseen = unseen,
          .ld_size = ld_size,
          .mover_idx = mover_idx,
          .opp_idx = opp_idx,
          .thread_control = config_get_thread_control(config),
          .endgame_plies = plies,
          .endgame_time = per_solve_time,
          .tt_fraction_of_mem = tt_fraction_of_mem,
          .shared_tt = shared_tt,
          .opp_sort_mode = opp_sort_mode,
          .mover_sort_mode = mover_sort_mode,
          .subperm_sort = subperm_sort,
          .slow_solve_log_s = slow_solve_log_s,
          .idle_probe_s = idle_probe_s,
          .rung4_probe_s = rung4_probe_s,
          .first_win = first_win,
          .endgame_threads = endgame_threads,
          .skip_word_pruning = skip_word_pruning,
          .max_opp_k = max_opp_k,
          .ordering = &orderings[oi],
          .n_orderings = 1,
          .eg_ctxs = eg_ctxs,
          .eg_results = eg_results,
          .solvers = solvers,
          .n_worker_slots = n_slots,
          .arena = &arena,
          .shared_n_solves = &total_solves,
          .shared_n_leaf_visits = &total_leaf_visits,
          .shared_n_recursive = &total_recursive,
          .shared_n_loss_cutoffs = &total_loss_cutoffs,
          .shared_n_win_cutoffs = &total_win_cutoffs,
          .shared_n_nested_calls = &total_nested_calls,
          .cache = cache,
          .nested_cache = nested_cache,
          .nested_our_turn = nested_our_turn,
          .nested_depth_limit = nested_depth_limit,
          .nested_mover_k = nested_mover_k,
          .shared_orderings_done = &orderings_done,
          .total_orderings = mc.n_orderings,
          .per_worker_orderings = pw_orderings,
          .per_worker_solves = pw_solves,
          .per_worker_recursive = pw_recursive,
          .per_worker_nested = pw_nested,
          .per_worker_busy_ns = pw_busy_ns,
      };
      job_ptrs[oi] = &jobs[oi];
    }
    n_jobs = mc.n_orderings;
  }

  Timer solve_t;
  ctimer_start(&solve_t);
  if (split_opp) {
    // Opp-split dispatch: one work unit per (ordering, opp-reply). jobs[0]
    // carries the shared config (base game, cand, slots, counters) — every
    // job has identical shared fields, the split worker takes the ordering
    // explicitly. Pre-pass (single-threaded) counts each ordering's replies.
    PessJob *shared = &jobs[0];
    // Enable recursive forking on the shared job (split units + their forked
    // sub-tasks inherit it via init_solver_from_job). Needs a real pool.
    // Set arena explicitly here too: jobs[0] may come from either build branch,
    // and the split path (and forking) require the arena to be non-NULL.
    shared->arena = &arena;
    shared->recursive_split = recursive_split && pool != NULL;
    shared->force_nested_perm = force_nested_perm;
    shared->pool = pool;
    if (recursive_split && pool != NULL) {
      (void)fprintf(stderr, "[pessfull] recursive-split ON (fork opp nodes)\n");
    }
    PessSplitAgg *aggs =
        malloc_or_die((size_t)mc.n_orderings * sizeof(PessSplitAgg));
    // First count total units.
    int *reply_counts = malloc_or_die((size_t)mc.n_orderings * sizeof(int));
    int64_t total_units = 0;
    for (int oi = 0; oi < mc.n_orderings; oi++) {
      const int rc = peg_pess_count_opp_replies(shared, &orderings[oi]);
      reply_counts[oi] = rc;
      total_units += (rc > 0) ? rc : 1; // 0 → one whole-ordering unit
      atomic_init(&aggs[oi].worst, PEG_OUT_WIN);
      atomic_init(&aggs[oi].has_loss, 0);
    }
    PessSplitUnit *units =
        malloc_or_die((size_t)total_units * sizeof(PessSplitUnit));
    void **unit_ptrs = malloc_or_die((size_t)total_units * sizeof(void *));
    int64_t ui = 0;
    for (int oi = 0; oi < mc.n_orderings; oi++) {
      const int rc = reply_counts[oi];
      if (rc <= 0) {
        units[ui] = (PessSplitUnit){.job = shared,
                                    .ordering = &orderings[oi],
                                    .agg = &aggs[oi],
                                    .reply_idx = -1};
        unit_ptrs[ui] = &units[ui];
        ui++;
      } else {
        for (int r = 0; r < rc; r++) {
          units[ui] = (PessSplitUnit){.job = shared,
                                      .ordering = &orderings[oi],
                                      .agg = &aggs[oi],
                                      .reply_idx = r};
          unit_ptrs[ui] = &units[ui];
          ui++;
        }
      }
    }
    (void)fprintf(stderr,
                  "[pessfull] split-opp: %d orderings → %lld work units\n",
                  mc.n_orderings, (long long)total_units);
    // Reset the shared counter and set total to units (the split worker uses
    // shared_orderings_done / total_orderings for its progress line).
    atomic_store(&orderings_done, 0);
    shared->total_orderings = (int)total_units;
    if (pool) {
      peg_pool_submit_and_wait(pool, peg_pess_split_worker_fn, unit_ptrs,
                               (int)total_units, /*helper_worker_idx=*/0);
    } else {
      for (int64_t k = 0; k < total_units; k++) {
        peg_pess_split_worker_fn(&units[k], 0);
      }
    }
    // Fold each ordering's aggregate into its outcome for Phase-4 aggregation.
    for (int oi = 0; oi < mc.n_orderings; oi++) {
      orderings[oi].outcome = (PegPessOut)atomic_load(&aggs[oi].worst);
    }
    free(aggs);
    free(reply_counts);
    free(units);
    free(unit_ptrs);
  } else if (pool) {
    peg_pool_submit_and_wait(pool, peg_pess_worker_fn, job_ptrs, n_jobs,
                             /*helper_worker_idx=*/0);
  } else {
    for (int ji = 0; ji < n_jobs; ji++) {
      peg_pess_worker_fn(&jobs[ji], 0);
    }
  }
  const double solve_elapsed = ctimer_elapsed_seconds(&solve_t);

  // Phase 4: aggregate outcomes.
  PegPessFullAccum acc = {0};
  for (int oi = 0; oi < mc.n_orderings; oi++) {
    const PegPessOrdering *o = &orderings[oi];
    if (o->outcome == PEG_OUT_WIN) {
      acc.wins += o->weight;
    } else if (o->outcome == PEG_OUT_LOSS) {
      acc.losses += o->weight;
    } else {
      acc.draws += o->weight;
    }
    acc.total += o->weight;
  }

  const double win_pct = acc.total > 0 ? (double)(acc.wins * 2 + acc.draws) /
                                             (2.0 * (double)acc.total)
                                       : 0.0;

  printf("\n=== Pessimistic full eval ===\n");
  printf("CGP:  %s\n", cgp);
  printf("Move: %s   plies=%d  threads=%d  sort=%d  nested=%d (depth_limit=%d, "
         "mover_k=%d)\n",
         move_str, plies, n_threads, do_sort ? 1 : 0, nested_our_turn ? 1 : 0,
         nested_depth_limit, nested_mover_k);
  printf("W/L/D = %lld/%lld/%lld   total=%lld   win%%=%.4f\n",
         (long long)acc.wins, (long long)acc.losses, (long long)acc.draws,
         (long long)acc.total, win_pct);
  printf("solves=%ld  recursive=%ld  cutoffs(loss=%ld,win=%ld)  "
         "nested_calls=%ld  solve_wall=%.2fs\n",
         atomic_load(&total_solves), atomic_load(&total_recursive),
         atomic_load(&total_loss_cutoffs), atomic_load(&total_win_cutoffs),
         atomic_load(&total_nested_calls), solve_elapsed);
  printf("endgame_leaf_visits=%ld (counts cached too; comparable to macondo's "
         "endgame count)\n",
         atomic_load(&total_leaf_visits));
  if (idle_probe_s > 0.0) {
    const long long slow = atomic_load(&g_probe_slow_solves);
    const long long idle_sum = atomic_load(&g_probe_slow_idle_sum);
    const long long with_idle = atomic_load(&g_probe_slow_with_idle);
    printf("rung5-probe (leaf solves >= %.3fs): slow=%lld, with>=2 idle "
           "cores=%lld (%.1f%%), avg idle cores at slow=%.2f\n",
           idle_probe_s, slow, with_idle,
           slow > 0 ? (100.0 * (double)with_idle) / (double)slow : 0.0,
           slow > 0 ? (double)idle_sum / (double)slow : 0.0);
  }
  if (rung4_probe_s > 0.0) {
    const long long slow = atomic_load(&g_rung4_slow);
    const long long seq = atomic_load(&g_rung4_slow_seq);
    const long long seq_idle = atomic_load(&g_rung4_seq_idle_sum);
    const long long seq_with_idle = atomic_load(&g_rung4_seq_with_idle);
    const long long seq_cands = atomic_load(&g_rung4_seq_cands_sum);
    printf("rung4-probe (nested cand loops >= %.3fs): slow=%lld, "
           "sequential(unparallelized)=%lld; of those: with>=2 idle "
           "cores=%lld (%.1f%%), avg idle=%.2f, avg cands=%.1f\n",
           rung4_probe_s, slow, seq, seq_with_idle,
           seq > 0 ? (100.0 * (double)seq_with_idle) / (double)seq : 0.0,
           seq > 0 ? (double)seq_idle / (double)seq : 0.0,
           seq > 0 ? (double)seq_cands / (double)seq : 0.0);
  }
  if (cache) {
    long h = atomic_load(&cache->hits);
    long m = atomic_load(&cache->misses);
    long total_lookups = h + m;
    printf("cache: hits=%ld misses=%ld (hit_rate=%.1f%%)\n", h, m,
           total_lookups > 0 ? (100.0 * (double)h) / (double)total_lookups
                             : 0.0);
  }
  if (nested_cache) {
    long h = atomic_load(&nested_cache->hits);
    long m = atomic_load(&nested_cache->misses);
    long bm = atomic_load(&nested_cache->bag_misses);
    long total = h + m + bm;
    printf(
        "nested-cache: hits=%ld misses=%ld bag-misses=%ld (hit_rate=%.1f%%)\n",
        h, m, bm, total > 0 ? (100.0 * (double)h) / (double)total : 0.0);
  }

  // Per-worker breakdown. Helps decide private vs shared TT and whether work
  // is balanced. Slot n_slots-1 is the helper (main-thread fallback).
  printf("\nper-worker telemetry "
         "(slot/orderings/recursive/solves/nested/busy_s/TT_hits/TT_lookups/"
         "TT_hit_rate):\n");
  for (int i = 0; i < n_slots; i++) {
    if (pw_orderings[i] == 0 && i != n_slots - 1) {
      continue;
    }
    long long tt_hits = -1;
    long long tt_lookups = -1;
    double tt_hr = 0.0;
    if (eg_ctxs[i]) {
      const TranspositionTable *tt =
          endgame_ctx_get_transposition_table(eg_ctxs[i]);
      if (tt) {
        tt_hits = atomic_load(&((TranspositionTable *)tt)->hits);
        tt_lookups = atomic_load(&((TranspositionTable *)tt)->lookups);
        if (tt_lookups > 0) {
          tt_hr = (100.0 * (double)tt_hits) / (double)tt_lookups;
        }
      }
    }
    printf("  slot=%-3d ord=%-4lld rec=%-9lld solves=%-8lld nested=%-7lld "
           "busy=%.2fs",
           i, (long long)pw_orderings[i], (long long)pw_recursive[i],
           (long long)pw_solves[i], (long long)pw_nested[i],
           (double)pw_busy_ns[i] / 1e9);
    if (tt_hits >= 0) {
      printf(" tt_hits=%lld tt_lookups=%lld hr=%.1f%%", tt_hits, tt_lookups,
             tt_hr);
    } else {
      printf(" tt=disabled");
    }
    printf("\n");
  }
  if (shared_tt) {
    const long long h = atomic_load(&shared_tt->hits);
    const long long l = atomic_load(&shared_tt->lookups);
    printf("shared-TT: hits=%lld lookups=%lld (hit_rate=%.1f%%)\n", h, l,
           l > 0 ? (100.0 * (double)h) / (double)l : 0.0);
  }

  for (int i = 0; i < n_slots; i++) {
    endgame_ctx_destroy(eg_ctxs[i]);
    endgame_results_destroy(eg_results[i]);
    for (int p = 0; p < solvers[i].ml_pool_count; p++) {
      move_list_destroy(solvers[i].ml_pool[p]);
    }
    if (solvers[i].eg_scratch) {
      game_destroy(solvers[i].eg_scratch);
    }
  }
  free(eg_ctxs);
  free(eg_results);
  free(solvers);
  free(arena.free_stack);
  free(jobs);
  free(job_ptrs);
  free(orderings);
  peg_pess_cache_destroy(cache);
  peg_nested_cache_destroy(nested_cache);
  if (shared_tt) {
    transposition_table_destroy(shared_tt);
  }
  free(pw_orderings);
  free(pw_solves);
  free(pw_recursive);
  free(pw_nested);
  free(pw_busy_ns);
  if (pool) {
    peg_pool_destroy(pool);
  }
  validated_moves_destroy(vms);
  error_stack_destroy(parse_err);
  config_destroy(config);
}

// Debug harness: solve a single endgame from a CGP and report timing. Used to
// reproduce/diagnose pathological endgames captured by the pessimistic
// solver's pre-solve logging. Env:
//   PASSPEG_ENDGAME_CGP      (required) full cgp string incl. -lex
//   PASSPEG_ENDGAME_PLIES    (default 2)
//   PASSPEG_ENDGAME_TIME     (default 0 = no limit)
//   PASSPEG_ENDGAME_THREADS  (default 1)
//   PASSPEG_ENDGAME_FIRST_WIN (default 0)
void test_pass_peg_endgame_one(void) {
  const char *cgp = getenv("PASSPEG_ENDGAME_CGP");
  if (!cgp || !*cgp) {
    log_fatal("PASSPEG_ENDGAME_CGP must be set");
  }
  const char *plies_env = getenv("PASSPEG_ENDGAME_PLIES");
  const int plies = plies_env && *plies_env ? passpeg_str_to_int(plies_env) : 2;
  const char *time_env = getenv("PASSPEG_ENDGAME_TIME");
  const double tlimit =
      time_env && *time_env ? passpeg_str_to_double(time_env) : 0.0;
  const char *threads_env = getenv("PASSPEG_ENDGAME_THREADS");
  const int threads = threads_env && passpeg_str_to_int(threads_env) > 0
                          ? passpeg_str_to_int(threads_env)
                          : 1;
  const char *fw_env = getenv("PASSPEG_ENDGAME_FIRST_WIN");
  const bool first_win = fw_env && passpeg_str_to_int(fw_env) > 0;

  Config *config = config_create_or_die("set -s1 score -s2 score");
  char load_cmd[10240];
  (void)snprintf(load_cmd, sizeof(load_cmd), "cgp %s", cgp);
  load_and_exec_config_or_die(config, load_cmd);
  Game *game = config_get_game(config);
  game_set_endgame_solving_mode(game);
  game_set_backup_mode(game, BACKUP_MODE_OFF);

  EndgameCtx *ctx = NULL;
  EndgameResults *results = endgame_results_create();
  (void)fprintf(stderr,
                "[pegendgame] plies=%d time=%.1fs threads=%d first_win=%d\n",
                plies, tlimit, threads, first_win ? 1 : 0);
  (void)fflush(stderr);
  EndgameArgs ea = {
      .thread_control = config_get_thread_control(config),
      .game = game,
      .plies = plies,
      .shared_tt = NULL,
      .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      .num_threads = threads,
      .use_heuristics = true,
      .num_top_moves = 1,
      .first_win = first_win,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .skip_word_pruning = false,
      .soft_time_limit = tlimit,
      .hard_time_limit = tlimit,
      .external_deadline_ns =
          tlimit > 0.0 ? ctimer_monotonic_ns() + (int64_t)(tlimit * 1.0e9) : 0,
  };
  Timer t;
  ctimer_start(&t);
  endgame_solve_inline(&ctx, &ea, results);
  const double elapsed = ctimer_elapsed_seconds(&t);
  const int eg_val = endgame_results_get_value(results, ENDGAME_RESULT_BEST);
  printf("[pegendgame] value=%d  wall=%.3fs\n", eg_val, elapsed);

  endgame_ctx_destroy(ctx);
  endgame_results_destroy(results);
  config_destroy(config);
}
