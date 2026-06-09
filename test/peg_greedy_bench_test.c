#include "peg_greedy_bench_test.h"

#include "../src/compat/cpthread.h"
#include "../src/compat/ctime.h"
#include "../src/def/board_defs.h"
#include "../src/def/cpthread_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/kwg_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/kwg.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/transposition_table.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/kwg_maker.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/peg_pool.h"
#include "../src/impl/word_prune.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
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

// Load up to max_n nonempty lines (CGP strings; anything after a tab is
// dropped) from `path` into a string_duplicate'd array. Returns the count.
static int load_cgp_lines(const char *path, char ***out, int max_n) {
  FILE *f = fopen(path, "re");
  if (!f) {
    return 0;
  }
  char **arr = malloc_or_die((size_t)max_n * sizeof(char *));
  int n = 0;
  char line[8192];
  while (n < max_n && fgets(line, sizeof(line), f)) {
    int len = (int)strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[--len] = '\0';
    }
    if (len == 0) {
      continue;
    }
    char *tab = strchr(line, '\t');
    if (tab) {
      *tab = '\0';
    }
    arr[n] = string_duplicate(line);
    n++;
  }
  (void)fclose(f);
  *out = arr;
  return n;
}

// ====================================================================
// peggreedy: fresh N-in-bag greedy bench (d=0 only)
//
// Goal: for each candidate mover move, enumerate the distinct N-tile bag
// compositions (= the bag's tile multiset at the moment mover plays), split
// each into (mover_drawn, bag_remaining), greedy-play to game end, and
// aggregate weighted win % / mean spread.
//
// Scenarios = (mover_drawn_multiset, bag_remaining_multiset) pairs. Within
// the drawn multiset the order of tiles doesn't matter — mover ends up
// with the same rack regardless of draw order. Within the remaining
// multiset (size 1 for k_drawn=N-1, more otherwise) the order also doesn't
// matter since opp will eventually draw them. So we enumerate by per-type
// COUNTS, not by ordered tuples.
//
// d=0 means: after mover plays cand and the bag/opp-rack are set up, we
// do a pure greedy playout (highest-equity move while bag has tiles,
// highest-score once bag empties) until natural game end. mover_total =
// signed (mover_score - opp_score) at terminal, with rack penalties
// applied if both players pass to the scoreless cap.
//
// Env knobs:
//   PASSPEG_GREEDY_PATH    — CGP file (default /tmp/peg_positions.txt)
//   PASSPEG_GREEDY_LEX     — lex override; empty = let CGP's -lex stand
//   PASSPEG_GREEDY_TOP_K   — number of top cands to print (default 15)
//   PASSPEG_GREEDY_ONLY    — semicolon-separated cand-text substrings;
//                             only cands whose movegen text matches one
//                             of these are evaluated. Empty = all cands.
//   PASSPEG_GREEDY_TSV     — output path for per-scenario TSV
// ====================================================================

static int peg_compute_unseen(const Game *game, int mover_idx,
                              uint8_t unseen[MAX_ALPHABET_SIZE]) {
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);
  memset(unseen, 0, sizeof(uint8_t) * MAX_ALPHABET_SIZE);
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] = (uint8_t)ld_get_dist(ld, ml);
  }
  const Rack *mrack = player_get_rack(game_get_player(game, mover_idx));
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] -= (uint8_t)rack_get_letter(mrack, ml);
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

// Deterministically set the bag to exactly the listed N tiles (no PRNG).
// bag_add_letter would randomize order via the bag's time-seeded PRNG, which
// is wrong for an exact enumerator and non-deterministic across processes.
static void peg_set_bag_tiles(Bag *bag, const MachineLetter *tiles, int n_tiles,
                              int ld_size) {
  (void)ld_size;
  bag_set_to_tiles(bag, tiles, n_tiles);
}

// Reset opp's rack to (unseen MINUS drawn) tiles.
static void peg_set_opp_rack(Rack *opp_rack,
                             const uint8_t unseen[MAX_ALPHABET_SIZE],
                             int ld_size, const MachineLetter *drawn,
                             int n_drawn) {
  uint8_t remaining[MAX_ALPHABET_SIZE];
  for (int ml = 0; ml < ld_size; ml++) {
    remaining[ml] = unseen[ml];
  }
  for (int i = 0; i < n_drawn; i++) {
    // drawn[0..n_drawn) holds machine letters in [0, ld_size); remaining[] is
    // initialized for that range by the loop above.
    // NOLINTNEXTLINE(clang-analyzer-core.uninitialized.ArraySubscript)
    if (remaining[drawn[i]] > 0) {
      remaining[drawn[i]]--;
    }
  }
  rack_reset(opp_rack);
  for (int ml = 0; ml < ld_size; ml++) {
    for (int i = 0; i < remaining[ml]; i++) {
      rack_add_letter(opp_rack, (MachineLetter)ml);
    }
  }
}

// Build the post-cand game state for one (mover_drawn, bag_remaining) split:
// bag holds (mover_drawn ++ bag_remaining) = N tiles total, opp rack is set
// to (unseen − bag tiles), cand is played, then mover draws their
// k_drawn tiles. Result: mover's rack has k_drawn drawn tiles added, bag has
// only bag_remaining left, opp's rack is the deal (full 7).
static Game *peg_make_post_cand_game(const Game *base_game, int mover_idx,
                                     const uint8_t *unseen, int ld_size,
                                     const Move *cand, int k_drawn,
                                     const MachineLetter *mover_drawn,
                                     int n_bag_remaining,
                                     const MachineLetter *bag_remaining) {
  Game *g = game_duplicate(base_game);
  game_set_endgame_solving_mode(g);
  game_set_backup_mode(g, BACKUP_MODE_OFF);
  Bag *bag = game_get_bag(g);
  Rack *opp_r = player_get_rack(game_get_player(g, 1 - mover_idx));
  Rack *mover_r = player_get_rack(game_get_player(g, mover_idx));
  // Combine drawn + remaining into one N-tile array for bag setup.
  MachineLetter all_bag[16];
  int N = k_drawn + n_bag_remaining;
  for (int i = 0; i < k_drawn; i++) {
    all_bag[i] = mover_drawn[i];
  }
  for (int i = 0; i < n_bag_remaining; i++) {
    all_bag[k_drawn + i] = bag_remaining[i];
  }
  peg_set_bag_tiles(bag, all_bag, N, ld_size);
  peg_set_opp_rack(opp_r, unseen, ld_size, all_bag, N);
  play_move_without_drawing_tiles(cand, g);
  for (int i = 0; i < k_drawn; i++) {
    rack_add_letter(mover_r, mover_drawn[i]);
    (void)bag_draw_letter(bag, mover_drawn[i], 0);
  }
  // play_move_without_drawing_tiles sets GAME_END_REASON_STANDARD when the
  // rack empties post-placement and bag <= RACK_SIZE — meant for the endgame
  // solver's no-drawing world. We manually re-stock the rack right after,
  // so the going-out flag is stale. Clear it whenever the rack isn't empty;
  // otherwise the greedy playout's first-line "if game_end != NONE break"
  // bails before simulating any continuation for emptier candidates.
  if (!rack_is_empty(mover_r)) {
    game_set_game_end_reason(g, GAME_END_REASON_NONE);
  }
  return g;
}

// Greedy playout to game end. Returns signed mover spread at terminal
// (mover_score − opp_score). Records each played move into out_pv (text)
// and writes final mover/opp rack contents to out_*_rack. If out_pv_text
// is NULL no PV is recorded.
static int32_t
peg_greedy_playout_pv(Game *game, int mover_idx, MoveList *playout_ml,
                      int thread_index, char *out_pv_text,
                      size_t out_pv_text_cap, char *out_mover_rack_end,
                      size_t out_mover_rack_end_cap, char *out_opp_rack_end,
                      size_t out_opp_rack_end_cap, char *out_final_cgp,
                      size_t out_final_cgp_cap) {
  // thread_index is vestigial since get_movegen() selects the per-pthread
  // MoveGen automatically; kept in the signature until the call sites are
  // cleaned up. Cast to silence -Wunused-parameter under the dev build.
  (void)thread_index;
  const LetterDistribution *ld = game_get_ld(game);
  StringBuilder *pv_sb = NULL;
  if (out_pv_text && out_pv_text_cap > 0) {
    pv_sb = string_builder_create();
    out_pv_text[0] = '\0';
  }
  int n_plies = 0;
  for (int turn = 0; turn < MAX_SEARCH_DEPTH; turn++) {
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      break;
    }
    const bool bag_has_tiles = bag_get_letters(game_get_bag(game)) > 0;
    const MoveGenArgs ga = {
        .game = game,
        .move_list = playout_ml,
        .move_record_type = MOVE_RECORD_BEST,
        .move_sort_type = bag_has_tiles ? MOVE_SORT_EQUITY : MOVE_SORT_SCORE,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&ga);
    if (move_list_get_count(playout_ml) == 0) {
      break;
    }
    const Move *best = move_list_get_move(playout_ml, 0);
    if (pv_sb) {
      if (n_plies > 0) {
        string_builder_add_string(pv_sb, " | ");
      }
      string_builder_add_move(pv_sb, game_get_board(game), best, ld, true);
    }
    play_move(best, game, NULL);
    n_plies++;
  }
  if (pv_sb) {
    (void)snprintf(out_pv_text, out_pv_text_cap, "%s",
                   string_builder_peek(pv_sb));
    string_builder_destroy(pv_sb);
  }
  if (out_mover_rack_end && out_mover_rack_end_cap > 0) {
    StringBuilder *rsb = string_builder_create();
    string_builder_add_rack(
        rsb, player_get_rack(game_get_player(game, mover_idx)), ld, false);
    (void)snprintf(out_mover_rack_end, out_mover_rack_end_cap, "%s",
                   string_builder_peek(rsb));
    string_builder_destroy(rsb);
  }
  if (out_opp_rack_end && out_opp_rack_end_cap > 0) {
    StringBuilder *rsb = string_builder_create();
    string_builder_add_rack(
        rsb, player_get_rack(game_get_player(game, 1 - mover_idx)), ld, false);
    (void)snprintf(out_opp_rack_end, out_opp_rack_end_cap, "%s",
                   string_builder_peek(rsb));
    string_builder_destroy(rsb);
  }
  if (out_final_cgp && out_final_cgp_cap > 0) {
    char *cgp = game_get_cgp(game, true);
    (void)snprintf(out_final_cgp, out_final_cgp_cap, "%s", cgp ? cgp : "");
    free(cgp);
  }
  const Player *me = game_get_player(game, mover_idx);
  const Player *op = game_get_player(game, 1 - mover_idx);
  int32_t spread = equity_to_int(player_get_score(me) - player_get_score(op));
  if (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    spread -= (int32_t)equity_to_int(rack_get_score(ld, player_get_rack(me)));
    spread += (int32_t)equity_to_int(rack_get_score(ld, player_get_rack(op)));
  }
  return spread;
}

static int64_t peg_binomial(int n, int k) {
  if (k < 0 || k > n) {
    return 0;
  }
  if (k == 0 || k == n) {
    return 1;
  }
  if (k > n - k) {
    k = n - k;
  }
  int64_t r = 1;
  for (int i = 0; i < k; i++) {
    r = r * (n - i) / (i + 1);
  }
  return r;
}

// In-place lexicographic next-permutation over MachineLetter arrays.
// Skips duplicates naturally (only enumerates distinct orderings).
// Caller should sort the array ascending before the first iteration.
// Returns false when the array is at the last permutation.
static bool peg_next_perm(MachineLetter *arr, int n) {
  if (n <= 1) {
    return false;
  }
  int k = n - 2;
  while (k >= 0 && arr[k] >= arr[k + 1]) {
    k--;
  }
  if (k < 0) {
    return false;
  }
  int l = n - 1;
  while (arr[k] >= arr[l]) {
    l--;
  }
  MachineLetter tmp = arr[k];
  arr[k] = arr[l];
  arr[l] = tmp;
  int i = k + 1;
  int j = n - 1;
  while (i < j) {
    tmp = arr[i];
    arr[i] = arr[j];
    arr[j] = tmp;
    i++;
    j--;
  }
  return true;
}

// Recursive enumerator over N-multisets from `counts[0..k_types-1]`. Calls
// cb(picked, ctx) once per distinct multiset.
__attribute__((unused)) static void
peg_enum_multiset(int *picked, int idx, int remaining, const int *counts,
                  int k_types, void (*cb)(const int *picked, void *ctx),
                  void *ctx) {
  if (idx == k_types) {
    if (remaining == 0) {
      cb(picked, ctx);
    }
    return;
  }
  int max_take = counts[idx] < remaining ? counts[idx] : remaining;
  for (int k = 0; k <= max_take; k++) {
    picked[idx] = k;
    peg_enum_multiset(picked, idx + 1, remaining - k, counts, k_types, cb, ctx);
  }
  picked[idx] = 0;
}

typedef struct PegCandResult {
  int ci;
  int64_t weight_sum;
  int64_t win_x2;     // 2 per win, 1 per tie, 0 per loss (scaled by weight)
  int64_t spread_sum; // signed (mover − opp) at terminal × weight
  int n_scen;
  // Running tally of total per-scenario weight encountered so far; used by
  // PASSPEG_SCENARIO_STRIDE for stratified sampling proportional to
  // weight (weight-unit stride mod k).
  int64_t sampled_weight_seen;
  // True if any inner endgame_solve returned with depth=-1 (no IDS pass
  // completed before the global deadline). In that case get_value() is
  // the uninitialized zero, so mt = mover_lead and the spread/win tallies
  // for that opp-POV state are bogus. Stage post-processing drops cands flagged
  // incomplete from the ranking.
  bool incomplete;
} PegCandResult;

// Shared state for one cand's enumeration; passed by pointer through the
// recursive enumerators below so we don't rely on gcc nested functions.
typedef struct PegEnumCtx {
  // The current N-multiset being explored (per-type counts, summing to N).
  // peg_enum_outer_multiset writes into this; peg_enum_mover_drawn reads.
  int *n_multiset;
  // The current mover-drawn sub-multiset (per-type counts, summing to
  // k_drawn). peg_enum_mover_drawn writes into this; the leaf
  // peg_emit_split reads.
  int *mover_pick;

  const MachineLetter *types;
  const int *type_counts;
  int k_types;
  int k_drawn;
  int n_bag_remaining;

  const Game *base_game;
  int mover_idx;
  const uint8_t *unseen;
  int ld_size;
  const Move *cand;
  const char *cand_txt;
  int cand_score;
  int pos_idx;
  const LetterDistribution *ld;

  FILE *tsv_f;
  PegCandResult *res;

  // Optional semicolon-separated list of "drawn/remaining" patterns; if
  // non-NULL, only scenarios whose canonical (drawn,remaining) tile strings
  // match one of these substrings get evaluated. Drawn / remaining are
  // emitted as sorted-by-type-index tile lists. Examples: "III/A",
  // "GII/U;IIT/A".
  const char *scenario_filter;

  // Optional semicolon-separated list of substrings; when non-NULL, only
  // opp moves whose movegen text contains one of these substrings are
  // considered. Lets us isolate specific opp moves (e.g. TEMPURA, 6D (T)A)
  // when probing a single scenario at depth.
  const char *opp_move_filter;

  // Evaluation depth.
  //   0 = greedy playout from post-cand state (mover & opp both greedy).
  //   >=1 = enumerate opp's top-K moves at post-cand; for each, apply,
  //         then run endgame_solve at `depth` plies (bag is empty after
  //         opp draws the last bag tile). Take MIN over branches —
  //         matches macondo "guaranteed wins": any opp move that beats
  //         mover ⇒ mark scenario a loss.
  int depth;
  // Number of opp moves enumerated at d>=1. Top-K by EQUITY.
  int opp_top_k;
  // ThreadControl for endgame_solve at d>=1.
  ThreadControl *thread_control;

  // ---- threading ----
  // The thread_index used for movegen + endgame caches by this scenario's
  // worker. Set per-job by the worker fn (= worker_idx the executor hands
  // in). Zero for the serial / non-executor code path.
  int worker_idx;
  // Optional shared executor for inner opp-util dispatch from inside
  // emit_split. NULL = run the opp-util sweep serially.
  PegPool *executor;
  // Mutex protecting the per-cand aggregator `res`. Multiple scenarios
  // of the same cand may run concurrently. NULL = serial mode.
  cpthread_mutex_t *res_mutex;
  // Mutex protecting writes to `tsv_f`. NULL = serial mode.
  cpthread_mutex_t *tsv_mutex;
  // Mutex serializing endgame_solve_inline calls. The internal endgame
  // state has shared mutables (movegen/ABDADA caches keyed by fixed-ish
  // thread indices, pruned-KWG generation, etc.) that race under
  // concurrent workers — so we currently serialize the heavy solve step
  // while keeping the cheaper scenario setup (game_duplicate, opp
  // movegen, MIN aggregation) parallel. NULL = serial mode.
  cpthread_mutex_t *endgame_mutex;
  // When non-NULL, emit_split pushes a copy of (n_multiset, mover_pick)
  // onto this list and returns instead of evaluating. Used by the outer
  // driver to enumerate scenarios in one thread before dispatching them
  // to workers. Guarded by *jobs_mutex (callers run enumeration
  // single-threaded, but the worker fn shares the result back).
  struct PegScenarioJobList *out_jobs;
  // Deadline-watch shared by all workers for time-budget interruption.
  // budget_timer points at the shared start Timer; budget_secs is the
  // total budget. Workers check `ctimer_elapsed_seconds(budget_timer) >
  // budget_secs` and skip their job if exceeded (returning fast so the
  // executor drains quickly). When budget_secs <= 0 or budget_timer is
  // NULL, no time limit is enforced.
  const Timer *budget_timer;
  double budget_secs;
  // Absolute monotonic-ns deadline derived from budget_secs at solver
  // entry. Plumbed through to endgame_solve_inline as external_deadline_ns
  // so abdada_negamax bails out mid-search rather than running to ply
  // completion. 0 = no deadline.
  int64_t deadline_monotonic_ns;
  // Optional per-inner opp-POV TSV, set via PASSPEG_INNER_TSV. When non-NULL,
  // peg_opp_pov_worker_fn writes one row per evaluated opp-POV state with the
  // outer-scenario tile letters, the opp candidate, the opp-POV bag
  // composition, opp-POV weight, and the leaf mt. Lets us trace exactly what
  // the inner 1peg evaluator saw for each candidate. Guarded by
  // inner_tsv_mutex.
  FILE *inner_tsv_f;
  cpthread_mutex_t *inner_tsv_mutex;
  // Per-cand cache of post-opp game state -> mover_total. Allocated by
  // the per-cand dispatcher, freed after the cand's scenarios complete.
  // NULL means caching disabled. See PegOppPovCache above. Largely
  // ineffective on its own (3.5% hit rate on leaf-state collisions) —
  // kept for diagnostics; the real speedup is the shared_eg_tt below.
  struct PegOppPovCache *opp_pov_cache;
  // Per-cand shared endgame TranspositionTable. Passed as shared_tt
  // into every endgame_solve call in this cand's scenarios so the TT
  // is reused across opp-POV states + opp_moves + scenarios. Endgame search
  // sub-trees often share positions (especially right after opp's move
  // when only a couple of tiles differ); the TT hits cut redundant
  // negamax work. macondo's "nested-cache 71.9% hit rate" is the
  // equivalent of this TT sharing.
  TranspositionTable *shared_eg_tt;
  // Per-worker persistent EndgameCtx pointers, indexed by
  // (worker_idx - executor_thread_offset). Workers reuse their own slot
  // across all endgame_solve calls in this cand's evaluation,
  // amortizing the per-call setup cost (ABDADA init, move sort, etc.).
  // Slot 0 reserved for the main thread (worker_idx 0).
  EndgameCtx **per_worker_eg_ctx;
  int per_worker_eg_ctx_n;
  int per_worker_eg_ctx_offset; // = executor_thread_offset (100)
} PegEnumCtx;

// One (cand, n_multiset, mover_pick) scenario, packaged for dispatch
// through PegPool. `base_ctx` is a shared read-only template;
// `n_multiset` and `mover_pick` are owned copies (k_types ints each).
// One (multiset, mover_pick) scenario, packaged for dispatch through
// PegPool. `base_ctx` is the per-cand read-only template;
// `n_multiset` and `mover_pick` are owned copies (k_types ints each). The
// worker hands this to peg_emit_split (with out_jobs=NULL) — at d=0 the
// worker walks bag-tile orderings serially via peg_eval_d0_ordering; at
// d>=1 the bag is empty at the leaf so there is no ordering walk.
typedef struct PegScenarioJob {
  const PegEnumCtx *base_ctx;
  int *n_multiset;
  int *mover_pick;
} PegScenarioJob;

typedef struct PegScenarioJobList {
  PegScenarioJob *items;
  int n;
  int cap;
} PegScenarioJobList;

static void peg_joblist_push(PegScenarioJobList *jl, PegScenarioJob job) {
  if (jl->n == jl->cap) {
    int new_cap = jl->cap > 0 ? jl->cap * 2 : 256;
    PegScenarioJob *new_items =
        realloc(jl->items, (size_t)new_cap * sizeof(PegScenarioJob));
    if (!new_items) {
      log_fatal("peg joblist realloc failed");
    }
    jl->items = new_items;
    jl->cap = new_cap;
  }
  // jl->items is non-null here: the grow block above reallocs (and log_fatals
  // on failure) whenever n == cap, including the initial cap == 0 case.
  // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
  jl->items[jl->n++] = job;
}

static inline void peg_lock(cpthread_mutex_t *m) {
  if (m) {
    cpthread_mutex_lock(m);
  }
}

static inline void peg_unlock(cpthread_mutex_t *m) {
  if (m) {
    cpthread_mutex_unlock(m);
  }
}

// True when (drawn_str, remaining_str) matches one of the semicolon-
// separated "drawn/remaining" patterns in `filter`. NULL filter = always
// matches.
static bool peg_scenario_filter_match(const char *filter, const char *drawn_str,
                                      const char *remaining_str) {
  if (!filter) {
    return true;
  }
  char joined[64];
  (void)snprintf(joined, sizeof(joined), "%s/%s", drawn_str, remaining_str);
  char tmp[2048];
  (void)snprintf(tmp, sizeof(tmp), "%s", filter);
  const char *tok = strtok(tmp, ";");
  while (tok != NULL) {
    if (strcmp(tok, joined) == 0) {
      return true;
    }
    tok = strtok(NULL, ";");
  }
  return false;
}

// Leaf: build the realized split's tile arrays, run a greedy playout, and
// fold the outcome into res / TSV.
// One (opp_move, perceived_bag_tile) leaf for the opp utility sweep
// inside emit_split. Read-only fields are shared; mover_total is the
// only per-job write target. The worker fn body is declared after
// emit_split (it shares scope with the outer scenario worker), so we
// forward-declare it here.
typedef struct PegOppInnerJob {
  const Game *base_game; // post-cand game (RO, shared)
  int mover_idx;
  int ld_size;
  const Move *opp_move; // shared, RO
  MachineLetter bag_ml; // perceived bag tile in this opp-POV
  int weight;           // == opp_type_counts[ti]
  int n_opp_types;
  const MachineLetter *opp_types;
  const int *opp_type_counts;
  int ti; // index into opp_types for this scenario
  int opp_depth;
  ThreadControl *thread_control;
  // Absolute monotonic-ns wall deadline for this item. When 0, no deadline.
  // Set by the caller from the outer peg ctx so a slow opp-inner can bail
  // cleanly instead of running the whole inner endgame past the cascade
  // budget.
  int64_t deadline_monotonic_ns;
  int32_t mover_total; // output
} PegOppInnerJob;
static void peg_opp_inner_worker_fn(void *arg, int worker_idx);

// Per-cand cache of (post-opp game state) -> mover_total. The walker
// re-evaluates many opp-POV states that produce structurally identical post-opp
// states (different scenarios reaching the same board + racks). macondo's
// equivalent caches the recursive PEG sub-result and hits ~70%+; sharing
// the leaf eval across scenarios within one cand cuts the redundant
// endgame_solve / greedy work substantially.
//
// Key: 64-bit FNV-1a over (board tiles | mover_rack counts | opp_rack
// counts | bag counts | scores). Open-addressing with linear probing.
// Single mutex — contention is low because each scenario does many cache
// ops between its single endgame_solve, and the cache is small.
typedef struct PegOppPovCacheEntry {
  uint64_t key;
  int32_t mt;
  bool valid;
} PegOppPovCacheEntry;

typedef struct PegOppPovCache {
  PegOppPovCacheEntry *entries;
  size_t capacity; // power of 2
  cpthread_mutex_t mutex;
  atomic_int hits;
  atomic_int misses;
} PegOppPovCache;

static PegOppPovCache *peg_opp_pov_cache_create(size_t capacity) {
  // Round up to power of 2.
  size_t cap = 1;
  while (cap < capacity) {
    cap *= 2;
  }
  PegOppPovCache *cache = malloc_or_die(sizeof(PegOppPovCache));
  cache->entries = calloc_or_die(cap, sizeof(PegOppPovCacheEntry));
  cache->capacity = cap;
  cpthread_mutex_init(&cache->mutex);
  atomic_init(&cache->hits, 0);
  atomic_init(&cache->misses, 0);
  return cache;
}

static void peg_opp_pov_cache_destroy(PegOppPovCache *cache) {
  if (!cache) {
    return;
  }
  free(cache->entries);
  free(cache);
}

static inline uint64_t peg_fnv1a_update(uint64_t hash, const void *data,
                                        size_t len) {
  const uint8_t *bytes = (const uint8_t *)data;
  for (size_t byte_idx = 0; byte_idx < len; byte_idx++) {
    hash ^= bytes[byte_idx];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

// Hash a Game state for the cache. We hash everything that affects the
// leaf-eval outcome (board tiles, both racks, bag, scores) so that two
// game-state-identical positions get the same key regardless of which
// path produced them.
static uint64_t peg_hash_game_state(const Game *game, int mover_idx) {
  uint64_t hash = 0xcbf29ce484222325ULL;
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      MachineLetter ml = board_get_letter(board, row, col);
      hash = peg_fnv1a_update(hash, &ml, sizeof(ml));
    }
  }
  const Rack *mover_rack = player_get_rack(game_get_player(game, mover_idx));
  const Rack *opp_rack = player_get_rack(game_get_player(game, 1 - mover_idx));
  const int dist_size = ld_get_size(game_get_ld(game));
  for (int i = 0; i < dist_size; i++) {
    uint8_t mover_count = rack_get_letter(mover_rack, i);
    uint8_t opp_count = rack_get_letter(opp_rack, i);
    hash = peg_fnv1a_update(hash, &mover_count, sizeof(mover_count));
    hash = peg_fnv1a_update(hash, &opp_count, sizeof(opp_count));
  }
  const Bag *bag = game_get_bag(game);
  for (int i = 0; i < dist_size; i++) {
    uint8_t bag_count = bag_get_letter((Bag *)bag, (MachineLetter)i);
    hash = peg_fnv1a_update(hash, &bag_count, sizeof(bag_count));
  }
  Equity mover_score = player_get_score(game_get_player(game, mover_idx));
  Equity opp_score = player_get_score(game_get_player(game, 1 - mover_idx));
  hash = peg_fnv1a_update(hash, &mover_score, sizeof(mover_score));
  hash = peg_fnv1a_update(hash, &opp_score, sizeof(opp_score));
  // Avoid 0 since we use it as "no key" sentinel in some paths.
  if (hash == 0) {
    hash = 1;
  }
  return hash;
}

// Lookup. Returns true on hit (and writes *out_mt); false on miss.
static bool peg_opp_pov_cache_lookup(PegOppPovCache *cache, uint64_t key,
                                     int32_t *out_mt) {
  if (!cache) {
    return false;
  }
  const size_t mask = cache->capacity - 1;
  size_t idx = (size_t)key & mask;
  cpthread_mutex_lock(&cache->mutex);
  for (size_t probe = 0; probe < cache->capacity; probe++) {
    const PegOppPovCacheEntry *entry = &cache->entries[(idx + probe) & mask];
    if (!entry->valid) {
      cpthread_mutex_unlock(&cache->mutex);
      atomic_fetch_add(&cache->misses, 1);
      return false;
    }
    if (entry->key == key) {
      *out_mt = entry->mt;
      cpthread_mutex_unlock(&cache->mutex);
      atomic_fetch_add(&cache->hits, 1);
      return true;
    }
  }
  cpthread_mutex_unlock(&cache->mutex);
  return false; // table full — extremely unlikely
}

static void peg_opp_pov_cache_store(PegOppPovCache *cache, uint64_t key,
                                    int32_t mt) {
  if (!cache) {
    return;
  }
  const size_t mask = cache->capacity - 1;
  size_t idx = (size_t)key & mask;
  cpthread_mutex_lock(&cache->mutex);
  for (size_t probe = 0; probe < cache->capacity; probe++) {
    PegOppPovCacheEntry *entry = &cache->entries[(idx + probe) & mask];
    if (!entry->valid || entry->key == key) {
      entry->key = key;
      entry->mt = mt;
      entry->valid = true;
      cpthread_mutex_unlock(&cache->mutex);
      return;
    }
  }
  cpthread_mutex_unlock(&cache->mutex);
}

// Count tiles a cand actually plays from rack (letters outside parens in
// the move text). E.g. "C6 ACIDOT(I)c 80" -> 7 (the (I) is a board tile);
// "5E A(N) 4" -> 1. Used by the cascade driver to bucket cands as
// emptier vs non-emptier given a known bag size.
static int peg_count_tiles_played(const char *cand_text) {
  if (!cand_text) {
    return 0;
  }
  // Skip past the position token (chars up to first space).
  const char *p = cand_text;
  while (*p && *p != ' ') {
    p++;
  }
  while (*p == ' ') {
    p++;
  }
  // Count alpha chars outside parens until the next space (the score).
  int count = 0;
  bool in_paren = false;
  while (*p && *p != ' ') {
    if (*p == '(') {
      in_paren = true;
    } else if (*p == ')') {
      in_paren = false;
    } else if (!in_paren &&
               ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) {
      count++;
    }
    p++;
  }
  return count;
}

// Three-bucket classifier for cascade diversity quota.
//   0 = capable-emp:   tp >= bag AND post-cand rack >= 5 (bingo-capable
//                      next turn after drawing the bag)
//   1 = non-emp:       tp <  bag (mover doesn't drain bag this turn)
//   2 = incapable-emp: tp >= bag AND post-cand rack <  5
// post-cand rack = (rack_size + bag_size) - tp  when tp >= bag,
//                  rack_size                     when tp <  bag.
static int peg_cand_bucket(int tiles_played, int bag_size, int rack_size) {
  if (tiles_played < bag_size) {
    return 1; // non-emp
  }
  const int post_rack = rack_size + bag_size - tiles_played;
  return post_rack >= 5 ? 0 : 2; // capable-emp : incapable-emp
}

// Generalized opp-perception opp-POV state: the perceived pool has
// opp_type_counts[t] tiles of type opp_types[t]; the opp-POV state says
// the bag holds opp_pov_bag_counts[t] of each type and the mover's rack
// holds the rest (opp_type_counts[t] - opp_pov_bag_counts[t]). For
// n_bag_now == 1 this degenerates to the single-tile case the
// PegOppInnerJob path supports today.
typedef struct PegOppPovJob {
  const Game *base_game; // walker state (RO, shared)
  int mover_idx;
  int ld_size;
  const Move *opp_move; // shared, RO
  int n_opp_types;
  const MachineLetter *opp_types; // shared, RO
  const int *opp_type_counts;     // total perceived-pool counts (RO)
  const int *opp_pov_bag_counts; // bag composition under this opp_pov_game (RO)
  ThreadControl *thread_control;
  // Outer ctx is borrowed for budget plumbing (deadline_monotonic_ns,
  // budget_timer/secs) when the inner-endgame path is selected. RO.
  const struct PegEnumCtx *outer_ctx;
  // Optional per-inner opp-POV TSV context: when outer_ctx->inner_tsv_f is set,
  // these annotate the row with the outer realized scenario and the opp
  // cand's text/score so the consumer can group by (cand, scenario, opp).
  const char *outer_drawn_str;
  const char *outer_remaining_str;
  const char *opp_move_text;
  int opp_move_score;
  int64_t opp_pov_weight;
  int32_t mover_total; // output (realized mt under this opp_pov_game)
  bool incomplete;     // output: depth=-1 (eg never iterated)
} PegOppPovJob;

// Per-call timing log captured via endgame_solve's per_ply_callback. Used
// by both the K<N walker leaf (peg_opp_pov_worker_fn) and the K=N bag-emptier
// path (peg_emit_split). The callback fires once per completed IDS depth
// on the first thread of the solver and records (depth, value, ms).
typedef struct PegInnerEgDepthLog {
  Timer call_timer;
  int n;
  int depths[32];
  int32_t values[32];
  double times_ms[32];
} PegInnerEgDepthLog;

static void peg_inner_eg_per_ply_cb(int depth, int32_t value,
                                    const struct PVLine *pv_line,
                                    const struct Game *game,
                                    const struct PVLine *ranked_pvs,
                                    int num_ranked_pvs, void *user_data) {
  (void)pv_line;
  (void)game;
  (void)ranked_pvs;
  (void)num_ranked_pvs;
  PegInnerEgDepthLog *log = (PegInnerEgDepthLog *)user_data;
  if (log->n < 32) {
    log->depths[log->n] = depth;
    log->values[log->n] = value;
    log->times_ms[log->n] = ctimer_elapsed_seconds(&log->call_timer) * 1000.0;
    log->n++;
  }
}

static void peg_opp_pov_worker_fn(void *arg, int worker_idx);
static void peg_eval_opp_with_perception(
    const PegEnumCtx *outer_ctx, const Game *walker, const Move *opp_move,
    const MachineLetter *opp_types, const int *opp_type_counts, int n_opp_types,
    int n_bag_now, const int *realized_bag_counts, double alpha,
    double *out_utility, int32_t *out_realized_mt, const char *outer_drawn_str,
    const char *outer_remaining_str, const char *opp_move_text,
    int opp_move_score);

// Evaluate ONE bag-tile ordering at depth=0: build the post-cand game with
// `iter_perm` as the residual bag, run the greedy playout (or endgame_solve
// for an emptier when PASSPEG_EMPTIER_USE_ENDGAME is set), and aggregate
// the realized mover_total into ctx->res under res_mutex. This is the leaf
// of the d=0 search — extracted so that ordering-grained parallel jobs and
// the serial in-loop path share a single implementation.
//
// `mover_drawn` (length mover_drawn_n) is the labeled tile array the mover
// drew this scenario. `drawn_str` is its canonical short form (for TSV).
// `n_bag_perm` is the number of meaningful entries in `iter_perm`; zero
// for emptiers (in which case bag is already empty post-cand).
// Compute the time still available under our Peg budget. Returns 0 when
// no budget is set (= unlimited for downstream calls). When the budget is
// already exhausted returns 0.001 (1 ms) so an endgame_solve call returns
// promptly with whatever partial result it has rather than running cold.
static double peg_remaining_budget_secs(const PegEnumCtx *ctx) {
  if (ctx->budget_timer == NULL || ctx->budget_secs <= 0.0) {
    return 0.0;
  }
  const double elapsed = ctimer_elapsed_seconds(ctx->budget_timer);
  const double remaining = ctx->budget_secs - elapsed;
  if (remaining <= 0.001) {
    return 0.001;
  }
  return remaining;
}

// Pick the earlier of a per-call deadline and the cascade deadline. Either
// may be 0 (meaning "no deadline"); the result is "no deadline" only if
// both are 0. Used to ensure endgame_solve calls inside the bench never
// outlive the cascade's overall budget — without this, a single
// endgame_solve at depth=4 can blow a 32s budget by 30s+ because the
// per-call wall budget (inner_eg_budget or kn_budget) is independent of
// the cascade's deadline_monotonic_ns.
static int64_t peg_clamp_deadline_ns(int64_t per_call_deadline_ns,
                                     int64_t cascade_deadline_ns) {
  if (cascade_deadline_ns <= 0) {
    return per_call_deadline_ns;
  }
  if (per_call_deadline_ns <= 0) {
    return cascade_deadline_ns;
  }
  return per_call_deadline_ns < cascade_deadline_ns ? per_call_deadline_ns
                                                    : cascade_deadline_ns;
}

static void peg_eval_d0_ordering(const PegEnumCtx *ctx,
                                 const MachineLetter *mover_drawn,
                                 int mover_drawn_n, const char *drawn_str,
                                 const MachineLetter *iter_perm, int n_bag_perm,
                                 int64_t this_weight) {
  (void)mover_drawn_n; // implied by ctx->k_drawn
  char perm_remaining_str[32] = {0};
  for (int i = 0; i < n_bag_perm && i < 30; i++) {
    perm_remaining_str[i] = ctx->ld->ld_ml_to_hl[iter_perm[i]][0];
  }
  Game *perm_game = peg_make_post_cand_game(
      ctx->base_game, ctx->mover_idx, ctx->unseen, ctx->ld_size, ctx->cand,
      ctx->k_drawn, mover_drawn, n_bag_perm, iter_perm);
  char perm_post_cgp[512] = {0};
  if (ctx->tsv_f) {
    char *cgp = game_get_cgp(perm_game, true);
    (void)snprintf(perm_post_cgp, sizeof(perm_post_cgp), "%s", cgp ? cgp : "");
    free(cgp);
  }
  char perm_pv[1024] = {0};
  char perm_final_cgp[512] = {0};
  char perm_mover_rack[32] = {0};
  char perm_opp_rack[32] = {0};
  int32_t perm_mt = 0;
  // Per-scenario timing for the inner TSV. Captures both the greedy d=0
  // path and the optional EMPTIER endgame fallback so the report can show
  // first-pass wall time per scenario.
  double scen_start_ms = 0.0;
  double scen_dur_ms = 0.0;
  int scen_eg_plies = 0; // 0 == pure greedy path
  int scen_eg_depth = 0;
  int scen_eg_num_moves = 0;
  PegInnerEgDepthLog scen_log = {0};
  if (ctx->inner_tsv_f) {
    if (ctx->budget_timer) {
      scen_start_ms = ctimer_elapsed_seconds(ctx->budget_timer) * 1000.0;
    }
    ctimer_start(&scen_log.call_timer);
  }
  const char *empty_eg_env = getenv("PASSPEG_EMPTIER_USE_ENDGAME");
  const int empty_eg_plies =
      empty_eg_env && *empty_eg_env ? passpeg_str_to_int(empty_eg_env) : 0;
  if (n_bag_perm == 0 && empty_eg_plies > 0 &&
      game_get_game_end_reason(perm_game) == GAME_END_REASON_NONE) {
    const int32_t mover_lead =
        equity_to_int(
            player_get_score(game_get_player(perm_game, ctx->mover_idx))) -
        equity_to_int(
            player_get_score(game_get_player(perm_game, 1 - ctx->mover_idx)));
    EndgameCtx *eg_ctx = NULL;
    EndgameResults *eg_results = endgame_results_create();
    EndgameArgs ea = {
        .thread_control = ctx->thread_control,
        .game = perm_game,
        .plies = empty_eg_plies,
        .shared_tt = NULL,
        .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
        .num_threads = 1,
        .use_heuristics = true,
        .num_top_moves = 1,
        .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
        .skip_word_pruning = true,
        .soft_time_limit = peg_remaining_budget_secs(ctx),
        .hard_time_limit = peg_remaining_budget_secs(ctx),
        .external_deadline_ns = ctx->deadline_monotonic_ns,
    };
    endgame_solve_inline(&eg_ctx, &ea, eg_results);
    const int turn = game_get_player_on_turn_index(perm_game);
    const int32_t eg_val =
        endgame_results_get_value(eg_results, ENDGAME_RESULT_BEST);
    perm_mt =
        (turn == ctx->mover_idx) ? mover_lead + eg_val : mover_lead - eg_val;
    scen_eg_plies = empty_eg_plies;
    scen_eg_depth = endgame_results_get_depth(eg_results, ENDGAME_RESULT_BEST);
    if (ctx->inner_tsv_f) {
      const PVLine *pv =
          endgame_results_get_pvline(eg_results, ENDGAME_RESULT_BEST);
      if (pv) {
        scen_eg_num_moves = pv->num_moves;
      }
      if (pv && pv->num_moves > 0) {
        Game *pv_g = game_duplicate(perm_game);
        game_set_endgame_solving_mode(pv_g);
        game_set_backup_mode(pv_g, BACKUP_MODE_OFF);
        StringBuilder *psb = string_builder_create();
        for (int mi = 0; mi < pv->num_moves; mi++) {
          Move mf;
          small_move_to_move(&mf, &pv->moves[mi], game_get_board(pv_g));
          if (mi > 0) {
            string_builder_add_string(psb, " | ");
          }
          string_builder_add_move(psb, game_get_board(pv_g), &mf,
                                  game_get_ld(pv_g), true);
          play_move(&mf, pv_g, NULL);
        }
        (void)snprintf(perm_pv, sizeof(perm_pv), "%s",
                       string_builder_peek(psb));
        string_builder_destroy(psb);
        game_destroy(pv_g);
      }
    }
    endgame_ctx_destroy(eg_ctx);
    endgame_results_destroy(eg_results);
  } else {
    // Capacity 1: greedy playout uses MOVE_RECORD_BEST which only ever
    // holds one move (best so far) + spare_move. The previous 4096
    // allocated ~hundreds of KB per playout for slots that were never
    // touched.
    MoveList *playout_ml = move_list_create(1);
    const bool need_pv = ctx->tsv_f || ctx->inner_tsv_f;
    perm_mt = peg_greedy_playout_pv(
        perm_game, ctx->mover_idx, playout_ml, ctx->worker_idx,
        need_pv ? perm_pv : NULL, sizeof(perm_pv),
        need_pv ? perm_mover_rack : NULL, sizeof(perm_mover_rack),
        need_pv ? perm_opp_rack : NULL, sizeof(perm_opp_rack),
        ctx->tsv_f ? perm_final_cgp : NULL, sizeof(perm_final_cgp));
    move_list_destroy(playout_ml);
    // scen_eg_plies stays 0 → "pure greedy path" in the report.
  }
  if (ctx->inner_tsv_f) {
    scen_dur_ms = ctimer_elapsed_seconds(&scen_log.call_timer) * 1000.0;
  }
  game_destroy(perm_game);

  peg_lock(ctx->res_mutex);
  ctx->res->weight_sum += this_weight;
  ctx->res->spread_sum += this_weight * (int64_t)perm_mt;
  if (perm_mt > 0) {
    ctx->res->win_x2 += 2 * this_weight;
  } else if (perm_mt == 0) {
    ctx->res->win_x2 += this_weight;
  }
  ctx->res->n_scen++;
  peg_unlock(ctx->res_mutex);

  if (ctx->tsv_f) {
    peg_lock(ctx->tsv_mutex);
    (void)fprintf(ctx->tsv_f,
                  "%d\t%s\t%d\t%d\t%d\t%s\t%s\t%lld\t%d\t%s\t%s\t%s\t%s\t%s\n",
                  ctx->pos_idx, ctx->cand_txt, ctx->cand_score,
                  ctx->k_drawn + n_bag_perm, ctx->k_drawn, drawn_str,
                  perm_remaining_str, (long long)this_weight, (int)perm_mt,
                  perm_post_cgp, perm_final_cgp, perm_pv, perm_mover_rack,
                  perm_opp_rack);
    peg_unlock(ctx->tsv_mutex);
  }
  // Per-scenario row to the inner TSV — same schema as the K=N/K<N
  // endgame rows but with opp_pov_bag = "" and (for the pure greedy first
  // pass) eg_plies = 0, eg_depth = 0. Lets the report show first-pass
  // timing for every scenario.
  if (ctx->inner_tsv_f) {
    peg_lock(ctx->inner_tsv_mutex);
    (void)fprintf(ctx->inner_tsv_f,
                  "%s\t%s\t%s\t%s\t%d\t%s\t%lld\t%d\t%d\t%d\t%d\t%d\t%d"
                  "\t%.1f\t%.1f\t%s\t%s\t%s\n",
                  ctx->cand_txt ? ctx->cand_txt : "", drawn_str,
                  perm_remaining_str,
                  "", // opp_move — none at d=0 (greedy plays it out)
                  0,  // opp_score
                  "", // opp_pov_bag — no perception at d=0
                  (long long)this_weight, (int)perm_mt, scen_eg_plies,
                  scen_eg_depth, 0, scen_eg_num_moves, 0, scen_start_ms,
                  scen_dur_ms, "" /* depth_log */, perm_mover_rack, perm_pv);
    peg_unlock(ctx->inner_tsv_mutex);
  }
}

// peg_emit_split is a ported research emitter; its size is inherent to the
// single-pass TSV/CGP layout it produces.
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size,hicpp-function-size)
static void peg_emit_split(const PegEnumCtx *ctx) {
  MachineLetter mover_drawn[16];
  MachineLetter bag_remaining[16];
  int mover_drawn_n = 0;
  int bag_remaining_n = 0;
  char drawn_str[32] = {0};
  char remaining_str[32] = {0};
  int drawn_len = 0;
  int remaining_len = 0;
  for (int type_idx = 0; type_idx < ctx->k_types; type_idx++) {
    const int mover_count = ctx->mover_pick[type_idx];
    const int bag_count = ctx->n_multiset[type_idx] - mover_count;
    for (int k = 0; k < mover_count; k++) {
      mover_drawn[mover_drawn_n++] = ctx->types[type_idx];
      if (drawn_len < 30) {
        drawn_str[drawn_len++] = ctx->ld->ld_ml_to_hl[ctx->types[type_idx]][0];
      }
    }
    for (int k = 0; k < bag_count; k++) {
      bag_remaining[bag_remaining_n++] = ctx->types[type_idx];
      if (remaining_len < 30) {
        remaining_str[remaining_len++] =
            ctx->ld->ld_ml_to_hl[ctx->types[type_idx]][0];
      }
    }
  }
  // Apply scenario filter (skip silently if no match — the cand's
  // aggregated stats reflect only the scenarios we evaluated).
  if (!peg_scenario_filter_match(ctx->scenario_filter, drawn_str,
                                 remaining_str)) {
    return;
  }

  // Collect-mode: defer evaluation by pushing job(s) onto out_jobs.
  // Two paths:
  //   depth == 0: do stride + ordering walk inline (single-threaded; the
  //     enumerator IS the one caller) and push ONE job per (multiset,
  //     ordering). Workers later evaluate one ordering each — gives
  //     ordering-grained parallelism (up to N_bag! per multiset).
  //   depth >= 1: push one multiset job; the worker calls back into
  //     peg_emit_split (with out_jobs=NULL) to run the opp-top-K +
  //     endgame_solve flow. At d>=1 the bag is empty at the leaf so
  //     there is no ordering walk to split.
  // Collect-mode: defer evaluation by pushing a (multiset, mover_pick)
  // job onto out_jobs. The dispatcher hands it to a worker which calls
  // emit_split again with out_jobs=NULL — at d=0 the worker walks bag-tile
  // orderings serially via peg_eval_d0_ordering, at d>=1 it runs the
  // opp-top-K + endgame_solve flow.
  //
  // We tried splitting d=0 jobs one-per-ordering for finer parallelism but
  // it cost ~13% wall on POND 4peg — job overhead beat the gain. Multiset
  // granularity is the right fit for ~50-250 us scenarios.
  if (ctx->out_jobs) {
    PegScenarioJob job;
    job.base_ctx = ctx;
    job.n_multiset = malloc_or_die((size_t)ctx->k_types * sizeof(int));
    job.mover_pick = malloc_or_die((size_t)ctx->k_types * sizeof(int));
    memcpy(job.n_multiset, ctx->n_multiset, (size_t)ctx->k_types * sizeof(int));
    memcpy(job.mover_pick, ctx->mover_pick, (size_t)ctx->k_types * sizeof(int));
    peg_joblist_push(ctx->out_jobs, job);
    return;
  }

  // Ordered-pair weight: number of labeled-tile sequences in which mover
  // first draws k_drawn tiles (in order), then bag_remaining is the bag's
  // residual (the walker further iterates its orderings as sub-scenarios).
  // For mover's draw the type-multiset (m_t) admits k_drawn! orderings of
  // the labeled tiles, so the per-multiset weight is
  //   k_drawn! × ∏_t C(c_t, m_t) × C(c_t − m_t, b_t)
  // Total over all multisets sums to P(N, k_drawn + n_bag_remaining), the
  // ordered-pair basis macondo/peg.c use.
  int64_t weight = 1;
  for (int type_idx = 0; type_idx < ctx->k_types; type_idx++) {
    const int total_count = ctx->type_counts[type_idx];
    const int mover_count = ctx->mover_pick[type_idx];
    const int bag_count = ctx->n_multiset[type_idx] - mover_count;
    weight *= peg_binomial(total_count, mover_count) *
              peg_binomial(total_count - mover_count, bag_count);
  }
  for (int f = 2; f <= ctx->k_drawn; f++) {
    weight *= f;
  }

  // PASSPEG_SCENARIO_STRIDE=k: stratified sampling on the conceptual
  // weight-unit-expanded job list. Each multiset of weight w contributes
  // w "weight-units" to a running tally. We sample a multiset if and only
  // if at least one stride-boundary (multiple of k) falls inside its
  // weight range; the # of samples taken from that multiset = how many
  // boundaries it covers. Each sampled multiset is evaluated once with
  // weight = samples × k so the expected aggregate weight is preserved.
  // Default k=1 (no sampling). Applies at every level (outer + inner).
  //
  // Bag-size gate: when the root bag is 1 or 2 (1peg/2peg), the scenario
  // space is still small enough to enumerate fully — stride sampling on
  // that tiny space throws away crucial coverage. Disable stride when
  // N (= k_drawn + n_bag_remaining) <= 2; only enable for 3peg+.
  {
    const int n_root = ctx->k_drawn + ctx->n_bag_remaining;
    const char *stride_env = getenv("PASSPEG_SCENARIO_STRIDE");
    const int stride_req =
        stride_env && *stride_env ? passpeg_str_to_int(stride_env) : 1;
    const int stride = (n_root >= 3) ? stride_req : 1;
    if (stride > 1 && ctx->res) {
      peg_lock(ctx->res_mutex);
      const int64_t old_seen = ctx->res->sampled_weight_seen;
      ctx->res->sampled_weight_seen += weight;
      peg_unlock(ctx->res_mutex);
      const int64_t samples =
          (old_seen + weight) / (int64_t)stride - old_seen / (int64_t)stride;
      if (samples == 0) {
        return; // no stride boundary fell within this multiset's weight band
      }
      weight = samples * (int64_t)stride;
    }
  }

  // At d=0 the per-ordering helper builds its own perm_game; the shared
  // post-cand game is only consumed by the d>=1 branches below. Skip the
  // allocation at d=0 — for collect mode this avoids ~K_jobs wasted
  // game_duplicate calls; for execute mode it's a minor saving.
  Game *game = NULL;
  char post_cand_cgp[512] = {0};
  if (ctx->depth >= 1) {
    game = peg_make_post_cand_game(
        ctx->base_game, ctx->mover_idx, ctx->unseen, ctx->ld_size, ctx->cand,
        ctx->k_drawn, mover_drawn, ctx->n_bag_remaining, bag_remaining);
    if (ctx->tsv_f) {
      char *cgp = game_get_cgp(game, true);
      (void)snprintf(post_cand_cgp, sizeof(post_cand_cgp), "%s",
                     cgp ? cgp : "");
      free(cgp);
    }
  }

  int32_t mover_total = 0;
  // Large enough to hold opp_move_text (<=63) + " | " + a full branch_pv
  // (<=1023) without truncation; see the snprintf into pv_text below.
  char pv_text[1280] = {0};
  char final_cgp[512] = {0};
  char mover_rack_end[32] = {0};
  char opp_rack_end[32] = {0};

  if (ctx->depth == 0) {
    // Pure greedy from post-cand: opp + mover both greedy to game end.
    // Walk distinct lex orderings of bag_remaining so each physical draw
    // order contributes its own sub-scenario (matching the walker's
    // d>=1 behavior). Per-perm aggregation here; early-return to skip
    // the bottom single-mt fold.
    //
    // PASSPEG_WALK_SAMPLE=N: instead of walking all K!/∏b_t! distinct
    // orderings per multiset, sample N cyclic rotations of the sorted
    // bag_remaining (= [0, 1, ..., min(N, K)-1]-shift) and scale per-
    // sample weight by full_orderings/N so the per-multiset total
    // matches a full walk.
    MachineLetter perm[16];
    for (int i = 0; i < ctx->n_bag_remaining; i++) {
      perm[i] = bag_remaining[i];
    }
    for (int i = 1; i < ctx->n_bag_remaining; i++) {
      for (int j = i; j > 0 && perm[j] < perm[j - 1]; j--) {
        const MachineLetter tmp = perm[j];
        perm[j] = perm[j - 1];
        perm[j - 1] = tmp;
      }
    }
    // Two ways to sub-sample the ordering walk (independent of
    // PASSPEG_SCENARIO_STRIDE):
    //   PASSPEG_WALK_SAMPLE=N    — take EXACTLY N cyclic rotations of the
    //                                sorted perm per multiset.
    //   PASSPEG_WALK_STRIDE=k    — take ~1/k of orderings per multiset
    //                                (n_to_sample = ceil(n_full / k)).
    // Both reuse the same cyclic-rotation sampler: deterministic,
    // per-multiset, no cross-multiset coordination -> no mutex needed.
    // If both env vars are set, WALK_SAMPLE wins (explicit count).
    const char *sample_env = getenv("PASSPEG_WALK_SAMPLE");
    const int sample_count =
        sample_env && *sample_env ? passpeg_str_to_int(sample_env) : 0;
    const char *walk_stride_env = getenv("PASSPEG_WALK_STRIDE");
    const int walk_stride = walk_stride_env && *walk_stride_env
                                ? passpeg_str_to_int(walk_stride_env)
                                : 1;
    const bool sample_mode =
        ctx->n_bag_remaining >= 2 && (sample_count > 0 || walk_stride > 1);
    int64_t per_sample_weight = weight;
    int n_to_sample = 1;
    if (sample_mode) {
      int b_counts[MAX_ALPHABET_SIZE] = {0};
      for (int i = 0; i < ctx->n_bag_remaining; i++) {
        b_counts[perm[i]]++;
      }
      int64_t k_fact = 1;
      for (int k = 2; k <= ctx->n_bag_remaining; k++) {
        k_fact *= k;
      }
      int64_t prod_b_fact = 1;
      for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
        if (b_counts[ml] > 1) {
          int64_t f = 1;
          for (int k = 2; k <= b_counts[ml]; k++) {
            f *= k;
          }
          prod_b_fact *= f;
        }
      }
      const int64_t n_full_orderings = k_fact / prod_b_fact;
      if (sample_count > 0) {
        n_to_sample = sample_count < (int)n_full_orderings
                          ? sample_count
                          : (int)n_full_orderings;
      } else {
        // walk_stride > 1: pick ceil(n_full / stride), at least 1.
        const int64_t s = (n_full_orderings + (int64_t)walk_stride - 1) /
                          (int64_t)walk_stride;
        if (s < 1) {
          n_to_sample = 1;
        } else if (s > n_full_orderings) {
          n_to_sample = (int)n_full_orderings;
        } else {
          n_to_sample = (int)s;
        }
      }
      per_sample_weight =
          (weight * n_full_orderings + n_to_sample / 2) / n_to_sample;
    }
    int sample_idx = 0;
    do {
      if (sample_mode && sample_idx >= n_to_sample) {
        break;
      }
      // In sample mode, build the cyclic-rotated perm for this sample.
      MachineLetter eff_perm[16];
      if (sample_mode) {
        for (int i = 0; i < ctx->n_bag_remaining; i++) {
          eff_perm[i] = perm[(i + sample_idx) % ctx->n_bag_remaining];
        }
      } else {
        for (int i = 0; i < ctx->n_bag_remaining; i++) {
          eff_perm[i] = perm[i];
        }
      }
      sample_idx++;
      const int64_t this_weight = sample_mode ? per_sample_weight : weight;
      const MachineLetter *iter_perm = sample_mode ? eff_perm : perm;
      peg_eval_d0_ordering(ctx, mover_drawn, mover_drawn_n, drawn_str,
                           iter_perm, ctx->n_bag_remaining, this_weight);
    } while ((sample_mode && sample_idx < n_to_sample) ||
             (!sample_mode && peg_next_perm(perm, ctx->n_bag_remaining)));
    return;
  }
  if (ctx->n_bag_remaining == 0) {
    // Bag-emptier: the bag is empty, both racks are determined, and opp is
    // on turn. Stage n => n "informed, rational" plies of negamax. We
    // delegate to endgame_solve(plies=depth); it finds opp's true best
    // move and plays out optimally for `depth` plies, heuristic-leafing
    // the rest.
    const int32_t mover_lead =
        equity_to_int(player_get_score(game_get_player(game, ctx->mover_idx))) -
        equity_to_int(
            player_get_score(game_get_player(game, 1 - ctx->mover_idx)));
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      mover_total = mover_lead;
    } else {
      EndgameCtx *eg_ctx = NULL;
      EndgameResults *eg_results = endgame_results_create();
      // Per-call timing log so the inner-TSV report can show when each
      // IDS depth was reached (same instrumentation as the K<N walker path).
      PegInnerEgDepthLog kn_log = {0};
      double kn_start_ms = 0.0;
      if (ctx->budget_timer) {
        kn_start_ms = ctimer_elapsed_seconds(ctx->budget_timer) * 1000.0;
      }
      ctimer_start(&kn_log.call_timer);
      // K=N (bag-emptier) endgame: plies driven by the current stage
      // depth (so stage 1 = 1-ply, stage 2 = 2-ply, etc.) unless the
      // user overrides via PASSPEG_INNER_EG_PLIES. Optional wall-cap
      // via PASSPEG_INNER_EG_BUDGET (default 0 = no cap — let the
      // small N-ply IDS finish naturally).
      const char *kn_budget_env = getenv("PASSPEG_INNER_EG_BUDGET");
      const double kn_budget = (kn_budget_env && *kn_budget_env)
                                   ? passpeg_str_to_double(kn_budget_env)
                                   : 0.0;
      const char *kn_plies_env = getenv("PASSPEG_INNER_EG_PLIES");
      const int kn_plies = (kn_plies_env && *kn_plies_env)
                               ? passpeg_str_to_int(kn_plies_env)
                               : ctx->depth;
      EndgameArgs ea = {
          .thread_control = ctx->thread_control,
          .game = game,
          .plies = kn_plies,
          .shared_tt = ctx->shared_eg_tt,
          .initial_small_move_arena_size =
              DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
          .num_threads = 1,
          .use_heuristics = true,
          .num_top_moves = 1,
          .per_ply_callback = ctx->inner_tsv_f ? peg_inner_eg_per_ply_cb : NULL,
          .per_ply_callback_data = ctx->inner_tsv_f ? (void *)&kn_log : NULL,
          .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
          .skip_word_pruning = true,
          .soft_time_limit = kn_budget,
          .hard_time_limit = kn_budget,
          // Clamp by the cascade deadline so a bag-empty K=N endgame at
          // ctx->depth plies can never outlive the outer wall budget.
          // Without this clamp, default kn_budget=0 leaves
          // external_deadline_ns=0 and endgame_solve runs unbounded — a single
          // pos 61 d=4 scenario hangs the cascade past the 60s bp_exec
          // watchdog.
          .external_deadline_ns = peg_clamp_deadline_ns(
              kn_budget > 0.0
                  ? ctimer_monotonic_ns() + (int64_t)(kn_budget * 1.0e9)
                  : 0,
              ctx->deadline_monotonic_ns),
      };
      endgame_solve_inline(&eg_ctx, &ea, eg_results);
      const double kn_dur_ms =
          ctimer_elapsed_seconds(&kn_log.call_timer) * 1000.0;
      // endgame_solve returns the value from the player-on-turn's POV
      // (here: opp). Negate to express it from mover's POV, then add to
      // mover_lead to get mover_total.
      //
      // If the cascade deadline fired before depth 1 completed, the result
      // value/PV are uninitialized — using them downstream blows up on
      // equity_to_int / "duplicate move type" assertions. Fall back to a
      // greedy playout so we always return a sane number.
      const int kn_depth_reached =
          endgame_results_get_depth(eg_results, ENDGAME_RESULT_BEST);
      if (kn_depth_reached < 0) {
        MoveList *pl_fb = move_list_create(1);
        mover_total =
            peg_greedy_playout_pv(game, ctx->mover_idx, pl_fb, ctx->worker_idx,
                                  NULL, 0, NULL, 0, NULL, 0, NULL, 0);
        move_list_destroy(pl_fb);
      } else {
        const int32_t opp_pov_val =
            endgame_results_get_value(eg_results, ENDGAME_RESULT_BEST);
        mover_total = mover_lead - opp_pov_val;
      }
      // Emit a row to the inner TSV so the report can show this cand's
      // K=N evaluation alongside the K<N walker rows. We synthesize a
      // single "row per scenario" — there's no opp_move iteration here
      // (the endgame picks opp's best move internally). Skip the PV walk
      // when no depth completed — the SmallMove entries may be partial
      // and tripping play_move asserts ("duplicate move type").
      if (ctx->inner_tsv_f) {
        const int kn_depth =
            endgame_results_get_depth(eg_results, ENDGAME_RESULT_BEST);
        char kn_pv_text[1024] = {0};
        int kn_pv_num_moves = 0;
        int kn_pv_negamax = 0;
        const PVLine *pv_line =
            kn_depth_reached >= 0
                ? endgame_results_get_pvline(eg_results, ENDGAME_RESULT_BEST)
                : NULL;
        if (pv_line) {
          kn_pv_num_moves = pv_line->num_moves;
          kn_pv_negamax = pv_line->negamax_depth;
          if (pv_line->num_moves > 0) {
            Game *pv_g = game_duplicate(game);
            game_set_endgame_solving_mode(pv_g);
            game_set_backup_mode(pv_g, BACKUP_MODE_OFF);
            StringBuilder *psb = string_builder_create();
            for (int mi = 0; mi < pv_line->num_moves; mi++) {
              Move mf;
              small_move_to_move(&mf, &pv_line->moves[mi],
                                 game_get_board(pv_g));
              if (mi > 0) {
                string_builder_add_string(psb, " | ");
              }
              string_builder_add_move(psb, game_get_board(pv_g), &mf,
                                      game_get_ld(pv_g), true);
              play_move(&mf, pv_g, NULL);
            }
            (void)snprintf(kn_pv_text, sizeof(kn_pv_text), "%s",
                           string_builder_peek(psb));
            string_builder_destroy(psb);
            game_destroy(pv_g);
          }
        }
        char kn_dlog[256] = {0};
        int kn_off = 0;
        for (int i = 0; i < kn_log.n && kn_off + 24 < (int)sizeof(kn_dlog);
             i++) {
          kn_off +=
              snprintf(kn_dlog + kn_off, sizeof(kn_dlog) - kn_off,
                       "%s%d@%.1f=%+d", i == 0 ? "" : ";", kn_log.depths[i],
                       kn_log.times_ms[i], (int)kn_log.values[i]);
        }
        // K=N path: mover rack is on `game` (mover already played the cand
        // and drew); render it for the report column.
        char kn_mover_rack[32] = {0};
        {
          const LetterDistribution *ld = game_get_ld(game);
          StringBuilder *rsb = string_builder_create();
          string_builder_add_rack(
              rsb, player_get_rack(game_get_player(game, ctx->mover_idx)), ld,
              false);
          (void)snprintf(kn_mover_rack, sizeof(kn_mover_rack), "%s",
                         string_builder_peek(rsb));
          string_builder_destroy(rsb);
        }
        peg_lock(ctx->inner_tsv_mutex);
        (void)fprintf(ctx->inner_tsv_f,
                      "%s\t%s\t%s\t%s\t%d\t%s\t%lld\t%d\t%d\t%d\t%d\t%d\t%d"
                      "\t%.1f\t%.1f\t%s\t%s\t%s\n",
                      ctx->cand_txt ? ctx->cand_txt : "", drawn_str,
                      "",  // remaining_str — empty for bag-emptier
                      "",  // opp_move — not selected separately
                      0,   // opp_score
                      "",  // opp_pov_bag — no perception
                      0LL, // opp_pov_weight
                      (int)mover_total, ctx->depth, kn_depth, 0,
                      kn_pv_num_moves, kn_pv_negamax, kn_start_ms, kn_dur_ms,
                      kn_dlog, kn_mover_rack, kn_pv_text);
        peg_unlock(ctx->inner_tsv_mutex);
        // With per-call budget the K=N endgame should always reach at
        // least depth 1. If it didn\'t, mover_total = mover_lead (a
        // valid but uninformative number); we no longer drop the cand —
        // the user\'s design is "return the best result when
        // interrupted", and partial endgame data is still a real signal.
      }
      if (ctx->tsv_f) {
        const PVLine *pv =
            endgame_results_get_pvline(eg_results, ENDGAME_RESULT_BEST);
        if (pv && pv->num_moves > 0) {
          Game *pv_game = game_duplicate(game);
          game_set_endgame_solving_mode(pv_game);
          game_set_backup_mode(pv_game, BACKUP_MODE_OFF);
          StringBuilder *pv_sb = string_builder_create();
          for (int mi = 0; mi < pv->num_moves; mi++) {
            Move m_full;
            small_move_to_move(&m_full, &pv->moves[mi],
                               game_get_board(pv_game));
            if (mi > 0) {
              string_builder_add_string(pv_sb, " | ");
            }
            string_builder_add_move(pv_sb, game_get_board(pv_game), &m_full,
                                    game_get_ld(pv_game), true);
            play_move(&m_full, pv_game, NULL);
          }
          (void)snprintf(pv_text, sizeof(pv_text), "%s",
                         string_builder_peek(pv_sb));
          string_builder_destroy(pv_sb);
          char *cgp = game_get_cgp(pv_game, true);
          (void)snprintf(final_cgp, sizeof(final_cgp), "%s", cgp ? cgp : "");
          free(cgp);
          StringBuilder *rsb = string_builder_create();
          string_builder_add_rack(
              rsb, player_get_rack(game_get_player(pv_game, ctx->mover_idx)),
              game_get_ld(pv_game), false);
          (void)snprintf(mover_rack_end, sizeof(mover_rack_end), "%s",
                         string_builder_peek(rsb));
          string_builder_destroy(rsb);
          StringBuilder *rsb2 = string_builder_create();
          string_builder_add_rack(
              rsb2,
              player_get_rack(game_get_player(pv_game, 1 - ctx->mover_idx)),
              game_get_ld(pv_game), false);
          (void)snprintf(opp_rack_end, sizeof(opp_rack_end), "%s",
                         string_builder_peek(rsb2));
          string_builder_destroy(rsb2);
          game_destroy(pv_game);
        }
      }
      endgame_ctx_destroy(eg_ctx);
      endgame_results_destroy(eg_results);
    }
  } else {
    // d>=1, n_bag_remaining >= 1: enumerate opp's top-K moves, apply each,
    // then evaluate the resulting position via endgame_solve at `depth`
    // plies. Take MIN over branches.
    MoveList *opp_ml = move_list_create(16384);
    const MoveGenArgs opp_ga = {
        .game = game,
        .move_list = opp_ml,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&opp_ga);
    const int n_opp = move_list_get_count(opp_ml);

    // One-shot opp utility ranking, per the "rational opp" model. For
    // each opp tile-placement move, evaluate it across all of opp's
    // perceived bag-tile scenarios (= the distinct types in opp's unseen
    // pool, which is mover_rack + bag from opp's POV), do a greedy
    // playout for each, and aggregate into opp's win% / mean spread.
    // opp utility = win_pct + alpha * mean_spread (alpha = env value).
    // Triggered when PASSPEG_GREEDY_OPP_UTIL is set (its value = alpha).
    const char *opp_env = getenv("PASSPEG_GREEDY_OPP_UTIL");
    if (opp_env && *opp_env) {
      const double alpha = passpeg_str_to_double(opp_env);
      // If PASSPEG_GREEDY_OPP_DEPTH > 0, replace the per-opp-POV-
      // scenario greedy playout with endgame_solve at that ply depth.
      // The bag is empty after opp plays + draws the 1 bag tile, so
      // endgame_solve is well-defined.
      const char *opp_depth_env = getenv("PASSPEG_GREEDY_OPP_DEPTH");
      const int opp_depth = opp_depth_env && *opp_depth_env
                                ? passpeg_str_to_int(opp_depth_env)
                                : 0;
      const int opp_idx = 1 - ctx->mover_idx;
      uint8_t opp_unseen[MAX_ALPHABET_SIZE];
      peg_compute_unseen(game, opp_idx, opp_unseen);
      MachineLetter opp_types[MAX_ALPHABET_SIZE];
      int opp_type_counts[MAX_ALPHABET_SIZE];
      int n_opp_types = 0;
      int opp_pool_total = 0;
      for (int ml = 0; ml < ctx->ld_size; ml++) {
        if (opp_unseen[ml] > 0) {
          opp_types[n_opp_types] = (MachineLetter)ml;
          opp_type_counts[n_opp_types] = (int)opp_unseen[ml];
          opp_pool_total += (int)opp_unseen[ml];
          n_opp_types++;
        }
      }
      (void)fprintf(stderr,
                    "[opp_util] scenario %s/%s  alpha=%g  opp_pool=%d tiles in "
                    "%d types\n",
                    drawn_str, remaining_str, alpha, opp_pool_total,
                    n_opp_types);

      typedef struct {
        int move_idx;
        double win_pct;
        double mean_spread;
        double utility;
      } OppRanked;
      OppRanked *ranked = calloc_or_die((size_t)n_opp, sizeof(OppRanked));
      int n_ranked = 0;

      // Collect tile-placement opp ranks; PASS/EXCHANGE are skipped.
      // If opp_move_filter is set, also restrict to the named opp moves
      // (substring match on movegen text). Lets the test do SH-style
      // ladders that re-rank only the previous round's survivors.
      int *placement_opp_ranks = malloc_or_die((size_t)n_opp * sizeof(int));
      int n_placement = 0;
      for (int opp_rank = 0; opp_rank < n_opp; opp_rank++) {
        const Move *opp_move = move_list_get_move(opp_ml, opp_rank);
        if (move_get_type(opp_move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
          continue;
        }
        if (ctx->opp_move_filter) {
          char text[64] = {0};
          StringBuilder *sb_m = string_builder_create();
          string_builder_add_move(sb_m, game_get_board(game), opp_move,
                                  game_get_ld(game), true);
          (void)snprintf(text, sizeof(text), "%s", string_builder_peek(sb_m));
          string_builder_destroy(sb_m);
          bool match = false;
          char tmp[2048];
          (void)snprintf(tmp, sizeof(tmp), "%s", ctx->opp_move_filter);
          const char *tok = strtok(tmp, ";");
          while (tok != NULL) {
            if (strstr(text, tok) != NULL) {
              match = true;
              break;
            }
            tok = strtok(NULL, ";");
          }
          if (!match) {
            continue;
          }
        }
        placement_opp_ranks[n_placement++] = opp_rank;
      }

      // Build one job per (opp_move, perceived_bag_tile) leaf. Flat
      // layout: jobs[pp * n_opp_types + ti] is the (placement_opp_ranks
      // [pp], opp_types[ti]) leaf.
      const int n_inner_jobs = n_placement > 0 ? n_placement * n_opp_types : 0;
      PegOppInnerJob *inner_jobs = NULL;
      void **inner_arg_ptrs = NULL;
      if (n_inner_jobs > 0) {
        inner_jobs =
            malloc_or_die((size_t)n_inner_jobs * sizeof(PegOppInnerJob));
        inner_arg_ptrs = malloc_or_die((size_t)n_inner_jobs * sizeof(void *));
        for (int pp = 0; pp < n_placement; pp++) {
          const int opp_rank = placement_opp_ranks[pp];
          const Move *opp_move = move_list_get_move(opp_ml, opp_rank);
          for (int ti = 0; ti < n_opp_types; ti++) {
            const int idx = pp * n_opp_types + ti;
            inner_jobs[idx] = (PegOppInnerJob){
                .base_game = game,
                .mover_idx = ctx->mover_idx,
                .ld_size = ctx->ld_size,
                .opp_move = opp_move,
                .bag_ml = opp_types[ti],
                .weight = opp_type_counts[ti],
                .n_opp_types = n_opp_types,
                .opp_types = opp_types,
                .opp_type_counts = opp_type_counts,
                .ti = ti,
                .opp_depth = opp_depth,
                .thread_control = ctx->thread_control,
                .deadline_monotonic_ns = ctx->deadline_monotonic_ns,
                .mover_total = 0,
            };
            inner_arg_ptrs[idx] = &inner_jobs[idx];
          }
        }
      }

      // Dispatch. With a shared executor we can submit nested work; the
      // help-while-waiting protocol on submit_and_wait lets the calling
      // worker continue draining the queue (no deadlock when nested
      // inner work is submitted from within an outer scenario worker).
      if (ctx->executor && n_inner_jobs > 0) {
        peg_pool_submit_and_wait(ctx->executor, peg_opp_inner_worker_fn,
                                 inner_arg_ptrs, n_inner_jobs, ctx->worker_idx);
      } else {
        for (int idx = 0; idx < n_inner_jobs; idx++) {
          peg_opp_inner_worker_fn(&inner_jobs[idx], ctx->worker_idx);
        }
      }

      // Aggregate per-opp_rank.
      for (int pp = 0; pp < n_placement; pp++) {
        const int opp_rank = placement_opp_ranks[pp];
        int64_t weight_sum = 0;
        double win_x2_sum = 0.0;
        int64_t spread_sum = 0;
        for (int ti = 0; ti < n_opp_types; ti++) {
          const int job_idx = pp * n_opp_types + ti;
          const int32_t opp_pov_mover_total = inner_jobs[job_idx].mover_total;
          const int job_weight = inner_jobs[job_idx].weight;
          weight_sum += job_weight;
          spread_sum += (int64_t)(-opp_pov_mover_total) * job_weight;
          if (opp_pov_mover_total < 0) {
            win_x2_sum += 2.0 * job_weight;
          } else if (opp_pov_mover_total == 0) {
            win_x2_sum += job_weight;
          }
        }
        const Move *opp_move = move_list_get_move(opp_ml, opp_rank);
        (void)opp_move; // used below via opp_rank
        const double opp_win_pct = win_x2_sum / (2.0 * (double)weight_sum);
        const double opp_mean_spread = (double)spread_sum / (double)weight_sum;
        const double utility = opp_win_pct + alpha * opp_mean_spread;
        ranked[n_ranked].move_idx = opp_rank;
        ranked[n_ranked].win_pct = opp_win_pct;
        ranked[n_ranked].mean_spread = opp_mean_spread;
        ranked[n_ranked].utility = utility;
        n_ranked++;
      }
      // Sort by utility descending.
      for (int i = 0; i < n_ranked; i++) {
        for (int j = i + 1; j < n_ranked; j++) {
          if (ranked[j].utility > ranked[i].utility) {
            OppRanked tmp = ranked[i];
            ranked[i] = ranked[j];
            ranked[j] = tmp;
          }
        }
      }
      const char *opp_topn_env = getenv("PASSPEG_GREEDY_OPP_TOPN");
      const int opp_topn =
          opp_topn_env && *opp_topn_env ? passpeg_str_to_int(opp_topn_env) : 20;
      (void)fprintf(stderr, "[opp_util] top-%d by utility:\n", opp_topn);
      for (int i = 0; i < n_ranked && i < opp_topn; i++) {
        const Move *m = move_list_get_move(opp_ml, ranked[i].move_idx);
        StringBuilder *sb = string_builder_create();
        string_builder_add_move(sb, game_get_board(game), m, game_get_ld(game),
                                true);
        (void)fprintf(stderr,
                      "  #%-3d  win=%.4f  spread=%+8.3f  util=%+9.5f  %s\n",
                      i + 1, ranked[i].win_pct, ranked[i].mean_spread,
                      ranked[i].utility, string_builder_peek(sb));
        string_builder_destroy(sb);
      }
      // Locate the target moves of interest (TEMPURA, AUGUSTER, TEMPTER).
      const char *targets[] = {"(TEMP)URA", "A(U)GUSTER", "(TEMP)TER",
                               "(TEMP)ERAS", "(TEMP)TERS"};
      for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
        for (int i = 0; i < n_ranked; i++) {
          const Move *m = move_list_get_move(opp_ml, ranked[i].move_idx);
          StringBuilder *sb = string_builder_create();
          string_builder_add_move(sb, game_get_board(game), m,
                                  game_get_ld(game), true);
          if (strstr(string_builder_peek(sb), targets[t]) != NULL) {
            (void)fprintf(
                stderr,
                "[opp_util] %-12s ranks #%-3d  win=%.4f  spread=%+7.3f"
                "  util=%+9.5f  %s\n",
                targets[t], i + 1, ranked[i].win_pct, ranked[i].mean_spread,
                ranked[i].utility, string_builder_peek(sb));
            string_builder_destroy(sb);
            break;
          }
          string_builder_destroy(sb);
        }
      }
      free(ranked);
      free(placement_opp_ranks);
      free(inner_jobs);
      free(inner_arg_ptrs);
      (void)fflush(stderr);
    }

    // One-shot dump of every opp tile-placement move for this scenario,
    // including the mover_total that results from greedy continuation
    // (so you can sort by "actual outcome" instead of equity). Triggered
    // by PASSPEG_GREEDY_DUMP_OPP=1.
    const char *dump_opp_env = getenv("PASSPEG_GREEDY_DUMP_OPP");
    if (dump_opp_env && passpeg_str_to_int(dump_opp_env) == 1) {
      (void)fprintf(stderr, "[dump_opp] scenario %s/%s  n_opp=%d  cand=%s\n",
                    drawn_str, remaining_str, n_opp, ctx->cand_txt);
      for (int dump_rank = 0; dump_rank < n_opp; dump_rank++) {
        const Move *dump_move = move_list_get_move(opp_ml, dump_rank);
        const int dump_type = move_get_type(dump_move);
        if (dump_type != GAME_EVENT_TILE_PLACEMENT_MOVE) {
          continue;
        }
        // Apply this opp move on a branch and greedy-play to game end.
        Game *dump_branch = game_duplicate(game);
        game_set_endgame_solving_mode(dump_branch);
        game_set_backup_mode(dump_branch, BACKUP_MODE_OFF);
        play_move(dump_move, dump_branch, NULL);
        game_set_game_end_reason(dump_branch, GAME_END_REASON_NONE);
        MoveList *dump_pl = move_list_create(4096);
        char dump_pv[1024] = {0};
        char dump_mr[32] = {0};
        char dump_or[32] = {0};
        const int32_t dump_mt = peg_greedy_playout_pv(
            dump_branch, ctx->mover_idx, dump_pl, ctx->worker_idx, dump_pv,
            sizeof(dump_pv), dump_mr, sizeof(dump_mr), dump_or, sizeof(dump_or),
            NULL, 0);
        move_list_destroy(dump_pl);
        game_destroy(dump_branch);
        StringBuilder *dump_sb = string_builder_create();
        string_builder_add_move(dump_sb, game_get_board(game), dump_move,
                                game_get_ld(game), true);
        (void)fprintf(stderr,
                      "  #%-5d  score=%-4d  greedy_mt=%+5d  %s  | pv: %s | end "
                      "mover=[%s] opp=[%s]\n",
                      dump_rank + 1, equity_to_int(move_get_score(dump_move)),
                      (int)dump_mt, string_builder_peek(dump_sb), dump_pv,
                      dump_mr, dump_or);
        string_builder_destroy(dump_sb);
      }
      (void)fflush(stderr);
    }

    // rational-opp halving path (opt-in). opp doesn't know the
    // realized bag tile: he evaluates each opp candidate move at the
    // current stage's depth across every perceived bag-tile type
    // (weighted by physical-tile counts), and picks the move that
    // maximizes utility = opp_win_pct + alpha * opp_mean_spread.
    // The scenario's mover_total is then the *realized* mt of opp's
    // pick — utility is used only to choose the move and then
    // discarded; aggregate win % is computed from realized outcomes.
    // Halving: starting from opp_top_k cands, stage s = 0..depth-1
    // cuts to opp_top_k >> s by utility-DESC, final stage at depth
    // picks the utility-max from the survivors.
    // Bag-emptier case (n_bag_remaining == 0): no perceived bag tile,
    // opp's "perceived" pool collapses to a single deterministic
    // scenario (mover's known rack), so utility-pick == MIN-over-opp.
    // Falls into the legacy MIN path below.
    const char *rational_env = getenv("PASSPEG_GREEDY_RATIONAL");
    const bool rational = rational_env &&
                          passpeg_str_to_int(rational_env) > 0 &&
                          ctx->n_bag_remaining >= 1;

    // PASSPEG_GREEDY_RAT_WALK=1 — rational walker for inner 2+peg
    // states (also handles 1peg as a degenerate case). At each PEG ply
    // we take opp_top_k candidates by movegen equity, rank them at
    // d=0 (opp's utility = avg over n_bag-multiset perception for opp
    // plies; realized greedy mt for mover plies), pick top-1, apply.
    // When the bag empties we run a greedy playout to game end.
    // The realized scenario's mover_total is the realized spread.
    //
    // PASSPEG_GREEDY_AUTO_WALKER=1 — per-cand auto-dispatch:
    //   bag-after >= 2 → walker
    //   bag-after == 1 → rational halving (no walker)
    //   bag-after == 0 → bag-emptier path (existing MIN/endgame)
    // When set, overrides RAT_WALK.
    const char *walk_env = getenv("PASSPEG_GREEDY_RAT_WALK");
    const char *auto_walk_env = getenv("PASSPEG_GREEDY_AUTO_WALKER");
    const bool auto_walker =
        auto_walk_env && passpeg_str_to_int(auto_walk_env) > 0;
    const bool walk_rat =
        rational &&
        (auto_walker ? ctx->n_bag_remaining >= 2
                     : (walk_env && passpeg_str_to_int(walk_env) > 0));

    if (walk_rat) {
      const char *walk_alpha_env = getenv("PASSPEG_GREEDY_ALPHA");
      const double walk_alpha = walk_alpha_env && *walk_alpha_env
                                    ? passpeg_str_to_double(walk_alpha_env)
                                    : 1e-4;
      const int per_ply_k_default = ctx->opp_top_k > 0 ? ctx->opp_top_k : 8;
      // PASSPEG_GREEDY_WALK_K="K1,K2,K3,K4" overrides the per-ply
      // candidate cap by current bag-size-at-ply. Position i = bag
      // size i+1. Unspecified entries fall back to per_ply_k_default.
      // Example: WALK_K="8,32,8" → at a ply with bag=1 use K=8, bag=2
      // K=32, bag=3 K=8. Lets you spend more search where it counts
      // (inner 2-peg) and less where it's cheap (inner 3-peg).
      int walk_k_by_bag[16];
      for (int i = 0; i < 16; i++) {
        walk_k_by_bag[i] = per_ply_k_default;
      }
      {
        const char *walk_k_env = getenv("PASSPEG_GREEDY_WALK_K");
        if (walk_k_env && *walk_k_env) {
          char tmp[256];
          (void)snprintf(tmp, sizeof(tmp), "%s", walk_k_env);
          int i = 0;
          const char *tok = strtok(tmp, ",");
          while (tok != NULL && i < 16) {
            walk_k_by_bag[i++] = passpeg_str_to_int(tok);
            tok = strtok(NULL, ",");
          }
        }
      }

      // opp_ml (generated at the top of the d>=1 branch on the original
      // `game`) is unused by the walker; release it here. The walker
      // builds fresh post-cand games per permutation below.
      move_list_destroy(opp_ml);
      game_destroy(game);

      // Enumerate distinct lexicographic orderings of bag_remaining.
      // Each ordering is a sub-scenario: opp draws the first tile in
      // order, mover draws the second, etc. We rebuild the post-cand
      // game per perm so that the bag's deterministic draw order
      // matches the perm.
      MachineLetter perm[16];
      for (int i = 0; i < ctx->n_bag_remaining; i++) {
        perm[i] = bag_remaining[i];
      }
      // Sort ascending.
      for (int i = 1; i < ctx->n_bag_remaining; i++) {
        for (int j = i; j > 0 && perm[j] < perm[j - 1]; j--) {
          MachineLetter tmp = perm[j];
          perm[j] = perm[j - 1];
          perm[j - 1] = tmp;
        }
      }

      do {
        char perm_remaining_str[32] = {0};
        const int n_perm = (int)(sizeof(perm) / sizeof(perm[0]));
        for (int i = 0; i < ctx->n_bag_remaining && i < n_perm; i++) {
          perm_remaining_str[i] = ctx->ld->ld_ml_to_hl[perm[i]][0];
        }

        Game *walker = peg_make_post_cand_game(
            ctx->base_game, ctx->mover_idx, ctx->unseen, ctx->ld_size,
            ctx->cand, ctx->k_drawn, mover_drawn, ctx->n_bag_remaining, perm);
        StringBuilder *walker_pv = ctx->tsv_f ? string_builder_create() : NULL;

        // Per-cand opp-ply counter: lets the user set a different K for the
        // very first opp decision (deeper search at the start of the peg)
        // vs subsequent opp decisions (where the bag is smaller and the
        // game is more constrained).
        const char *first_opp_k_env = getenv("PASSPEG_GREEDY_WALK_K_FIRST_OPP");
        const char *later_opp_k_env = getenv("PASSPEG_GREEDY_WALK_K_LATER_OPP");
        const int first_opp_k = first_opp_k_env && *first_opp_k_env
                                    ? passpeg_str_to_int(first_opp_k_env)
                                    : 0;
        const int later_opp_k = later_opp_k_env && *later_opp_k_env
                                    ? passpeg_str_to_int(later_opp_k_env)
                                    : 0;
        int opp_ply_count = 0;
        // Total rational nonempty decisions made by the walker. We cap the
        // walker at ctx->depth plies so non-emptier and emptier cands both
        // get exactly `depth` plies of informed lookahead at stage `depth`.
        // Whatever remains after the walker (if any) is filled in by a
        // terminal endgame_solve when the bag has emptied.
        int walker_plies = 0;

        while (bag_get_letters(game_get_bag(walker)) > 0) {
          // Cascade-budget poll. The walker loop fans out into deep
          // per-ply work (opp-side perception enumerations, mover-side
          // move generation + playouts). Without this poll a single
          // scenario can run the full walker tree even after the outer
          // wall budget is exceeded.
          if (ctx->deadline_monotonic_ns > 0 &&
              ctimer_monotonic_ns() > ctx->deadline_monotonic_ns) {
            break;
          }
          const int turn = game_get_player_on_turn_index(walker);
          const bool is_opp = (turn != ctx->mover_idx);
          const int bag_at_ply = bag_get_letters(game_get_bag(walker));
          int per_ply_k = (bag_at_ply >= 1 && bag_at_ply <= 16)
                              ? walk_k_by_bag[bag_at_ply - 1]
                              : per_ply_k_default;
          if (is_opp) {
            if (opp_ply_count == 0 && first_opp_k > 0) {
              per_ply_k = first_opp_k;
            } else if (opp_ply_count > 0 && later_opp_k > 0) {
              per_ply_k = later_opp_k;
            }
          }

          MoveList *wml = move_list_create(16384);
          const MoveGenArgs wga = {
              .game = walker,
              .move_list = wml,
              .move_record_type = MOVE_RECORD_ALL,
              .move_sort_type = MOVE_SORT_EQUITY,
              .override_kwg = NULL,
              .eq_margin_movegen = 0,
              .target_equity = EQUITY_MAX_VALUE,
              .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
          };
          generate_moves(&wga);
          const int n_full = move_list_get_count(wml);
          // MOVE_RECORD_ALL is heap-ordered; iterating positions 0..N grabs
          // moves by heap layout, not by equity. Sort the placement-only
          // candidates by equity descending and then take the first per_ply_k.
          int *placement_idx =
              malloc_or_die((size_t)(n_full > 0 ? n_full : 1) * sizeof(int));
          int n_placement = 0;
          for (int i = 0; i < n_full; i++) {
            const Move *m = move_list_get_move(wml, i);
            if (move_get_type(m) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
              placement_idx[n_placement++] = i;
            }
          }
          const int top_take =
              per_ply_k < n_placement ? per_ply_k : n_placement;
          for (int ti = 0; ti < top_take; ti++) {
            int best = ti;
            Equity best_eq =
                move_get_equity(move_list_get_move(wml, placement_idx[ti]));
            for (int j = ti + 1; j < n_placement; j++) {
              const Equity je =
                  move_get_equity(move_list_get_move(wml, placement_idx[j]));
              if (je > best_eq) {
                best = j;
                best_eq = je;
              }
            }
            if (best != ti) {
              const int tmp = placement_idx[ti];
              placement_idx[ti] = placement_idx[best];
              placement_idx[best] = tmp;
            }
          }
          // PASSPEG_OPP_RANK_BY_PLAYOUT=1: at each opp ply, re-rank the
          // top-PASSPEG_OPP_RANK_POOL (default 32) static-equity candidates
          // by realized greedy-playout outcome (lowest mover_total = worst
          // for mover = best opp choice), then keep the top per_ply_k from
          // that re-rank. This is more expensive but should pick up moves
          // that are defensively strong without scoring high (which static
          // equity systematically under-ranks).
          if (is_opp) {
            const char *opp_rank_env = getenv("PASSPEG_OPP_RANK_BY_PLAYOUT");
            const bool rank_by_playout =
                opp_rank_env && passpeg_str_to_int(opp_rank_env) > 0;
            if (rank_by_playout) {
              const char *pool_env = getenv("PASSPEG_OPP_RANK_POOL");
              const int pool_size_req =
                  pool_env && *pool_env ? passpeg_str_to_int(pool_env) : 32;
              const int pool_size =
                  pool_size_req < n_placement ? pool_size_req : n_placement;
              // First take the top pool_size by static equity (already sorted
              // for top top_take = per_ply_k; need to extend the sort to
              // pool_size).
              for (int ti = top_take; ti < pool_size; ti++) {
                int best = ti;
                Equity best_eq =
                    move_get_equity(move_list_get_move(wml, placement_idx[ti]));
                for (int j = ti + 1; j < n_placement; j++) {
                  const Equity je = move_get_equity(
                      move_list_get_move(wml, placement_idx[j]));
                  if (je > best_eq) {
                    best = j;
                    best_eq = je;
                  }
                }
                if (best != ti) {
                  const int tmp = placement_idx[ti];
                  placement_idx[ti] = placement_idx[best];
                  placement_idx[best] = tmp;
                }
              }
              // For each candidate in the pool, run a realized greedy
              // playout from post-opp state and record mover_total.
              int32_t *po_mt =
                  malloc_or_die((size_t)pool_size * sizeof(int32_t));
              for (int i = 0; i < pool_size; i++) {
                const Move *m = move_list_get_move(wml, placement_idx[i]);
                Game *probe = game_duplicate(walker);
                game_set_endgame_solving_mode(probe);
                game_set_backup_mode(probe, BACKUP_MODE_OFF);
                play_move(m, probe, NULL);
                game_set_game_end_reason(probe, GAME_END_REASON_NONE);
                MoveList *pml = move_list_create(1);
                po_mt[i] = peg_greedy_playout_pv(probe, ctx->mover_idx, pml,
                                                 ctx->worker_idx, NULL, 0, NULL,
                                                 0, NULL, 0, NULL, 0);
                move_list_destroy(pml);
                game_destroy(probe);
              }
              // Partial-selection-sort: keep the per_ply_k entries with the
              // LOWEST po_mt (best for opp).
              for (int ti = 0; ti < top_take; ti++) {
                int best = ti;
                int32_t best_mt = po_mt[ti];
                for (int j = ti + 1; j < pool_size; j++) {
                  if (po_mt[j] < best_mt) {
                    best = j;
                    best_mt = po_mt[j];
                  }
                }
                if (best != ti) {
                  const int tmp_idx = placement_idx[ti];
                  placement_idx[ti] = placement_idx[best];
                  placement_idx[best] = tmp_idx;
                  const int32_t tmp_mt = po_mt[ti];
                  po_mt[ti] = po_mt[best];
                  po_mt[best] = tmp_mt;
                }
              }
              free(po_mt);
            }
          }

          int sel_idx[256];
          int n_sel = 0;
          for (int i = 0; i < top_take && n_sel < 256; i++) {
            sel_idx[n_sel++] = placement_idx[i];
          }
          free(placement_idx);
          if (n_sel == 0) {
            move_list_destroy(wml);
            break;
          }

          int best_local = 0;
          if (is_opp) {
            // Opp ply: perception eval with n_bag-multiset enumeration.
            const int opp_idx = turn;
            uint8_t walk_unseen[MAX_ALPHABET_SIZE];
            peg_compute_unseen(walker, opp_idx, walk_unseen);
            MachineLetter walk_types[MAX_ALPHABET_SIZE];
            int walk_type_counts[MAX_ALPHABET_SIZE];
            int n_walk_types = 0;
            for (int ml_idx = 0; ml_idx < ctx->ld_size; ml_idx++) {
              if (walk_unseen[ml_idx] > 0) {
                walk_types[n_walk_types] = (MachineLetter)ml_idx;
                walk_type_counts[n_walk_types] = (int)walk_unseen[ml_idx];
                n_walk_types++;
              }
            }
            // Realized bag composition (indexed parallel to walk_types).
            int realized_bag_counts[MAX_ALPHABET_SIZE] = {0};
            const Bag *wbag = game_get_bag(walker);
            const int n_bag_now = bag_get_letters(wbag);
            for (int t = 0; t < n_walk_types; t++) {
              realized_bag_counts[t] =
                  bag_get_letter((Bag *)wbag, walk_types[t]);
            }
            double best_util = -1e18;
            for (int pick_idx = 0; pick_idx < n_sel; pick_idx++) {
              // Per-iteration cascade-budget poll. Each
              // peg_eval_opp_with_perception call is a deep recursive
              // enumeration over perception opp-POV states that can run
              // hundreds of game_duplicate + greedy_playout calls; without this
              // poll a single scenario can exceed the outer wall budget by an
              // order of magnitude.
              if (ctx->deadline_monotonic_ns > 0 &&
                  ctimer_monotonic_ns() > ctx->deadline_monotonic_ns) {
                break;
              }
              const Move *m = move_list_get_move(wml, sel_idx[pick_idx]);
              double util = 0.0;
              int32_t realized = 0;
              // Format opp move text for inner-TSV annotation (cheap when
              // tsv disabled — string_builder is light, and the actual write
              // only happens if inner_tsv_f is set).
              char opp_move_text[64] = {0};
              int opp_move_score = 0;
              if (ctx->inner_tsv_f) {
                StringBuilder *sb = string_builder_create();
                string_builder_add_move(sb, game_get_board(walker), m,
                                        game_get_ld(walker), true);
                (void)snprintf(opp_move_text, sizeof(opp_move_text), "%s",
                               string_builder_peek(sb));
                string_builder_destroy(sb);
                opp_move_score = equity_to_int(move_get_score(m));
              }
              peg_eval_opp_with_perception(
                  ctx, walker, m, walk_types, walk_type_counts, n_walk_types,
                  n_bag_now, realized_bag_counts, walk_alpha, &util, &realized,
                  ctx->inner_tsv_f ? drawn_str : NULL,
                  ctx->inner_tsv_f ? perm_remaining_str : NULL,
                  ctx->inner_tsv_f ? opp_move_text : NULL, opp_move_score);
              if (util > best_util) {
                best_util = util;
                best_local = pick_idx;
              }
            }
          } else {
            // Mover ply: realized greedy mt per candidate.
            int32_t best_mt = INT32_MIN;
            for (int pick_idx = 0; pick_idx < n_sel; pick_idx++) {
              if (ctx->deadline_monotonic_ns > 0 &&
                  ctimer_monotonic_ns() > ctx->deadline_monotonic_ns) {
                break;
              }
              const Move *m = move_list_get_move(wml, sel_idx[pick_idx]);
              Game *probe = game_duplicate(walker);
              game_set_endgame_solving_mode(probe);
              game_set_backup_mode(probe, BACKUP_MODE_OFF);
              play_move(m, probe, NULL);
              game_set_game_end_reason(probe, GAME_END_REASON_NONE);
              MoveList *pml = move_list_create(1);
              const int32_t mt = peg_greedy_playout_pv(
                  probe, ctx->mover_idx, pml, ctx->worker_idx, NULL, 0, NULL, 0,
                  NULL, 0, NULL, 0);
              move_list_destroy(pml);
              game_destroy(probe);
              if (mt > best_mt) {
                best_mt = mt;
                best_local = pick_idx;
              }
            }
          }

          const Move *picked = move_list_get_move(wml, sel_idx[best_local]);
          if (walker_pv) {
            if (string_builder_length(walker_pv) > 0) {
              string_builder_add_string(walker_pv, " | ");
            }
            string_builder_add_move(walker_pv, game_get_board(walker), picked,
                                    game_get_ld(walker), true);
          }
          play_move(picked, walker, NULL);
          game_set_game_end_reason(walker, GAME_END_REASON_NONE);
          if (is_opp) {
            opp_ply_count++;
          }
          walker_plies++;
          move_list_destroy(wml);
          // Cap the walker at ctx->depth plies. Anything beyond is filled
          // in by greedy / endgame_solve below.
          if (walker_plies >= ctx->depth) {
            break;
          }
        }

        // Finish the scenario. Every cand at stage `depth` gets exactly
        // `depth` lookahead plies. The walker has already done some of
        // those (`walker_plies` — its rational-opp pre-bag-empty
        // decisions). The remaining (`depth - walker_plies`) plies are
        // filled in by endgame_solve when the bag is empty. If the bag
        // still has tiles (walker capped early due to walker_plies >=
        // depth), no remaining plies — fall back to greedy.
        const int32_t mover_lead =
            equity_to_int(
                player_get_score(game_get_player(walker, ctx->mover_idx))) -
            equity_to_int(
                player_get_score(game_get_player(walker, 1 - ctx->mover_idx)));
        int32_t walker_mt = 0;
        char final_pv[1024] = {0};
        char final_mr[32] = {0};
        char final_or[32] = {0};
        int remaining_plies = ctx->depth - walker_plies;
        const bool bag_empty_now = bag_get_letters(game_get_bag(walker)) == 0;
        // PASSPEG_INNER_USE_ENDGAME=N (plies): when the walker exits with the
        // bag already empty but no remaining plies in its depth budget, the
        // fallback is greedy — that misvalues endgames the same way the leaf
        // does. Promote remaining_plies to at least N so the terminal eval
        // uses endgame_solve (already endgame-aware downstream).
        {
          const char *inner_eg_env = getenv("PASSPEG_INNER_USE_ENDGAME");
          const int inner_eg_plies = inner_eg_env && *inner_eg_env
                                         ? passpeg_str_to_int(inner_eg_env)
                                         : 0;
          if (inner_eg_plies > 0 && bag_empty_now &&
              remaining_plies < inner_eg_plies) {
            remaining_plies = inner_eg_plies;
          }
        }
        if (remaining_plies > 0 && bag_empty_now &&
            game_get_game_end_reason(walker) == GAME_END_REASON_NONE) {
          EndgameCtx *eg_ctx = NULL;
          EndgameResults *eg_results = endgame_results_create();
          EndgameArgs ea = {
              .thread_control = ctx->thread_control,
              .game = walker,
              .plies = remaining_plies,
              .shared_tt = NULL,
              .initial_small_move_arena_size =
                  DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
              .num_threads = 1,
              .use_heuristics = true,
              .num_top_moves = 1,
              .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
              .skip_word_pruning = true,
              .soft_time_limit = peg_remaining_budget_secs(ctx),
              .hard_time_limit = peg_remaining_budget_secs(ctx),
              .external_deadline_ns = ctx->deadline_monotonic_ns,
          };
          endgame_solve_inline(&eg_ctx, &ea, eg_results);
          // Cascade deadline may have fired mid-search — guard against using
          // an uninitialized result (see equity.h equity_to_int assertion).
          const int walker_eg_depth =
              endgame_results_get_depth(eg_results, ENDGAME_RESULT_BEST);
          if (walker_eg_depth < 0) {
            MoveList *pl_fb = move_list_create(1);
            walker_mt = peg_greedy_playout_pv(
                walker, ctx->mover_idx, pl_fb, ctx->worker_idx,
                walker_pv ? final_pv : NULL, sizeof(final_pv),
                walker_pv ? final_mr : NULL, sizeof(final_mr),
                walker_pv ? final_or : NULL, sizeof(final_or), NULL, 0);
            move_list_destroy(pl_fb);
          } else {
            const int turn = game_get_player_on_turn_index(walker);
            const int32_t eg_val =
                endgame_results_get_value(eg_results, ENDGAME_RESULT_BEST);
            walker_mt = (turn == ctx->mover_idx) ? mover_lead + eg_val
                                                 : mover_lead - eg_val;
          }
          endgame_ctx_destroy(eg_ctx);
          endgame_results_destroy(eg_results);
        } else {
          MoveList *final_ml = move_list_create(1);
          walker_mt = peg_greedy_playout_pv(
              walker, ctx->mover_idx, final_ml, ctx->worker_idx,
              walker_pv ? final_pv : NULL, sizeof(final_pv),
              walker_pv ? final_mr : NULL, sizeof(final_mr),
              walker_pv ? final_or : NULL, sizeof(final_or), NULL, 0);
          move_list_destroy(final_ml);
        }

        if (walker_pv) {
          if (final_pv[0] != '\0') {
            if (string_builder_length(walker_pv) > 0) {
              string_builder_add_string(walker_pv, " | ");
            }
            string_builder_add_string(walker_pv, final_pv);
          }
          (void)snprintf(pv_text, sizeof(pv_text), "%s",
                         string_builder_peek(walker_pv));
          (void)snprintf(mover_rack_end, sizeof(mover_rack_end), "%s",
                         final_mr);
          (void)snprintf(opp_rack_end, sizeof(opp_rack_end), "%s", final_or);
          string_builder_destroy(walker_pv);
        }
        game_destroy(walker);

        // Per-perm aggregation. Each ordering counts as a distinct
        // sub-scenario with weight = multiset weight (so a multiset with
        // n_orderings letter orderings contributes n_orderings entries
        // to weight_sum; this captures the physical bag-draw orderings
        // we'd otherwise miss).
        peg_lock(ctx->res_mutex);
        ctx->res->weight_sum += weight;
        ctx->res->spread_sum += weight * (int64_t)walker_mt;
        if (walker_mt > 0) {
          ctx->res->win_x2 += 2 * weight;
        } else if (walker_mt == 0) {
          ctx->res->win_x2 += weight;
        }
        ctx->res->n_scen++;
        peg_unlock(ctx->res_mutex);

        if (ctx->tsv_f) {
          peg_lock(ctx->tsv_mutex);
          (void)fprintf(
              ctx->tsv_f,
              "%d\t%s\t%d\t%d\t%d\t%s\t%s\t%lld\t%d\t%s\t%s\t%s\t%s\t%s\n",
              ctx->pos_idx, ctx->cand_txt, ctx->cand_score,
              ctx->k_drawn + ctx->n_bag_remaining, ctx->k_drawn, drawn_str,
              perm_remaining_str, (long long)weight, (int)walker_mt,
              post_cand_cgp, "", pv_text, mover_rack_end, opp_rack_end);
          peg_unlock(ctx->tsv_mutex);
        }
      } while (peg_next_perm(perm, ctx->n_bag_remaining));

      return; // walker handled all aggregation per-perm; skip the
              // single-mt fold at the bottom of emit_split.
    }
    if (rational) {
      // 1. Pre-filter opp moves to placements + opp_move_filter.
      int *opp_cand_idx =
          malloc_or_die((size_t)(n_opp > 0 ? n_opp : 1) * sizeof(int));
      int n_opp_cand = 0;
      for (int opp_rank = 0; opp_rank < n_opp; opp_rank++) {
        const Move *opp_move_chk = move_list_get_move(opp_ml, opp_rank);
        if (move_get_type(opp_move_chk) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
          continue;
        }
        if (ctx->opp_move_filter) {
          char text[64] = {0};
          StringBuilder *sb_chk = string_builder_create();
          string_builder_add_move(sb_chk, game_get_board(game), opp_move_chk,
                                  game_get_ld(game), true);
          (void)snprintf(text, sizeof(text), "%s", string_builder_peek(sb_chk));
          string_builder_destroy(sb_chk);
          bool match = false;
          char tmp[2048];
          (void)snprintf(tmp, sizeof(tmp), "%s", ctx->opp_move_filter);
          const char *tok = strtok(tmp, ";");
          while (tok != NULL) {
            if (strstr(text, tok) != NULL) {
              match = true;
              break;
            }
            tok = strtok(NULL, ";");
          }
          if (!match) {
            continue;
          }
        }
        opp_cand_idx[n_opp_cand++] = opp_rank;
      }

      // 2. opp's perceived bag-tile pool.
      const int opp_idx = 1 - ctx->mover_idx;
      uint8_t opp_unseen[MAX_ALPHABET_SIZE];
      peg_compute_unseen(game, opp_idx, opp_unseen);
      MachineLetter opp_types[MAX_ALPHABET_SIZE];
      int opp_type_counts[MAX_ALPHABET_SIZE];
      int n_opp_types = 0;
      for (int ml = 0; ml < ctx->ld_size; ml++) {
        if (opp_unseen[ml] > 0) {
          opp_types[n_opp_types] = (MachineLetter)ml;
          opp_type_counts[n_opp_types] = (int)opp_unseen[ml];
          n_opp_types++;
        }
      }
      int realized_ti = -1;
      {
        const MachineLetter realized_tile = bag_remaining[0];
        for (int t = 0; t < n_opp_types; t++) {
          if (opp_types[t] == realized_tile) {
            realized_ti = t;
            break;
          }
        }
      }

      const char *alpha_env = getenv("PASSPEG_GREEDY_ALPHA");
      const double alpha =
          alpha_env && *alpha_env ? passpeg_str_to_double(alpha_env) : 1e-4;

      // 3. Halving: stages 0..depth-1 cut, final stage at depth picks.
      int n_to_eval = n_opp_cand;
      double *final_utility = NULL;
      int32_t *final_realized = NULL;
      for (int stage = 0; stage <= ctx->depth && n_to_eval > 0; stage++) {
        const int target_k =
            stage < ctx->depth ? (ctx->opp_top_k >> stage) : n_to_eval;
        if (stage < ctx->depth && (target_k <= 0 || n_to_eval <= target_k)) {
          continue;
        }
        // Build per-(opp, perceived_tile) jobs and dispatch.
        const int n_jobs = n_to_eval * n_opp_types;
        PegOppInnerJob *jobs =
            malloc_or_die((size_t)n_jobs * sizeof(PegOppInnerJob));
        void **args = malloc_or_die((size_t)n_jobs * sizeof(void *));
        for (int i = 0; i < n_to_eval; i++) {
          const Move *opp_move = move_list_get_move(opp_ml, opp_cand_idx[i]);
          for (int ti = 0; ti < n_opp_types; ti++) {
            int idx = i * n_opp_types + ti;
            jobs[idx] = (PegOppInnerJob){
                .base_game = game,
                .mover_idx = ctx->mover_idx,
                .ld_size = ctx->ld_size,
                .opp_move = opp_move,
                .bag_ml = opp_types[ti],
                .weight = opp_type_counts[ti],
                .n_opp_types = n_opp_types,
                .opp_types = opp_types,
                .opp_type_counts = opp_type_counts,
                .ti = ti,
                .opp_depth = stage,
                .thread_control = ctx->thread_control,
                .deadline_monotonic_ns = ctx->deadline_monotonic_ns,
                .mover_total = 0,
            };
            args[idx] = &jobs[idx];
          }
        }
        if (ctx->executor && n_jobs > 0) {
          peg_pool_submit_and_wait(ctx->executor, peg_opp_inner_worker_fn, args,
                                   n_jobs, ctx->worker_idx);
        } else {
          for (int j = 0; j < n_jobs; j++) {
            peg_opp_inner_worker_fn(&jobs[j], ctx->worker_idx);
          }
        }
        // Aggregate per-opp utility + realized mt.
        double *stage_util = malloc_or_die((size_t)n_to_eval * sizeof(double));
        int32_t *stage_realized =
            malloc_or_die((size_t)n_to_eval * sizeof(int32_t));
        for (int i = 0; i < n_to_eval; i++) {
          int64_t weight_sum = 0;
          int64_t spread_sum_opp = 0;
          double win_x2_sum = 0.0;
          int32_t realized_mt = 0;
          for (int ti = 0; ti < n_opp_types; ti++) {
            int idx = i * n_opp_types + ti;
            int32_t mt = jobs[idx].mover_total;
            int w = jobs[idx].weight;
            int32_t mt_opp = -mt;
            weight_sum += w;
            spread_sum_opp += (int64_t)mt_opp * w;
            if (mt_opp > 0) {
              win_x2_sum += 2.0 * w;
            } else if (mt_opp == 0) {
              win_x2_sum += w;
            }
            if (ti == realized_ti) {
              realized_mt = mt;
            }
          }
          double opp_winpct = win_x2_sum / (2.0 * (double)weight_sum);
          double opp_mean_spread = (double)spread_sum_opp / (double)weight_sum;
          stage_util[i] = opp_winpct + alpha * opp_mean_spread;
          stage_realized[i] = realized_mt;
        }
        free(jobs);
        free(args);
        if (stage < ctx->depth) {
          // Partial selection sort: top target_k by utility DESC.
          for (int i = 0; i < target_k; i++) {
            int max_i = i;
            for (int j = i + 1; j < n_to_eval; j++) {
              if (stage_util[j] > stage_util[max_i]) {
                max_i = j;
              }
            }
            if (max_i != i) {
              double t_u = stage_util[i];
              stage_util[i] = stage_util[max_i];
              stage_util[max_i] = t_u;
              int32_t t_r = stage_realized[i];
              stage_realized[i] = stage_realized[max_i];
              stage_realized[max_i] = t_r;
              int t_idx = opp_cand_idx[i];
              opp_cand_idx[i] = opp_cand_idx[max_i];
              opp_cand_idx[max_i] = t_idx;
            }
          }
          n_to_eval = target_k;
          free(stage_util);
          free(stage_realized);
        } else {
          free(final_utility);
          free(final_realized);
          final_utility = stage_util;
          final_realized = stage_realized;
        }
      }

      // 4. opp picks utility-max from the final stage.
      int32_t mover_total_rational = 0;
      if (final_utility != NULL && n_to_eval > 0) {
        int picked = 0;
        for (int i = 1; i < n_to_eval; i++) {
          if (final_utility[i] > final_utility[picked]) {
            picked = i;
          }
        }
        mover_total_rational = final_realized[picked];
        // For TSV: capture opp's pick text + endgame PV under the
        // realized bag tile. One extra endgame_solve per scenario.
        if (ctx->tsv_f) {
          const Move *picked_move =
              move_list_get_move(opp_ml, opp_cand_idx[picked]);
          char picked_text[64] = {0};
          {
            StringBuilder *sb_p = string_builder_create();
            string_builder_add_move(sb_p, game_get_board(game), picked_move,
                                    game_get_ld(game), true);
            (void)snprintf(picked_text, sizeof(picked_text), "%s",
                           string_builder_peek(sb_p));
            string_builder_destroy(sb_p);
          }
          // Build the realized opp_pov_game game (same as
          // peg_opp_inner_worker_fn does, but for the realized bag tile only)
          // and run endgame_solve with PV capture.
          Game *opp_pov_game = game_duplicate(game);
          game_set_endgame_solving_mode(opp_pov_game);
          game_set_backup_mode(opp_pov_game, BACKUP_MODE_OFF);
          {
            Bag *hb = game_get_bag(opp_pov_game);
            for (int ml = 0; ml < ctx->ld_size; ml++) {
              while (bag_get_letter(hb, (MachineLetter)ml) > 0) {
                (void)bag_draw_letter(hb, (MachineLetter)ml, 0);
              }
            }
            // Bag should already be empty; nothing else to seed since
            // the realized state mirrors the post-cand game.
          }
          play_move(picked_move, opp_pov_game, NULL);
          game_set_game_end_reason(opp_pov_game, GAME_END_REASON_NONE);
          const LetterDistribution *bld = game_get_ld(opp_pov_game);
          EndgameCtx *eg_ctx = NULL;
          EndgameResults *eg_results = endgame_results_create();
          EndgameArgs ea = {
              .thread_control = ctx->thread_control,
              .game = opp_pov_game,
              .plies = ctx->depth,
              .shared_tt = NULL,
              .initial_small_move_arena_size =
                  DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
              .num_threads = 1,
              .use_heuristics = true,
              .num_top_moves = 1,
              .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
              .skip_word_pruning = true,
              .soft_time_limit = 5.0,
              .hard_time_limit = 5.0,
              // soft/hard_time_limit only caps the next IDS depth start —
              // a single depth can run past it. external_deadline_ns is
              // the strict mid-search bail. Clamp by cascade deadline so
              // this realized-pick endgame can never outlive the wall budget.
              .external_deadline_ns = peg_clamp_deadline_ns(
                  ctimer_monotonic_ns() + (int64_t)(5.0 * 1.0e9),
                  ctx->deadline_monotonic_ns),
          };
          endgame_solve_inline(&eg_ctx, &ea, eg_results);
          // Build the PV text: opp's pick first, then endgame plies.
          StringBuilder *pv_sb = string_builder_create();
          string_builder_add_string(pv_sb, picked_text);
          const PVLine *pv =
              endgame_results_get_pvline(eg_results, ENDGAME_RESULT_BEST);
          if (pv && pv->num_moves > 0) {
            Game *pv_game = game_duplicate(opp_pov_game);
            game_set_endgame_solving_mode(pv_game);
            game_set_backup_mode(pv_game, BACKUP_MODE_OFF);
            for (int mi = 0; mi < pv->num_moves; mi++) {
              Move m_full;
              small_move_to_move(&m_full, &pv->moves[mi],
                                 game_get_board(pv_game));
              string_builder_add_string(pv_sb, " | ");
              string_builder_add_move(pv_sb, game_get_board(pv_game), &m_full,
                                      bld, true);
              play_move(&m_full, pv_game, NULL);
            }
            char *cgp = game_get_cgp(pv_game, true);
            (void)snprintf(final_cgp, sizeof(final_cgp), "%s", cgp ? cgp : "");
            free(cgp);
            StringBuilder *rsb = string_builder_create();
            string_builder_add_rack(
                rsb, player_get_rack(game_get_player(pv_game, ctx->mover_idx)),
                bld, false);
            (void)snprintf(mover_rack_end, sizeof(mover_rack_end), "%s",
                           string_builder_peek(rsb));
            string_builder_destroy(rsb);
            StringBuilder *rsb2 = string_builder_create();
            string_builder_add_rack(
                rsb2,
                player_get_rack(game_get_player(pv_game, 1 - ctx->mover_idx)),
                bld, false);
            (void)snprintf(opp_rack_end, sizeof(opp_rack_end), "%s",
                           string_builder_peek(rsb2));
            string_builder_destroy(rsb2);
            game_destroy(pv_game);
          }
          (void)snprintf(pv_text, sizeof(pv_text), "%s",
                         string_builder_peek(pv_sb));
          string_builder_destroy(pv_sb);
          endgame_ctx_destroy(eg_ctx);
          endgame_results_destroy(eg_results);
          game_destroy(opp_pov_game);
        }
      }
      free(final_utility);
      free(final_realized);
      free(opp_cand_idx);
      move_list_destroy(opp_ml);
      mover_total = mover_total_rational;
    } else {
      int32_t worst_for_mover = 0;
      bool have_any = false;
      char worst_opp_text[64] = {0};
      StringBuilder *pv_builder = ctx->tsv_f ? string_builder_create() : NULL;

      // Pre-filter to placement moves (and optionally to the opp_move_filter
      // substring set). When PASSPEG_GREEDY_OPP_RERANK=1, also sort by
      // d=0 greedy mover_total ascending (worst-for-mover first) and take
      // the top opp_top_k — much more honest than equity's natural order
      // for the d=1 MIN-over-branches semantics (a low-equity but lethal
      // reply like TEMPURA can sit at rank #700 by equity but be #1 by
      // greedy mover_total). Default is the legacy equity order.
      int *opp_cand_idx =
          malloc_or_die((size_t)(n_opp > 0 ? n_opp : 1) * sizeof(int));
      int n_opp_cand = 0;
      for (int opp_rank = 0; opp_rank < n_opp; opp_rank++) {
        const Move *opp_move_chk = move_list_get_move(opp_ml, opp_rank);
        if (move_get_type(opp_move_chk) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
          continue;
        }
        if (ctx->opp_move_filter) {
          char text[64] = {0};
          StringBuilder *sb_chk = string_builder_create();
          string_builder_add_move(sb_chk, game_get_board(game), opp_move_chk,
                                  game_get_ld(game), true);
          (void)snprintf(text, sizeof(text), "%s", string_builder_peek(sb_chk));
          string_builder_destroy(sb_chk);
          bool match = false;
          char tmp[2048];
          (void)snprintf(tmp, sizeof(tmp), "%s", ctx->opp_move_filter);
          const char *tok = strtok(tmp, ";");
          while (tok != NULL) {
            if (strstr(text, tok) != NULL) {
              match = true;
              break;
            }
            tok = strtok(NULL, ";");
          }
          if (!match) {
            continue;
          }
        }
        opp_cand_idx[n_opp_cand++] = opp_rank;
      }

      // MOVE_RECORD_ALL returns a heap-ordered list, not a sorted one — so
      // opp_cand_idx[0..opp_top_k-1] in input order picks 8 moves by heap
      // position, not by equity. Partial-selection-sort the prefix we'll
      // actually evaluate so the "top opp_top_k" really is the top opp_top_k
      // by movegen equity. Without this, killer outplays (TOREROS, NOTARISE)
      // sit deep in the heap and never reach the MIN loop.
      {
        const int prefix =
            ctx->opp_top_k < n_opp_cand ? ctx->opp_top_k : n_opp_cand;
        for (int i = 0; i < prefix; i++) {
          int best = i;
          Equity best_eq =
              move_get_equity(move_list_get_move(opp_ml, opp_cand_idx[i]));
          for (int j = i + 1; j < n_opp_cand; j++) {
            const Equity je =
                move_get_equity(move_list_get_move(opp_ml, opp_cand_idx[j]));
            if (je > best_eq) {
              best = j;
              best_eq = je;
            }
          }
          if (best != i) {
            const int tmp = opp_cand_idx[i];
            opp_cand_idx[i] = opp_cand_idx[best];
            opp_cand_idx[best] = tmp;
          }
        }
      }

      // Optional Sequential-Halving ladder. PASSPEG_GREEDY_OPP_HALVE=1
      // implies cuts of K, K/2, K/4, ..., K/2^(D-1) at depths 0, 1, ...,
      // D-1, where K = opp_top_k and D = ctx->depth. The final MIN runs
      // over the K/2^(D-1) survivors at depth D. For (K=8, D=2):
      //   d=0 on all → top 8, d=1 on 8 → top 4, d=2 on 4 → MIN.
      // When set, supersedes PASSPEG_GREEDY_OPP_RERANK.
      const char *halve_env = getenv("PASSPEG_GREEDY_OPP_HALVE");
      const bool halve = halve_env && passpeg_str_to_int(halve_env) > 0;
      if (halve && n_opp_cand > 1 && ctx->depth >= 1) {
        int cuts[16];
        int n_stages = 0;
        // Stage s cuts to opp_top_k >> s for s = 0..depth-1.
        // (At s=0 we filter from the full opp pool down to opp_top_k.)
        for (int stage_idx = 0; stage_idx < ctx->depth && n_stages < 16;
             stage_idx++) {
          int k = ctx->opp_top_k >> stage_idx;
          if (k < 1) {
            k = 1;
          }
          cuts[n_stages++] = k;
        }
        for (int stage_idx = 0; stage_idx < n_stages; stage_idx++) {
          if (n_opp_cand <= cuts[stage_idx] || cuts[stage_idx] <= 0) {
            continue;
          }
          const int stage_depth = stage_idx; // d=0, d=1, d=2, ...
          int32_t *mt_arr = malloc_or_die((size_t)n_opp_cand * sizeof(int32_t));
          for (int i = 0; i < n_opp_cand; i++) {
            const Move *opp_move_p =
                move_list_get_move(opp_ml, opp_cand_idx[i]);
            Game *prior_branch = game_duplicate(game);
            game_set_endgame_solving_mode(prior_branch);
            game_set_backup_mode(prior_branch, BACKUP_MODE_OFF);
            play_move(opp_move_p, prior_branch, NULL);
            game_set_game_end_reason(prior_branch, GAME_END_REASON_NONE);
            if (stage_depth == 0) {
              MoveList *pml = move_list_create(1);
              mt_arr[i] = peg_greedy_playout_pv(prior_branch, ctx->mover_idx,
                                                pml, ctx->worker_idx, NULL, 0,
                                                NULL, 0, NULL, 0, NULL, 0);
              move_list_destroy(pml);
            } else {
              const int32_t mover_lead =
                  equity_to_int(player_get_score(
                      game_get_player(prior_branch, ctx->mover_idx))) -
                  equity_to_int(player_get_score(
                      game_get_player(prior_branch, 1 - ctx->mover_idx)));
              if (game_get_game_end_reason(prior_branch) !=
                  GAME_END_REASON_NONE) {
                mt_arr[i] = mover_lead;
              } else {
                EndgameCtx *eg = NULL;
                EndgameResults *er = endgame_results_create();
                EndgameArgs ea = {
                    .thread_control = ctx->thread_control,
                    .game = prior_branch,
                    .plies = stage_depth,
                    .shared_tt = NULL,
                    .initial_small_move_arena_size =
                        DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                    .num_threads = 1,
                    .use_heuristics = true,
                    .num_top_moves = 1,
                    .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
                    .skip_word_pruning = true,
                    .soft_time_limit = 5.0,
                    .hard_time_limit = 5.0,
                    // Clamp by cascade deadline — soft/hard_time_limit alone
                    // can't strictly cap a single IDS depth.
                    .external_deadline_ns = peg_clamp_deadline_ns(
                        ctimer_monotonic_ns() + (int64_t)(5.0 * 1.0e9),
                        ctx->deadline_monotonic_ns),
                };
                endgame_solve_inline(&eg, &ea, er);
                mt_arr[i] = mover_lead +
                            endgame_results_get_value(er, ENDGAME_RESULT_BEST);
                endgame_ctx_destroy(eg);
                endgame_results_destroy(er);
              }
            }
            game_destroy(prior_branch);
          }
          // Partial selection sort: bring the top `keep` worst-for-mover
          // entries to the front of opp_cand_idx.
          const int keep = cuts[stage_idx];
          for (int i = 0; i < keep; i++) {
            int min_i = i;
            for (int j = i + 1; j < n_opp_cand; j++) {
              if (mt_arr[j] < mt_arr[min_i]) {
                min_i = j;
              }
            }
            if (min_i != i) {
              int32_t tmp_mt = mt_arr[i];
              mt_arr[i] = mt_arr[min_i];
              mt_arr[min_i] = tmp_mt;
              int tmp_idx = opp_cand_idx[i];
              opp_cand_idx[i] = opp_cand_idx[min_i];
              opp_cand_idx[min_i] = tmp_idx;
            }
          }
          n_opp_cand = keep;
          free(mt_arr);
        }
      }

      const char *rerank_env = getenv("PASSPEG_GREEDY_OPP_RERANK");
      const bool rerank =
          !halve && rerank_env && passpeg_str_to_int(rerank_env) > 0;
      if (rerank && n_opp_cand > 1) {
        int32_t *opp_mt = malloc_or_die((size_t)n_opp_cand * sizeof(int32_t));
        for (int i = 0; i < n_opp_cand; i++) {
          const Move *opp_move_p = move_list_get_move(opp_ml, opp_cand_idx[i]);
          Game *prior_branch = game_duplicate(game);
          game_set_endgame_solving_mode(prior_branch);
          game_set_backup_mode(prior_branch, BACKUP_MODE_OFF);
          play_move(opp_move_p, prior_branch, NULL);
          game_set_game_end_reason(prior_branch, GAME_END_REASON_NONE);
          MoveList *pml = move_list_create(1);
          opp_mt[i] = peg_greedy_playout_pv(prior_branch, ctx->mover_idx, pml,
                                            ctx->worker_idx, NULL, 0, NULL, 0,
                                            NULL, 0, NULL, 0);
          move_list_destroy(pml);
          game_destroy(prior_branch);
        }
        // Selection sort ascending by opp_mt (worst-for-mover first).
        // n_opp_cand is at most ~1000 so this is fine.
        for (int i = 0; i < n_opp_cand; i++) {
          int min_i = i;
          for (int j = i + 1; j < n_opp_cand; j++) {
            if (opp_mt[j] < opp_mt[min_i]) {
              min_i = j;
            }
          }
          if (min_i != i) {
            int32_t tmp_mt = opp_mt[i];
            opp_mt[i] = opp_mt[min_i];
            opp_mt[min_i] = tmp_mt;
            int tmp_idx = opp_cand_idx[i];
            opp_cand_idx[i] = opp_cand_idx[min_i];
            opp_cand_idx[min_i] = tmp_idx;
          }
        }
        free(opp_mt);
      }

      const int n_eval_final =
          n_opp_cand < ctx->opp_top_k ? n_opp_cand : ctx->opp_top_k;

      for (int eval_idx = 0; eval_idx < n_eval_final; eval_idx++) {
        const int opp_rank = opp_cand_idx[eval_idx];
        const Move *opp_move = move_list_get_move(opp_ml, opp_rank);
        Game *branch = game_duplicate(game);
        game_set_endgame_solving_mode(branch);
        game_set_backup_mode(branch, BACKUP_MODE_OFF);
        char opp_move_text[64] = {0};
        {
          StringBuilder *opp_sb = string_builder_create();
          string_builder_add_move(opp_sb, game_get_board(branch), opp_move,
                                  game_get_ld(branch), true);
          (void)snprintf(opp_move_text, sizeof(opp_move_text), "%s",
                         string_builder_peek(opp_sb));
          string_builder_destroy(opp_sb);
        }
        (void)opp_move_text;
        play_move(opp_move, branch, NULL);
        game_set_game_end_reason(branch, GAME_END_REASON_NONE);

        // After opp plays, the bag is empty (mover drew k_drawn tiles, opp
        // drew the remaining bag_remaining tiles). Run endgame_solve at
        // ctx->depth plies for an optimal-play evaluation.
        //
        // For bag-emptier cands (n_bag_remaining == 0) we grant one extra
        // ply: non-emptiers spend their first ply on the rational opp pick
        // (the perception-utility step), leaving (depth-1) plies for the
        // post-pick endgame; emptiers have no opp-perception ply and need
        // the extra ply to compare like-for-like.
        const int eg_plies =
            (ctx->n_bag_remaining == 0) ? ctx->depth + 1 : ctx->depth;
        int32_t branch_total = 0;
        char branch_pv[1024] = {0};
        char branch_mover_rack[32] = {0};
        char branch_opp_rack[32] = {0};
        char branch_final_cgp[512] = {0};
        {
          const LetterDistribution *bld = game_get_ld(branch);
          const int32_t mover_lead =
              equity_to_int(
                  player_get_score(game_get_player(branch, ctx->mover_idx))) -
              equity_to_int(player_get_score(
                  game_get_player(branch, 1 - ctx->mover_idx)));
          if (game_get_game_end_reason(branch) != GAME_END_REASON_NONE) {
            branch_total = mover_lead;
          } else {
            EndgameCtx *eg_ctx = NULL;
            EndgameResults *eg_results = endgame_results_create();
            EndgameArgs ea = {
                .thread_control = ctx->thread_control,
                .game = branch,
                .plies = eg_plies,
                .shared_tt = NULL,
                .initial_small_move_arena_size =
                    DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                .num_threads = 1,
                .use_heuristics = true,
                .num_top_moves = 1,
                .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
                .skip_word_pruning = true,
                .soft_time_limit = 5.0,
                .hard_time_limit = 5.0,
                // Clamp by cascade deadline — soft/hard_time_limit alone
                // can't strictly cap a single IDS depth.
                .external_deadline_ns = peg_clamp_deadline_ns(
                    ctimer_monotonic_ns() + (int64_t)(5.0 * 1.0e9),
                    ctx->deadline_monotonic_ns),
            };
            endgame_solve_inline(&eg_ctx, &ea, eg_results);
            const int eg_val =
                endgame_results_get_value(eg_results, ENDGAME_RESULT_BEST);
            // After opp's move, mover is on turn → eg_val is mover's gain.
            branch_total = mover_lead + eg_val;
            // Optional: capture endgame PV as text.
            if (pv_builder) {
              const PVLine *pv =
                  endgame_results_get_pvline(eg_results, ENDGAME_RESULT_BEST);
              if (pv && pv->num_moves > 0) {
                Game *pv_game = game_duplicate(branch);
                game_set_endgame_solving_mode(pv_game);
                game_set_backup_mode(pv_game, BACKUP_MODE_OFF);
                StringBuilder *pv_sb = string_builder_create();
                for (int mi = 0; mi < pv->num_moves; mi++) {
                  Move m_full;
                  small_move_to_move(&m_full, &pv->moves[mi],
                                     game_get_board(pv_game));
                  if (mi > 0) {
                    string_builder_add_string(pv_sb, " | ");
                  }
                  string_builder_add_move(pv_sb, game_get_board(pv_game),
                                          &m_full, bld, true);
                  play_move(&m_full, pv_game, NULL);
                }
                (void)snprintf(branch_pv, sizeof(branch_pv), "%s",
                               string_builder_peek(pv_sb));
                string_builder_destroy(pv_sb);
                char *cgp = game_get_cgp(pv_game, true);
                (void)snprintf(branch_final_cgp, sizeof(branch_final_cgp), "%s",
                               cgp ? cgp : "");
                free(cgp);
                StringBuilder *rsb = string_builder_create();
                string_builder_add_rack(
                    rsb,
                    player_get_rack(game_get_player(pv_game, ctx->mover_idx)),
                    bld, false);
                (void)snprintf(branch_mover_rack, sizeof(branch_mover_rack),
                               "%s", string_builder_peek(rsb));
                string_builder_destroy(rsb);
                StringBuilder *rsb2 = string_builder_create();
                string_builder_add_rack(rsb2,
                                        player_get_rack(game_get_player(
                                            pv_game, 1 - ctx->mover_idx)),
                                        bld, false);
                (void)snprintf(branch_opp_rack, sizeof(branch_opp_rack), "%s",
                               string_builder_peek(rsb2));
                string_builder_destroy(rsb2);
                game_destroy(pv_game);
              }
            }
            endgame_ctx_destroy(eg_ctx);
            endgame_results_destroy(eg_results);
          }
        }
        game_destroy(branch);

        if (!have_any || branch_total < worst_for_mover) {
          worst_for_mover = branch_total;
          (void)snprintf(worst_opp_text, sizeof(worst_opp_text), "%s",
                         opp_move_text);
          if (pv_builder) {
            // Capture the FULL PV for the worst branch in the TSV row: opp's
            // move first, then the greedy continuation that peg_greedy_*
            // recorded.
            (void)snprintf(pv_text, sizeof(pv_text), "%s%s%s", opp_move_text,
                           branch_pv[0] ? " | " : "", branch_pv);
            (void)snprintf(final_cgp, sizeof(final_cgp), "%s",
                           branch_final_cgp);
            (void)snprintf(mover_rack_end, sizeof(mover_rack_end), "%s",
                           branch_mover_rack);
            (void)snprintf(opp_rack_end, sizeof(opp_rack_end), "%s",
                           branch_opp_rack);
          }
          have_any = true;
        }
      }
      if (pv_builder) {
        string_builder_destroy(pv_builder);
      }
      free(opp_cand_idx);
      move_list_destroy(opp_ml);
      mover_total = worst_for_mover;
      (void)worst_opp_text;
    } // end legacy MIN-over-realized branch
  }
  game_destroy(game);

  peg_lock(ctx->res_mutex);
  ctx->res->weight_sum += weight;
  ctx->res->spread_sum += weight * (int64_t)mover_total;
  if (mover_total > 0) {
    ctx->res->win_x2 += 2 * weight;
  } else if (mover_total == 0) {
    ctx->res->win_x2 += weight;
  }
  ctx->res->n_scen++;
  peg_unlock(ctx->res_mutex);

  if (ctx->tsv_f) {
    peg_lock(ctx->tsv_mutex);
    (void)fprintf(ctx->tsv_f,
                  "%d\t%s\t%d\t%d\t%d\t%s\t%s\t%lld\t%d\t%s\t%s\t%s\t%s\t%s\n",
                  ctx->pos_idx, ctx->cand_txt, ctx->cand_score,
                  ctx->k_drawn + ctx->n_bag_remaining, ctx->k_drawn, drawn_str,
                  remaining_str, (long long)weight, (int)mover_total,
                  post_cand_cgp, final_cgp, pv_text, mover_rack_end,
                  opp_rack_end);
    peg_unlock(ctx->tsv_mutex);
  }
}

// Worker fn invoked by PegPool for each scenario job. Builds a
// per-worker PegEnumCtx pointing at the job's owned (n_multiset,
// mover_pick) and calls peg_emit_split in execute mode (out_jobs=NULL).
// At d=0 emit_split walks bag-tile orderings serially via the helper;
// at d>=1 it runs the opp-top-K + endgame_solve flow.
static void peg_scenario_worker_fn(void *arg, int worker_idx) {
  PegScenarioJob *job = (PegScenarioJob *)arg;
  PegEnumCtx local = *job->base_ctx;
  // Skip if the time budget has expired. The result accumulator just
  // misses this scenario; rankings reflect whichever scenarios completed
  // before the deadline.
  if (local.budget_timer != NULL && local.budget_secs > 0.0 &&
      ctimer_elapsed_seconds(local.budget_timer) > local.budget_secs) {
    return;
  }
  local.n_multiset = job->n_multiset;
  local.mover_pick = job->mover_pick;
  local.worker_idx = worker_idx;
  local.out_jobs = NULL;
  peg_emit_split(&local);
}

static void peg_opp_inner_worker_fn(void *arg, int worker_idx) {
  PegOppInnerJob *j = (PegOppInnerJob *)arg;
  // Bail immediately if the cascade deadline has already fired — don't
  // even allocate the opp_pov_game game. Leaves j->mover_total at its default
  // (0).
  if (j->deadline_monotonic_ns > 0 &&
      ctimer_monotonic_ns() > j->deadline_monotonic_ns) {
    return;
  }
  Game *opp_pov_game = game_duplicate(j->base_game);
  game_set_endgame_solving_mode(opp_pov_game);
  game_set_backup_mode(opp_pov_game, BACKUP_MODE_OFF);
  Bag *opp_pov_bag = game_get_bag(opp_pov_game);
  Rack *opp_pov_mover_rack =
      player_get_rack(game_get_player(opp_pov_game, j->mover_idx));
  for (int ml = 0; ml < j->ld_size; ml++) {
    while (bag_get_letter(opp_pov_bag, (MachineLetter)ml) > 0) {
      (void)bag_draw_letter(opp_pov_bag, (MachineLetter)ml, 0);
    }
  }
  rack_reset(opp_pov_mover_rack);
  // Single bag tile, set deterministically (no PRNG).
  {
    const MachineLetter one = j->bag_ml;
    bag_set_to_tiles(opp_pov_bag, &one, 1);
  }
  for (int t = 0; t < j->n_opp_types; t++) {
    int copies = j->opp_type_counts[t] - (t == j->ti ? 1 : 0);
    for (int k = 0; k < copies; k++) {
      rack_add_letter(opp_pov_mover_rack, j->opp_types[t]);
    }
  }
  play_move(j->opp_move, opp_pov_game, NULL);
  game_set_game_end_reason(opp_pov_game, GAME_END_REASON_NONE);
  int32_t mt = 0;
  if (j->opp_depth == 0) {
    MoveList *pl = move_list_create(1);
    mt = peg_greedy_playout_pv(opp_pov_game, j->mover_idx, pl, worker_idx, NULL,
                               0, NULL, 0, NULL, 0, NULL, 0);
    move_list_destroy(pl);
  } else {
    const int32_t opp_pov_lead =
        equity_to_int(
            player_get_score(game_get_player(opp_pov_game, j->mover_idx))) -
        equity_to_int(
            player_get_score(game_get_player(opp_pov_game, 1 - j->mover_idx)));
    if (game_get_game_end_reason(opp_pov_game) != GAME_END_REASON_NONE) {
      mt = opp_pov_lead;
    } else {
      EndgameCtx *eg = NULL;
      EndgameResults *er = endgame_results_create();
      // Per-call hard wall budget for the opp-inner endgame solve.
      // soft/hard time limits only govern starting the *next* IDS depth;
      // without an external_deadline the *current* depth can run forever
      // on a pathological position. Plumbing this as an absolute
      // monotonic-ns deadline makes check_depth_deadline interrupt
      // mid-search. Configurable via PASSPEG_OPP_INNER_BUDGET (seconds).
      const char *opp_budget_env = getenv("PASSPEG_OPP_INNER_BUDGET");
      const double opp_budget = opp_budget_env && *opp_budget_env
                                    ? passpeg_str_to_double(opp_budget_env)
                                    : 5.0;
      EndgameArgs ea = {
          .thread_control = j->thread_control,
          .game = opp_pov_game,
          .plies = j->opp_depth,
          .shared_tt = NULL,
          .initial_small_move_arena_size =
              DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
          .num_threads = 1,
          .use_heuristics = true,
          .num_top_moves = 1,
          .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
          .skip_word_pruning = true,
          .soft_time_limit = opp_budget,
          .hard_time_limit = opp_budget,
          // Clamp per-call deadline by the cascade deadline so an inner
          // solve can never outlive the outer budget.
          .external_deadline_ns = peg_clamp_deadline_ns(
              ctimer_monotonic_ns() + (int64_t)(opp_budget * 1.0e9),
              j->deadline_monotonic_ns),
      };
      const int64_t solve_start_ns = ctimer_monotonic_ns();
      endgame_solve_inline(&eg, &ea, er);
      const int64_t solve_end_ns = ctimer_monotonic_ns();
      const double solve_secs = (double)(solve_end_ns - solve_start_ns) / 1.0e9;
      if (solve_secs > opp_budget * 1.2 + 0.2) {
        // Flag if endgame_solve didn't honor the external deadline.
        (void)fprintf(stderr,
                      "[opp_inner SLOW] worker=%d plies=%d solve=%.2fs"
                      " (deadline=%.2fs — endgame_solve isn't honoring it!)\n",
                      worker_idx, j->opp_depth, solve_secs, opp_budget);
        (void)fflush(stderr);
      }
      // If the 5s deadline fired before any depth completed, the result
      // value is uninitialized — use the opp_pov_game's mover_lead as a
      // fallback so downstream arithmetic doesn't propagate garbage.
      const int eg_depth = endgame_results_get_depth(er, ENDGAME_RESULT_BEST);
      if (eg_depth < 0) {
        mt = opp_pov_lead;
      } else {
        const int eg_val = endgame_results_get_value(er, ENDGAME_RESULT_BEST);
        mt = opp_pov_lead + eg_val;
      }
      endgame_ctx_destroy(eg);
      endgame_results_destroy(er);
    }
  }
  game_destroy(opp_pov_game);
  j->mover_total = mt;
}

// Generalized opp_pov_game worker: bag = opp_pov_bag_counts, mover_rack = total
// - opp_pov_bag_counts. Applies opp_move and runs greedy playout to game end.
static void peg_opp_pov_worker_fn(void *arg, int worker_idx) {
  PegOppPovJob *j = (PegOppPovJob *)arg;
  Game *opp_pov_game = game_duplicate(j->base_game);
  game_set_endgame_solving_mode(opp_pov_game);
  game_set_backup_mode(opp_pov_game, BACKUP_MODE_OFF);
  Bag *opp_pov_bag = game_get_bag(opp_pov_game);
  Rack *opp_pov_mover_rack =
      player_get_rack(game_get_player(opp_pov_game, j->mover_idx));
  // Drain bag and mover rack.
  for (int ml = 0; ml < j->ld_size; ml++) {
    while (bag_get_letter(opp_pov_bag, (MachineLetter)ml) > 0) {
      (void)bag_draw_letter(opp_pov_bag, (MachineLetter)ml, 0);
    }
  }
  rack_reset(opp_pov_mover_rack);
  // Bag = opp_pov_bag_counts, set deterministically (no PRNG). bag_add_letter
  // would randomize order via the time-seeded PRNG.
  {
    MachineLetter bag_tiles[MAX_BAG_SIZE] = {0};
    int n_bag = 0;
    for (int t = 0; t < j->n_opp_types; t++) {
      for (int k = 0; k < j->opp_pov_bag_counts[t]; k++) {
        bag_tiles[n_bag++] = j->opp_types[t];
      }
    }
    bag_set_to_tiles(opp_pov_bag, bag_tiles, n_bag);
  }
  // Mover rack = total - opp_pov_bag_counts.
  for (int t = 0; t < j->n_opp_types; t++) {
    const int copies = j->opp_type_counts[t] - j->opp_pov_bag_counts[t];
    for (int k = 0; k < copies; k++) {
      rack_add_letter(opp_pov_mover_rack, j->opp_types[t]);
    }
  }
  // Capture the opp_pov_game mover rack BEFORE opp plays — that's the rack the
  // leaf evaluator sees and that the report should show. After play_move opp
  // draws from the bag (changing nothing on mover's rack), but the rack is
  // already what we want here.
  char opp_pov_mover_rack_str[32] = {0};
  if (j->outer_ctx && j->outer_ctx->inner_tsv_f) {
    const LetterDistribution *ld = game_get_ld(opp_pov_game);
    StringBuilder *rsb = string_builder_create();
    string_builder_add_rack(rsb, opp_pov_mover_rack, ld, false);
    (void)snprintf(opp_pov_mover_rack_str, sizeof(opp_pov_mover_rack_str), "%s",
                   string_builder_peek(rsb));
    string_builder_destroy(rsb);
  }
  play_move(j->opp_move, opp_pov_game, NULL);
  game_set_game_end_reason(opp_pov_game, GAME_END_REASON_NONE);
  // Cache lookup: if this exact post-opp game state has been evaluated
  // before in this cand's pass, reuse the result. Big win when two opp-POV
  // states collapse to the same (board, mover_rack, opp_rack, bag) state —
  // common because many opp moves leave the bag empty and the rest of the state
  // is determined by the realized opp_pov_game's bag-content.
  uint64_t cache_key = 0;
  bool cache_hit = false;
  int32_t cache_hit_mt = 0;
  if (j->outer_ctx && j->outer_ctx->opp_pov_cache) {
    cache_key = peg_hash_game_state(opp_pov_game, j->mover_idx);
    cache_hit = peg_opp_pov_cache_lookup(j->outer_ctx->opp_pov_cache, cache_key,
                                         &cache_hit_mt);
  }
  // PASSPEG_INNER_USE_ENDGAME=N (plies): when the bag is empty after opp
  // plays (i.e., this was effectively a 1-tile-in-bag inner state for the
  // opp-POV state), swap greedy for an N-ply endgame_solve. Greedy
  // systematically misvalues bingo-blocking and going-out positions; in 2peg
  // pos 28 this asymmetry inflated K=1 A(N) over K=2 ACIDOT(I)c (the latter
  // already gets endgame_solve via PASSPEG_EMPTIER_USE_ENDGAME at the outer K=N
  // path). Inner endgame_solve at the K<N walker leaf is OFF by default — the
  // walker's "1peg" cands use greedy playout. Turn it on per-stage via
  // PASSPEG_INNER_USE_ENDGAME=N (= use N-ply IDS at the leaf). Optional
  // per-call wall cap via PASSPEG_INNER_EG_BUDGET (seconds, default 0
  // = no budget, let the IDS complete N plies naturally).
  const char *inner_eg_env = getenv("PASSPEG_INNER_USE_ENDGAME");
  const int inner_eg_plies =
      (inner_eg_env && *inner_eg_env) ? passpeg_str_to_int(inner_eg_env) : 0;
  const char *inner_eg_budget_env = getenv("PASSPEG_INNER_EG_BUDGET");
  const double inner_eg_budget =
      inner_eg_budget_env && *inner_eg_budget_env
          ? passpeg_str_to_double(inner_eg_budget_env)
          : 0.0;
  const bool bag_empty_post_opp =
      (bag_get_letters(game_get_bag(opp_pov_game)) == 0);
  int32_t mt = 0;
  char inner_pv_text[1024] = {0};
  int inner_pv_plies = 0; // 0 == greedy path; >0 == endgame depth used
  int inner_eg_depth_reached = 0;
  int inner_eg_status = 0; // 0 unset / 1 finished / 2 interrupted
  int inner_eg_num_moves = 0;
  int inner_eg_negamax_depth = 0;
  double inner_eg_start_ms = 0.0; // PEG-relative timestamp (ms)
  double inner_eg_dur_ms = 0.0;
  PegInnerEgDepthLog inner_eg_log = {0};
  // For the greedy path we time the playout with this dedicated timer so
  // the inner-TSV report can show stage-1 (walker + greedy leaf)
  // per-opp_pov_game wall time too, not just the endgame-leaf path.
  Timer greedy_timer = {0};
  if (j->outer_ctx && j->outer_ctx->inner_tsv_f && j->outer_ctx->budget_timer) {
    inner_eg_start_ms =
        ctimer_elapsed_seconds(j->outer_ctx->budget_timer) * 1000.0;
  }
  if (cache_hit) {
    mt = cache_hit_mt;
    inner_pv_plies = 0;
    // (skip both endgame and greedy paths — we have the answer)
  } else if (inner_eg_plies > 0 && bag_empty_post_opp &&
             game_get_game_end_reason(opp_pov_game) == GAME_END_REASON_NONE &&
             j->outer_ctx) {
    const int32_t mover_lead =
        equity_to_int(
            player_get_score(game_get_player(opp_pov_game, j->mover_idx))) -
        equity_to_int(
            player_get_score(game_get_player(opp_pov_game, 1 - j->mover_idx)));
    // Reuse this worker's persistent EndgameCtx across calls. The slot
    // is keyed by worker_idx; first call allocates, subsequent calls
    // reset+reuse to amortize setup cost (ABDADA init, move sort, etc.).
    EndgameCtx **eg_ctx_pp = NULL;
    EndgameCtx *eg_ctx_local = NULL;
    {
      const int slot =
          worker_idx == 0
              ? 0
              : (worker_idx - j->outer_ctx->per_worker_eg_ctx_offset + 1);
      if (j->outer_ctx->per_worker_eg_ctx && slot >= 0 &&
          slot < j->outer_ctx->per_worker_eg_ctx_n) {
        eg_ctx_pp = &j->outer_ctx->per_worker_eg_ctx[slot];
      } else {
        eg_ctx_pp = &eg_ctx_local;
      }
    }
    EndgameResults *eg_results = endgame_results_create();
    // Initialize the per-call depth log so the per_ply_callback can
    // record (depth, value, ms) tuples. inner_eg_start_ms was set above
    // before the if/else branch so both endgame and greedy paths share it.
    ctimer_start(&inner_eg_log.call_timer);
    inner_eg_log.n = 0;
    EndgameArgs ea = {
        .thread_control = j->thread_control,
        .game = opp_pov_game,
        .plies = inner_eg_plies,
        .shared_tt = j->outer_ctx->shared_eg_tt,
        .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
        .num_threads = 1,
        .use_heuristics = true,
        .num_top_moves = 1,
        .per_ply_callback =
            j->outer_ctx->inner_tsv_f ? peg_inner_eg_per_ply_cb : NULL,
        .per_ply_callback_data =
            j->outer_ctx->inner_tsv_f ? (void *)&inner_eg_log : NULL,
        .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
        .skip_word_pruning = true,
        // Per-call wall budget. external_deadline_ns is what actually
        // interrupts an in-flight iteration (check_depth_deadline polls
        // it every 1024 nodes); soft_time_limit only governs whether to
        // start the *next* depth. So we set BOTH: soft/hard guides EBF,
        // external_deadline is the hard wall stop.
        //
        // Clamp the per-call deadline by the cascade's outer deadline so
        // a single inner endgame solve can't outlive the cascade budget.
        // Without this, a deep K<N walker leaf at d=4 can run 60s+ even
        // when the cascade has 5s remaining.
        .soft_time_limit = inner_eg_budget,
        .hard_time_limit = inner_eg_budget,
        .external_deadline_ns = peg_clamp_deadline_ns(
            ctimer_monotonic_ns() + (int64_t)(inner_eg_budget * 1.0e9),
            // cppcheck-suppress knownConditionTrueFalse ; outer_ctx is non-NULL
            // on the paths reaching here; the guard is kept as defensive code.
            j->outer_ctx ? j->outer_ctx->deadline_monotonic_ns : 0),
    };
    endgame_solve_inline(eg_ctx_pp, &ea, eg_results);
    inner_eg_dur_ms = ctimer_elapsed_seconds(&inner_eg_log.call_timer) * 1000.0;
    inner_eg_depth_reached =
        endgame_results_get_depth(eg_results, ENDGAME_RESULT_BEST);
    if (inner_eg_depth_reached < 0) {
      // The per-call wall budget expired before any IDS iteration
      // completed (rare with a sane 1s budget). Fall back to greedy so
      // we still produce a real number for this opp_pov_game; mark incomplete
      // only for stats — the cand stays in the ranking.
      MoveList *pl_fb = move_list_create(1);
      mt = peg_greedy_playout_pv(opp_pov_game, j->mover_idx, pl_fb, worker_idx,
                                 NULL, 0, NULL, 0, NULL, 0, NULL, 0);
      move_list_destroy(pl_fb);
      inner_pv_plies = 0;
      j->incomplete = true;
    } else {
      const int turn = game_get_player_on_turn_index(opp_pov_game);
      const int32_t eg_val =
          endgame_results_get_value(eg_results, ENDGAME_RESULT_BEST);
      mt = (turn == j->mover_idx) ? mover_lead + eg_val : mover_lead - eg_val;
      inner_pv_plies = inner_eg_plies;
    }
    {
      endgame_result_status_t st = endgame_results_get_status(eg_results);
      if (st == ENDGAME_RESULT_STATUS_FINISHED) {
        inner_eg_status = 1;
      } else if (st == ENDGAME_RESULT_STATUS_INTERRUPTED) {
        inner_eg_status = 2;
      } else {
        inner_eg_status = 0;
      }
    }
    // Extract endgame PV for the inner TSV.
    if (j->outer_ctx->inner_tsv_f) {
      const PVLine *pv =
          endgame_results_get_pvline(eg_results, ENDGAME_RESULT_BEST);
      if (pv) {
        inner_eg_num_moves = pv->num_moves;
        inner_eg_negamax_depth = pv->negamax_depth;
      }
      if (pv && pv->num_moves > 0) {
        Game *pv_game = game_duplicate(opp_pov_game);
        game_set_endgame_solving_mode(pv_game);
        game_set_backup_mode(pv_game, BACKUP_MODE_OFF);
        StringBuilder *pv_sb = string_builder_create();
        for (int mi = 0; mi < pv->num_moves; mi++) {
          Move m_full;
          small_move_to_move(&m_full, &pv->moves[mi], game_get_board(pv_game));
          if (mi > 0) {
            string_builder_add_string(pv_sb, " | ");
          }
          string_builder_add_move(pv_sb, game_get_board(pv_game), &m_full,
                                  game_get_ld(pv_game), true);
          play_move(&m_full, pv_game, NULL);
        }
        (void)snprintf(inner_pv_text, sizeof(inner_pv_text), "%s",
                       string_builder_peek(pv_sb));
        string_builder_destroy(pv_sb);
        game_destroy(pv_game);
      }
    }
    // Only destroy the ctx if we allocated it as a local (not a shared
    // per-worker slot). Per-worker ctxs are destroyed by the cand
    // dispatcher after all the cand's scenarios complete.
    if (eg_ctx_pp == &eg_ctx_local) {
      endgame_ctx_destroy(eg_ctx_local);
    }
    endgame_results_destroy(eg_results);
  } else {
    // Greedy fallback. Capture the PV when an inner TSV is requested so
    // the report can show what greedy played out — it's the "old answer"
    // we're contrasting endgame against. Time the playout so stage-1
    // (walker + greedy leaf) per-opp_pov_game wall time is visible too.
    if (j->outer_ctx && j->outer_ctx->inner_tsv_f) {
      ctimer_start(&greedy_timer);
    }
    MoveList *pl = move_list_create(1);
    if (j->outer_ctx && j->outer_ctx->inner_tsv_f) {
      mt = peg_greedy_playout_pv(opp_pov_game, j->mover_idx, pl, worker_idx,
                                 inner_pv_text, sizeof(inner_pv_text), NULL, 0,
                                 NULL, 0, NULL, 0);
    } else {
      mt = peg_greedy_playout_pv(opp_pov_game, j->mover_idx, pl, worker_idx,
                                 NULL, 0, NULL, 0, NULL, 0, NULL, 0);
    }
    move_list_destroy(pl);
    if (j->outer_ctx && j->outer_ctx->inner_tsv_f) {
      inner_eg_dur_ms = ctimer_elapsed_seconds(&greedy_timer) * 1000.0;
      // inner_pv_plies stays 0 → greedy path indicator in the report.
    }
  }
  // Store the result so a later opp_pov_game / scenario landing on the same
  // post-opp state can short-circuit. Skip storing on cache_hit (already
  // there) and on incomplete (depth=-1 fallback result is greedy and may
  // disagree with a later proper-endgame attempt at the same state — let
  // the later call recompute rather than poison the cache).
  if (!cache_hit && !j->incomplete && cache_key && j->outer_ctx &&
      j->outer_ctx->opp_pov_cache) {
    peg_opp_pov_cache_store(j->outer_ctx->opp_pov_cache, cache_key, mt);
  }
  // Optional per-inner opp-POV TSV. Each row: outer realized tiles, opp cand
  // text + score, opp-POV bag composition (canonical short tile string),
  // opp_pov_game weight, leaf mt. Consumer groups by (cand, scenario, opp).
  if (j->outer_ctx && j->outer_ctx->inner_tsv_f) {
    const LetterDistribution *ld = game_get_ld(j->base_game);
    char opp_pov_bag_str[32] = {0};
    int opp_pov_pos = 0;
    for (int t = 0; t < j->n_opp_types && opp_pov_pos < 30; t++) {
      for (int k = 0; k < j->opp_pov_bag_counts[t] && opp_pov_pos < 30; k++) {
        opp_pov_bag_str[opp_pov_pos++] = ld->ld_ml_to_hl[j->opp_types[t]][0];
      }
    }
    char depth_log_str[256] = {0};
    int dl_off = 0;
    for (int i = 0;
         i < inner_eg_log.n && dl_off + 24 < (int)sizeof(depth_log_str); i++) {
      dl_off +=
          snprintf(depth_log_str + dl_off, sizeof(depth_log_str) - dl_off,
                   "%s%d@%.1f=%+d", i == 0 ? "" : ";", inner_eg_log.depths[i],
                   inner_eg_log.times_ms[i], (int)inner_eg_log.values[i]);
    }
    peg_lock(j->outer_ctx->inner_tsv_mutex);
    (void)fprintf(j->outer_ctx->inner_tsv_f,
                  "%s\t%s\t%s\t%s\t%d\t%s\t%lld\t%d\t%d\t%d\t%d\t%d\t%d"
                  "\t%.1f\t%.1f\t%s\t%s\t%s\n",
                  j->outer_ctx->cand_txt ? j->outer_ctx->cand_txt : "",
                  j->outer_drawn_str ? j->outer_drawn_str : "",
                  j->outer_remaining_str ? j->outer_remaining_str : "",
                  j->opp_move_text ? j->opp_move_text : "", j->opp_move_score,
                  opp_pov_bag_str, (long long)j->opp_pov_weight, (int)mt,
                  inner_pv_plies, inner_eg_depth_reached, inner_eg_status,
                  inner_eg_num_moves, inner_eg_negamax_depth, inner_eg_start_ms,
                  inner_eg_dur_ms, depth_log_str, opp_pov_mover_rack_str,
                  inner_pv_text);
    peg_unlock(j->outer_ctx->inner_tsv_mutex);
  }
  // Note: we no longer mark the cand "incomplete" when a opp_pov_game fell back
  // to greedy — the user's design is "return the best result when
  // interrupted", and the greedy fallback IS a valid (cheaper) leaf
  // eval. Stage-level interruption is still tracked via budget_hit at
  // the wrapper level.
  game_destroy(opp_pov_game);
  j->mover_total = mt;
}

// Recursive accumulator: for each n_bag-multiset of perceived pool,
// build a opp_pov_game, run greedy, fold into opp's win/spread totals (and
// stash the realized mt when the multiset matches the realized bag).
typedef struct PegOppPovEnumCtx {
  const PegEnumCtx *outer_ctx;
  const Game *walker;
  const Move *opp_move;
  int n_opp_types;
  const MachineLetter *opp_types;
  const int *opp_type_counts;
  const int *realized_bag_counts;
  int *cur_opp_pov_bag; // size n_opp_types
  // accumulators
  int64_t total_weight;
  double win_x2_sum;      // weighted by opp-POV state weight, opp's POV
  int64_t spread_sum_opp; // weighted spread sum, opp's POV
  bool realized_set;
  int32_t realized_mt;
  // PASSPEG_PERCEPTION_STRIDE: stride-style stratified sampling on the
  // opp-POV state enumeration. perception_stride <= 1 means full enumeration.
  // opp_pov_weight_seen is a per-call (single-threaded) running counter of
  // opp-POV state weights, used for stride boundary tracking.
  int perception_stride;
  int64_t opp_pov_weight_seen;
  // Carried through to PegOppPovJob for inner-TSV row annotation.
  const char *outer_drawn_str;
  const char *outer_remaining_str;
  const char *opp_move_text;
  int opp_move_score;
} PegOppPovEnumCtx;

static void peg_enum_opp_pov_recursive(PegOppPovEnumCtx *e, int type_idx,
                                       int remaining) {
  // Cascade-budget poll. enum_opp_pov_recursive calls peg_opp_pov_worker_fn
  // SYNCHRONOUSLY per leaf opp_pov_game; without this poll a single scenario
  // can run hundreds of game_duplicate + greedy_playout calls past the outer
  // wall budget. Bail out early once the deadline has fired — callers treat
  // partial opp_pov_game coverage as "scenario skipped" (out_utility stays at
  // its initial value, no contribution to the result accumulator).
  if (e->outer_ctx && e->outer_ctx->deadline_monotonic_ns > 0 &&
      ctimer_monotonic_ns() > e->outer_ctx->deadline_monotonic_ns) {
    return;
  }
  if (type_idx == e->n_opp_types) {
    if (remaining != 0) {
      return;
    }
    int64_t weight = 1;
    for (int t = 0; t < e->n_opp_types; t++) {
      weight *= peg_binomial(e->opp_type_counts[t], e->cur_opp_pov_bag[t]);
    }
    // Identify the realized opp-POV state (the one whose bag composition
    // matches the actual bag). We always want its mt for the scenario's
    // realized outcome, even if perception stride would otherwise skip it.
    bool is_realized = true;
    for (int t = 0; t < e->n_opp_types; t++) {
      if (e->cur_opp_pov_bag[t] != e->realized_bag_counts[t]) {
        is_realized = false;
        break;
      }
    }
    // Apply perception stride: stride-style stratified sampling over
    // opp-POV states, weighted by multinomial weight. Sampled opp-POV states
    // contribute to the utility accumulators with weight scaled by samples *
    // stride; Skipped opp-POV states don't contribute. Realized opp-POV state
    // is always evaluated (cost: one extra job at most per perception call) so
    // realized_mt is always available — but it only contributes to the utility
    // accumulators when stride samples it.
    const int64_t old_seen = e->opp_pov_weight_seen;
    e->opp_pov_weight_seen += weight;
    bool sampled = true;
    int64_t effective_weight = weight;
    if (e->perception_stride > 1) {
      const int64_t samples =
          (old_seen + weight) / (int64_t)e->perception_stride -
          old_seen / (int64_t)e->perception_stride;
      if (samples == 0) {
        sampled = false;
      } else {
        effective_weight = samples * (int64_t)e->perception_stride;
      }
    }
    if (!sampled && !is_realized) {
      return; // stride-skipped and not the realized opp_pov_game; nothing to
              // do.
    }
    PegOppPovJob job = {
        .base_game = e->walker,
        .mover_idx = e->outer_ctx->mover_idx,
        .ld_size = e->outer_ctx->ld_size,
        .opp_move = e->opp_move,
        .n_opp_types = e->n_opp_types,
        .opp_types = e->opp_types,
        .opp_type_counts = e->opp_type_counts,
        .opp_pov_bag_counts = e->cur_opp_pov_bag,
        .thread_control = e->outer_ctx->thread_control,
        .outer_ctx = e->outer_ctx,
        .outer_drawn_str = e->outer_drawn_str,
        .outer_remaining_str = e->outer_remaining_str,
        .opp_move_text = e->opp_move_text,
        .opp_move_score = e->opp_move_score,
        .opp_pov_weight = weight,
        .mover_total = 0,
    };
    peg_opp_pov_worker_fn(&job, e->outer_ctx->worker_idx);
    const int32_t mt = job.mover_total;
    if (sampled) {
      const int32_t mt_opp = -mt;
      e->total_weight += effective_weight;
      e->spread_sum_opp += (int64_t)mt_opp * effective_weight;
      if (mt_opp > 0) {
        e->win_x2_sum += 2.0 * (double)effective_weight;
      } else if (mt_opp == 0) {
        e->win_x2_sum += (double)effective_weight;
      }
    }
    if (is_realized && !e->realized_set) {
      e->realized_mt = mt;
      e->realized_set = true;
    }
    return;
  }
  const int max_take = e->opp_type_counts[type_idx] < remaining
                           ? e->opp_type_counts[type_idx]
                           : remaining;
  for (int k = 0; k <= max_take; k++) {
    e->cur_opp_pov_bag[type_idx] = k;
    peg_enum_opp_pov_recursive(e, type_idx + 1, remaining - k);
  }
  e->cur_opp_pov_bag[type_idx] = 0;
}

// Public-ish helper: evaluate opp's utility for one opp_move at the
// given walker state, averaging over all n_bag-multisets of opp's
// perceived pool (weighted by multinomial). Also returns the realized
// mt under the actual bag composition (used as scenario outcome at the
// final ply).
static void peg_eval_opp_with_perception(
    const PegEnumCtx *outer_ctx, const Game *walker, const Move *opp_move,
    const MachineLetter *opp_types, const int *opp_type_counts, int n_opp_types,
    int n_bag_now, const int *realized_bag_counts, double alpha,
    double *out_utility, int32_t *out_realized_mt, const char *outer_drawn_str,
    const char *outer_remaining_str, const char *opp_move_text,
    int opp_move_score) {
  int cur_opp_pov_bag[MAX_ALPHABET_SIZE] = {0};
  // Perception stride only fires when the root bag size is large enough
  // that the perception opp-POV state space justifies sampling. At root bag
  // <= 2 (1peg / 2peg) the space is small enough to enumerate; sampling
  // 1/k of it throws away too much coverage.
  const int root_bag_size = outer_ctx->k_drawn + outer_ctx->n_bag_remaining;
  const char *perception_stride_env = getenv("PASSPEG_PERCEPTION_STRIDE");
  const int perception_stride =
      (root_bag_size >= 3 && n_bag_now >= 2 && perception_stride_env &&
       *perception_stride_env)
          ? passpeg_str_to_int(perception_stride_env)
          : 1;
  PegOppPovEnumCtx e = {
      .outer_ctx = outer_ctx,
      .walker = walker,
      .opp_move = opp_move,
      .n_opp_types = n_opp_types,
      .opp_types = opp_types,
      .opp_type_counts = opp_type_counts,
      .realized_bag_counts = realized_bag_counts,
      .cur_opp_pov_bag = cur_opp_pov_bag,
      .total_weight = 0,
      .win_x2_sum = 0.0,
      .spread_sum_opp = 0,
      .realized_set = false,
      .realized_mt = 0,
      .perception_stride = perception_stride,
      .opp_pov_weight_seen = 0,
      .outer_drawn_str = outer_drawn_str,
      .outer_remaining_str = outer_remaining_str,
      .opp_move_text = opp_move_text,
      .opp_move_score = opp_move_score,
  };
  peg_enum_opp_pov_recursive(&e, 0, n_bag_now);
  if (e.total_weight > 0) {
    const double winpct = e.win_x2_sum / (2.0 * (double)e.total_weight);
    const double mean_spread =
        (double)e.spread_sum_opp / (double)e.total_weight;
    *out_utility = winpct + alpha * mean_spread;
  } else {
    *out_utility = -1e9;
  }
  *out_realized_mt = e.realized_mt;
}

// For a fixed N-multiset in ctx->n_multiset, enumerate all mover-drawn
// sub-multisets of size k_drawn (= per-type counts summing to k_drawn).
static void peg_enum_mover_drawn(PegEnumCtx *ctx, int type_idx, int remaining) {
  if (type_idx == ctx->k_types) {
    if (remaining == 0) {
      peg_emit_split(ctx);
    }
    return;
  }
  const int max_take = ctx->n_multiset[type_idx] < remaining
                           ? ctx->n_multiset[type_idx]
                           : remaining;
  for (int take = 0; take <= max_take; take++) {
    ctx->mover_pick[type_idx] = take;
    peg_enum_mover_drawn(ctx, type_idx + 1, remaining - take);
  }
  ctx->mover_pick[type_idx] = 0;
}

// Enumerate all N-multisets from `type_counts` (the unseen pool). For each
// emitted multiset, fan out to peg_enum_mover_drawn to enumerate splits.
static void peg_enum_outer_multiset(PegEnumCtx *ctx, int type_idx,
                                    int remaining) {
  if (type_idx == ctx->k_types) {
    if (remaining == 0) {
      peg_enum_mover_drawn(ctx, 0, ctx->k_drawn);
    }
    return;
  }
  const int max_take = ctx->type_counts[type_idx] < remaining
                           ? ctx->type_counts[type_idx]
                           : remaining;
  for (int take = 0; take <= max_take; take++) {
    ctx->n_multiset[type_idx] = take;
    peg_enum_outer_multiset(ctx, type_idx + 1, remaining - take);
  }
  ctx->n_multiset[type_idx] = 0;
}

void test_pass_peg_greedy_bench(void) {
  const char *path_env = getenv("PASSPEG_GREEDY_PATH");
  const char *path =
      path_env && *path_env ? path_env : "/tmp/peg_positions.txt";
  const char *topk_env = getenv("PASSPEG_GREEDY_TOP_K");
  int top_k = topk_env && *topk_env ? passpeg_str_to_int(topk_env) : 15;
  const char *only_env = getenv("PASSPEG_GREEDY_ONLY");
  const char *only_moves = only_env && *only_env ? only_env : NULL;
  const char *only_scen_env = getenv("PASSPEG_GREEDY_ONLY_SCEN");
  const char *scenario_filter =
      only_scen_env && *only_scen_env ? only_scen_env : NULL;
  const char *depth_env = getenv("PASSPEG_GREEDY_DEPTH");
  const int depth = depth_env && *depth_env ? passpeg_str_to_int(depth_env) : 0;
  const char *opp_topk_env = getenv("PASSPEG_GREEDY_OPP_TOP_K");
  const int opp_top_k =
      opp_topk_env && *opp_topk_env ? passpeg_str_to_int(opp_topk_env) : 8;
  const char *opp_match_env = getenv("PASSPEG_GREEDY_OPP_MATCH");
  const char *opp_move_filter =
      opp_match_env && *opp_match_env ? opp_match_env : NULL;
  const char *tsv_env = getenv("PASSPEG_GREEDY_TSV");
  const char *tsv_path = tsv_env && *tsv_env ? tsv_env : NULL;
  const char *inner_tsv_env = getenv("PASSPEG_INNER_TSV");
  const char *inner_tsv_path =
      inner_tsv_env && *inner_tsv_env ? inner_tsv_env : NULL;
  // PASSPEG_GREEDY_RESULT_FILE: when set, write a machine-readable TSV of
  // the ranked results (one row per cand) so a cascade driver can read the
  // ranking instead of grepping stderr. Columns:
  //   pos  rank  win  spread  scen  weight  tiles_played  bucket  cand_text
  // bucket: 0=capable-empt (tp>=bag, post-rack>=5), 1=non-empt (tp<bag),
  //         2=incapable-empt (tp>=bag, post-rack<5). rack_size assumed 7.
  const char *result_env = getenv("PASSPEG_GREEDY_RESULT_FILE");
  const char *result_path = result_env && *result_env ? result_env : NULL;
  const char *threads_env = getenv("PASSPEG_GREEDY_THREADS");
  const int n_threads =
      threads_env && *threads_env ? passpeg_str_to_int(threads_env) : 1;

  // Persistent worker pool reused across positions and across the outer
  // (cand × scenario) loop AND the inner opp utility sweep. workers
  // claim indices in [100, 100 + n_threads); the main thread runs at
  // helper_worker_idx = 0, outside that range.
  PegPool *executor = n_threads > 1 ? peg_pool_create(n_threads, 100) : NULL;
  cpthread_mutex_t res_mutex;
  cpthread_mutex_t tsv_mutex;
  cpthread_mutex_t endgame_mutex;
  cpthread_mutex_t inner_tsv_mutex;
  cpthread_mutex_init(&res_mutex);
  cpthread_mutex_init(&tsv_mutex);
  cpthread_mutex_init(&endgame_mutex);
  cpthread_mutex_init(&inner_tsv_mutex);

  char **cgps = NULL;
  int n_pos = load_cgp_lines(path, &cgps, 1000);
  if (n_pos == 0) {
    log_fatal("no positions loaded from %s", path);
  }
  (void)fprintf(
      stderr,
      "[peggreedy] loaded %d positions from %s  depth=%d  opp_top_k=%d"
      "  threads=%d"
      "%s%s\n",
      n_pos, path, depth, opp_top_k, n_threads,
      scenario_filter ? "  scenario_filter=" : "",
      scenario_filter ? scenario_filter : "");
  (void)fflush(stderr);

  FILE *tsv_f = NULL;
  if (tsv_path) {
    tsv_f = fopen_or_die(tsv_path, "we");
    (void)fprintf(tsv_f,
                  "pos\tcand\tcand_score\tN\tK_drawn\tdrawn\tremaining\tweight"
                  "\tmover_total\tpost_cand_cgp\tfinal_cgp\tpv_text"
                  "\tmover_rack_end\topp_rack_end\n");
  }

  FILE *inner_tsv_f = NULL;
  if (inner_tsv_path) {
    inner_tsv_f = fopen_or_die(inner_tsv_path, "we");
    (void)fprintf(
        inner_tsv_f,
        "cand\tdrawn\tremaining\topp_move\topp_score\topp_pov_bag\topp_pov_"
        "weight"
        "\tmover_total\teg_plies\teg_depth\teg_status\teg_pv_moves"
        "\teg_pv_negamax\teg_start_ms\teg_dur_ms\teg_depth_log"
        "\topp_pov_mover_rack\teg_pv\n");
  }

  FILE *result_f = NULL;
  if (result_path) {
    result_f = fopen_or_die(result_path, "we");
    (void)fprintf(result_f,
                  "pos\trank\twin\tspread\tscen\tweight\ttiles_played\tbucket"
                  "\tcand_text\n");
  }

  for (int pi = 0; pi < n_pos; pi++) {
    Config *config = config_create_or_die("set -s1 score -s2 score");
    char load_cmd[10240];
    (void)snprintf(load_cmd, sizeof(load_cmd), "cgp %s", cgps[pi]);
    load_and_exec_config_or_die(config, load_cmd);
    Game *game = config_get_game(config);
    int mover_idx = game_get_player_on_turn_index(game);
    const LetterDistribution *ld = game_get_ld(game);
    int lds = ld_get_size(ld);

    // Unseen pool = full distribution − mover's rack − board tiles. Opp's
    // rack as it appears in the CGP is ignored: PEG always treats those
    // tiles as unknown. Bag size N then follows by: opp gets RACK_SIZE of
    // the unseen, bag gets the rest.
    uint8_t unseen[MAX_ALPHABET_SIZE];
    int total_unseen = peg_compute_unseen(game, mover_idx, unseen);
    int N = total_unseen - RACK_SIZE;
    if (N < 0) {
      N = 0;
    }

    MachineLetter types[MAX_ALPHABET_SIZE];
    int counts[MAX_ALPHABET_SIZE];
    int k_types = 0;
    for (int ml = 0; ml < lds; ml++) {
      if (unseen[ml] > 0) {
        types[k_types] = (MachineLetter)ml;
        counts[k_types] = (int)unseen[ml];
        k_types++;
      }
    }

    MoveList *ml_cands = move_list_create(16384);
    const MoveGenArgs ga = {
        .game = game,
        .move_list = ml_cands,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&ga);
    int n_all = move_list_get_count(ml_cands);

    (void)fprintf(stderr,
                  "[peggreedy] pos %d  N=%d  unseen=%d  k_types=%d  cands=%d\n",
                  pi, N, total_unseen, k_types, n_all);
    if (getenv("PASSPEG_DUMP_CANDS")) {
      const int dump_top = 32;
      (void)fprintf(stderr, "[cand_dump] top-%d by equity (sorted desc):\n",
                    dump_top);
      const Rack *mr = player_get_rack(game_get_player(game, mover_idx));
      // Build an index array sorted by equity descending. MoveList from
      // MOVE_RECORD_ALL is heap-ordered, not fully sorted.
      int *eq_order = malloc_or_die((size_t)n_all * sizeof(int));
      for (int i = 0; i < n_all; i++) {
        eq_order[i] = i;
      }
      // Insertion sort top-dump_top by descending equity.
      for (int i = 0; i < dump_top && i < n_all; i++) {
        int best = i;
        Equity best_eq =
            move_get_equity(move_list_get_move(ml_cands, eq_order[i]));
        for (int j = i + 1; j < n_all; j++) {
          Equity je =
              move_get_equity(move_list_get_move(ml_cands, eq_order[j]));
          if (je > best_eq) {
            best = j;
            best_eq = je;
          }
        }
        if (best != i) {
          int tmp = eq_order[i];
          eq_order[i] = eq_order[best];
          eq_order[best] = tmp;
        }
      }
      int printed = 0;
      for (int rank = 0; rank < n_all && printed < dump_top; rank++) {
        const Move *m = move_list_get_move(ml_cands, eq_order[rank]);
        if (move_get_type(m) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
          continue;
        }
        printed++;
        // Compute leave: mover_rack minus the tiles played.
        Rack leave;
        rack_set_dist_size(&leave, ld_get_size(ld));
        rack_reset(&leave);
        for (int t = 0; t < ld_get_size(ld); t++) {
          int n = rack_get_letter(mr, t);
          for (int k = 0; k < n; k++) {
            rack_add_letter(&leave, t);
          }
        }
        const int tiles_played = move_get_tiles_played(m);
        for (int t = 0; t < tiles_played; t++) {
          MachineLetter ml = move_get_tile(m, t);
          if (ml == PLAYED_THROUGH_MARKER) {
            continue;
          }
          // Treat blanks: stored as letter|BLANK_MASK, blank in rack.
          if (get_is_blanked(ml)) {
            ml = BLANK_MACHINE_LETTER;
          }
          rack_take_letter(&leave, ml);
        }
        StringBuilder *sb = string_builder_create();
        string_builder_add_move(sb, game_get_board(game), m, ld, true);
        StringBuilder *lsb = string_builder_create();
        string_builder_add_rack(lsb, &leave, ld, false);
        (void)fprintf(
            stderr, "  #%-3d  score=%-3d  leave=%-8s  equity=%+9.3f  %s\n",
            printed, equity_to_int(move_get_score(m)), string_builder_peek(lsb),
            (double)move_get_equity(m) / 1000.0, string_builder_peek(sb));
        string_builder_destroy(sb);
        string_builder_destroy(lsb);
      }
      free(eq_order);
      (void)fflush(stderr);
    }
    (void)fflush(stderr);

    // Pre-build a pruned KWG for the current board state and install it
    // as an override on the base game. game_duplicate (used inside
    // emit_split via peg_make_post_cand_game) carries the override
    // pointer through to every scenario's branch, so endgame_solve can
    // run with skip_word_pruning=true and reuse this single pruned KWG
    // instead of rebuilding one per (cand, scenario). The rebuild path
    // shares state that races under concurrent workers; pre-building
    // once per position eliminates that race entirely. The pruned KWG
    // for the pre-cand board is a superset of the words playable in any
    // post-cand position (the post-cand board has strictly more tiles
    // and strictly fewer possible words), so this is a correctness-
    // preserving optimization.
    KWG *peg_pruned_kwg = NULL;
    if (depth >= 1) {
      DictionaryWordList *word_list = dictionary_word_list_create();
      const KWG *full_kwg = player_get_kwg(game_get_player(game, mover_idx));
      generate_possible_words(game, full_kwg, word_list);
      peg_pruned_kwg = make_kwg_from_words_small(
          word_list, KWG_MAKER_OUTPUT_GADDAG, KWG_MAKER_MERGE_EXACT);
      dictionary_word_list_destroy(word_list);
      game_set_override_kwgs(game, peg_pruned_kwg, NULL,
                             DUAL_LEXICON_MODE_IGNORANT);
      game_gen_all_cross_sets(game);
    }

    PegCandResult *results =
        calloc_or_die((size_t)(n_all > 0 ? n_all : 1), sizeof(PegCandResult));
    int n_results = 0;

    Timer t;
    ctimer_start(&t);

    // ml_cands is heap-ordered (MOVE_RECORD_ALL), not equity-sorted. Build
    // an equity-sorted index. If PASSPEG_CAND_TOP_K is set, only evaluate
    // the top-K placements by static equity — useful as a lossy speedup
    // for inner PEG runs where opp's best move is almost always near the
    // top by equity.
    const char *_inc_pass_env = getenv("PASSPEG_INCLUDE_PASS");
    const bool _inc_pass_here =
        _inc_pass_env && passpeg_str_to_int(_inc_pass_env) > 0;
    int *cand_order = malloc_or_die((size_t)n_all * sizeof(int));
    int cand_order_n = 0;
    for (int ci = 0; ci < n_all; ci++) {
      const Move *m = move_list_get_move(ml_cands, ci);
      const game_event_t mt = move_get_type(m);
      if (mt == GAME_EVENT_TILE_PLACEMENT_MOVE ||
          (_inc_pass_here && mt == GAME_EVENT_PASS)) {
        cand_order[cand_order_n++] = ci;
      }
    }
    const char *cand_topk_env = getenv("PASSPEG_CAND_TOP_K");
    const int cand_topk_limit =
        cand_topk_env && *cand_topk_env ? passpeg_str_to_int(cand_topk_env) : 0;
    const int cand_sort_top =
        (cand_topk_limit > 0 && cand_topk_limit < cand_order_n)
            ? cand_topk_limit
            : cand_order_n;
    // Ordering:
    //  - With PASSPEG_GREEDY_ONLY set, the filter is a semicolon-separated
    //    list of move-text substrings IN PREVIOUS-STAGE RANK ORDER. Honor
    //    that order so partial-stage runs (budget hit mid-stage) evaluate
    //    the previous stage's best-known cands first.
    //  - Otherwise, fall back to static-equity descending.
    if (only_moves) {
      // Build a parallel array of cand text strings so we can substring-
      // match each filter token to find which ml_cand index it refers to.
      char (*cand_text)[256] = (char (*)[256])malloc_or_die(
          (size_t)cand_order_n * sizeof(char[256]));
      for (int i = 0; i < cand_order_n; i++) {
        const Move *m = move_list_get_move(ml_cands, cand_order[i]);
        StringBuilder *sb_m = string_builder_create();
        string_builder_add_move(sb_m, game_get_board(game), m, ld, true);
        (void)snprintf(cand_text[i], 256, "%s", string_builder_peek(sb_m));
        string_builder_destroy(sb_m);
      }
      int *new_order = malloc_or_die((size_t)cand_order_n * sizeof(int));
      int new_order_n = 0;
      bool *consumed =
          (bool *)malloc_or_die((size_t)cand_order_n * sizeof(bool));
      memset(consumed, 0, (size_t)cand_order_n * sizeof(bool));
      char tmp_filter[4096];
      (void)snprintf(tmp_filter, sizeof(tmp_filter), "%s", only_moves);
      const char *tok = strtok(tmp_filter, ";");
      while (tok) {
        // Skip leading spaces in the token.
        while (*tok == ' ') {
          tok++;
        }
        for (int i = 0; i < cand_order_n; i++) {
          if (!consumed[i] && strstr(cand_text[i], tok)) {
            new_order[new_order_n++] = cand_order[i];
            consumed[i] = true;
            break;
          }
        }
        tok = strtok(NULL, ";");
      }
      // Append any unmatched cands at the end so they still get evaluated
      // if budget allows (preserves the prior behaviour for cands the
      // filter never named; mostly relevant outside the wrapper path).
      for (int i = 0; i < cand_order_n; i++) {
        if (!consumed[i]) {
          new_order[new_order_n++] = cand_order[i];
        }
      }
      // Replace cand_order with the filter-driven ordering.
      memcpy(cand_order, new_order, (size_t)new_order_n * sizeof(int));
      cand_order_n = new_order_n;
      free(new_order);
      free(consumed);
      free(cand_text);
    } else {
      // Static-equity descending (selection sort over the top-K).
      for (int i = 0; i < cand_sort_top; i++) {
        int best = i;
        Equity best_eq =
            move_get_equity(move_list_get_move(ml_cands, cand_order[i]));
        for (int j = i + 1; j < cand_order_n; j++) {
          const Equity je =
              move_get_equity(move_list_get_move(ml_cands, cand_order[j]));
          if (je > best_eq) {
            best = j;
            best_eq = je;
          }
        }
        if (best != i) {
          const int tmp = cand_order[i];
          cand_order[i] = cand_order[best];
          cand_order[best] = tmp;
        }
      }
    }
    const int cand_iter_n = cand_sort_top;

    const char *include_pass_env = getenv("PASSPEG_INCLUDE_PASS");
    const bool include_pass =
        include_pass_env && passpeg_str_to_int(include_pass_env) > 0;

    // Pooled-dispatch storage: keep each cand's EnumCtx + cand_txt alive
    // until ALL cands' scenarios have run, so workers can pull jobs across
    // cand boundaries instead of stalling at every per-cand barrier.
    PegEnumCtx *enum_ctxs = NULL;
    char (*cand_txts)[256] = NULL;
    PegScenarioJobList all_jobs = {0};
    if (executor != NULL) {
      enum_ctxs = malloc_or_die((size_t)cand_iter_n * sizeof(PegEnumCtx));
      cand_txts = malloc_or_die((size_t)cand_iter_n * sizeof(char[256]));
    }
    int cand_used = 0;

    // PASSPEG_GREEDY_BUDGET=T (seconds, may be fractional): time budget
    // for this stage. Checked at the START of each cand iteration AND
    // inside the scenario worker. When exceeded, the cand loop breaks and
    // we dispatch only the cands enumerated so far. Workers that pick up
    // jobs after the deadline return early (no-op). Result is the partial
    // ranking from completed cands+scenarios.
    const char *budget_env = getenv("PASSPEG_GREEDY_BUDGET");
    const double budget_secs =
        budget_env && *budget_env ? passpeg_str_to_double(budget_env) : 0.0;
    // Absolute monotonic-ns deadline shared with endgame_solve_inline so
    // mid-search abdada_negamax can bail rather than running to depth
    // completion. 0 = no deadline.
    const int64_t budget_deadline_ns =
        budget_secs > 0.0
            ? ctimer_monotonic_ns() + (int64_t)(budget_secs * 1.0e9)
            : 0;

    for (int ord_i = 0; ord_i < cand_iter_n; ord_i++) {
      if (budget_secs > 0.0 && ctimer_elapsed_seconds(&t) > budget_secs) {
        (void)fprintf(
            stderr,
            "[peggreedy] budget %.3fs hit at cand %d/%d, stopping enum\n",
            budget_secs, ord_i, cand_iter_n);
        break;
      }
      const int ci = cand_order[ord_i];
      const Move *cand = move_list_get_move(ml_cands, ci);
      const bool is_pass = (move_get_type(cand) == GAME_EVENT_PASS);
      if ((is_pass && !include_pass) ||
          (!is_pass && move_get_tiles_played(cand) < 1)) {
        continue;
      }
      // only_moves filter: build the cand text once, into a stack scratch
      // for the serial path or into the persistent cand_txts slot for the
      // pooled path so workers can read it after enumeration returns.
      char serial_cand_txt[256] = {0};
      char *cand_txt =
          (executor != NULL) ? cand_txts[cand_used] : serial_cand_txt;
      if (executor != NULL) {
        cand_txt[0] = '\0';
      }
      {
        StringBuilder *sb_m = string_builder_create();
        string_builder_add_move(sb_m, game_get_board(game), cand, ld, true);
        (void)snprintf(cand_txt, 256, "%s", string_builder_peek(sb_m));
        string_builder_destroy(sb_m);
      }
      if (only_moves) {
        bool match = false;
        char tmp[2048];
        (void)snprintf(tmp, sizeof(tmp), "%s", only_moves);
        const char *tok = strtok(tmp, ";");
        while (tok) {
          if (strstr(cand_txt, tok)) {
            match = true;
            break;
          }
          tok = strtok(NULL, ";");
        }
        if (!match) {
          continue;
        }
      }

      int K = move_get_tiles_played(cand);
      int k_drawn = K < N ? K : N;
      int n_bag_remaining = N - k_drawn;
      int cand_score = equity_to_int(move_get_score(cand));

      PegCandResult *res = &results[n_results++];
      res->ci = ci;

      if (executor == NULL) {
        // Serial path: enumerate + evaluate in place with stack scratch.
        int n_multiset_buf[MAX_ALPHABET_SIZE] = {0};
        int mover_pick_buf[MAX_ALPHABET_SIZE] = {0};
        PegEnumCtx enum_ctx = {
            .n_multiset = n_multiset_buf,
            .mover_pick = mover_pick_buf,
            .types = types,
            .type_counts = counts,
            .k_types = k_types,
            .k_drawn = k_drawn,
            .n_bag_remaining = n_bag_remaining,
            .base_game = game,
            .mover_idx = mover_idx,
            .unseen = unseen,
            .ld_size = lds,
            .cand = cand,
            .cand_txt = cand_txt,
            .cand_score = cand_score,
            .pos_idx = pi,
            .ld = ld,
            .tsv_f = tsv_f,
            .res = res,
            .scenario_filter = scenario_filter,
            .opp_move_filter = opp_move_filter,
            .depth = depth,
            .opp_top_k = opp_top_k,
            .thread_control = config_get_thread_control(config),
            .worker_idx = 0,
            .executor = NULL,
            .res_mutex = NULL,
            .tsv_mutex = NULL,
            .endgame_mutex = NULL,
            .out_jobs = NULL,
            .budget_timer = budget_secs > 0.0 ? &t : NULL,
            .budget_secs = budget_secs,
            .deadline_monotonic_ns = budget_deadline_ns,
            .inner_tsv_f = inner_tsv_f,
            .inner_tsv_mutex = inner_tsv_f ? &inner_tsv_mutex : NULL,
            .opp_pov_cache = NULL, // serial path: not yet wired to cache
            .shared_eg_tt = NULL,  // serial path: not wired
        };
        peg_enum_outer_multiset(&enum_ctx, 0, N);
      } else {
        // Pooled path: enumerate scenarios into the GLOBAL job list. The
        // EnumCtx itself lives in enum_ctxs[cand_used] so workers reading
        // job->base_ctx still see valid memory after enumeration returns.
        // The enumerator's n_multiset/mover_pick are scratch used only
        // during enumeration; each pushed job owns its own copies.
        int n_multiset_scratch[MAX_ALPHABET_SIZE] = {0};
        int mover_pick_scratch[MAX_ALPHABET_SIZE] = {0};
        // Per-cand opp_pov_game cache (task #35). Allocated here, freed after
        // this cand's scenarios complete. Disable via PASSPEG_OPP_POV_CACHE=0.
        PegOppPovCache *cand_opp_pov_cache = NULL;
        const char *opp_pov_cache_env = getenv("PASSPEG_OPP_POV_CACHE");
        const bool opp_pov_cache_on =
            !opp_pov_cache_env ||
            passpeg_str_to_int(opp_pov_cache_env) > 0; // default on
        if (opp_pov_cache_on) {
          cand_opp_pov_cache = peg_opp_pov_cache_create(16384);
        }
        // Per-cand shared endgame transposition table. This is the real
        // macondo-equivalent shared cache: endgame_solve uses it for
        // sub-tree position lookups, and any two endgame_solve calls in
        // this cand's opp-POV states/scenarios that reach overlapping sub-trees
        // share the work. Memory: 1% of system RAM per cand (allocated
        // and freed per-cand). Disable via PASSPEG_SHARED_EG_TT=0.
        TranspositionTable *cand_shared_eg_tt = NULL;
        const char *shared_tt_env = getenv("PASSPEG_SHARED_EG_TT");
        const bool shared_tt_on =
            !shared_tt_env || passpeg_str_to_int(shared_tt_env) > 0;
        if (shared_tt_on) {
          const char *shared_tt_frac_env = getenv("PASSPEG_SHARED_EG_TT_FRAC");
          const double shared_tt_frac =
              (shared_tt_frac_env && *shared_tt_frac_env)
                  ? passpeg_str_to_double(shared_tt_frac_env)
                  : 0.01; // 1% of memory by default
          cand_shared_eg_tt = transposition_table_create(shared_tt_frac);
        }
        // Per-worker persistent EndgameCtx slots. Slot 0 = main thread;
        // slots 1..n = executor workers (worker_idx 100..100+n-1).
        // Default on; disable via PASSPEG_PERSIST_EG_CTX=0.
        EndgameCtx **per_worker_eg_ctx = NULL;
        const int per_worker_eg_ctx_n = n_threads + 1;
        const char *persist_env = getenv("PASSPEG_PERSIST_EG_CTX");
        const bool persist_on =
            !persist_env || passpeg_str_to_int(persist_env) > 0;
        if (persist_on) {
          per_worker_eg_ctx =
              calloc_or_die((size_t)per_worker_eg_ctx_n, sizeof(EndgameCtx *));
        }
        enum_ctxs[cand_used] = (PegEnumCtx){
            .n_multiset = n_multiset_scratch,
            .mover_pick = mover_pick_scratch,
            .types = types,
            .type_counts = counts,
            .k_types = k_types,
            .k_drawn = k_drawn,
            .n_bag_remaining = n_bag_remaining,
            .base_game = game,
            .mover_idx = mover_idx,
            .unseen = unseen,
            .ld_size = lds,
            .cand = cand,
            .cand_txt = cand_txt,
            .cand_score = cand_score,
            .pos_idx = pi,
            .ld = ld,
            .tsv_f = tsv_f,
            .res = res,
            .scenario_filter = scenario_filter,
            .opp_move_filter = opp_move_filter,
            .depth = depth,
            .opp_top_k = opp_top_k,
            .thread_control = config_get_thread_control(config),
            .worker_idx = 0,
            .executor = executor,
            .res_mutex = &res_mutex,
            .tsv_mutex = &tsv_mutex,
            .endgame_mutex = &endgame_mutex,
            .out_jobs = &all_jobs,
            .budget_timer = budget_secs > 0.0 ? &t : NULL,
            .budget_secs = budget_secs,
            .deadline_monotonic_ns = budget_deadline_ns,
            .inner_tsv_f = inner_tsv_f,
            .inner_tsv_mutex = inner_tsv_f ? &inner_tsv_mutex : NULL,
            .opp_pov_cache = cand_opp_pov_cache,
            .shared_eg_tt = cand_shared_eg_tt,
            .per_worker_eg_ctx = per_worker_eg_ctx,
            .per_worker_eg_ctx_n = per_worker_eg_ctx_n,
            .per_worker_eg_ctx_offset = 100, // executor's thread_index_offset
        };
        // Per-cand batch dispatch. We enumerate THIS cand's scenarios
        // into the shared `all_jobs` list, then immediately submit just
        // those jobs and wait for them all to finish before moving to
        // the next cand. That gives strict best-first cand order: cand
        // 0's evaluation fully completes (or its opp-POV states run out the
        // global deadline and the cand is marked incomplete) before
        // cand 1's jobs are dispatched. Worker-level scheduling can
        // still interleave scenarios within one cand, which is fine.
        const int cand_jobs_start = all_jobs.n;
        peg_enum_outer_multiset(&enum_ctxs[cand_used], 0, N);
        const int cand_jobs_end = all_jobs.n;
        // After enumeration, the scratch buffers go out of scope; clear
        // the dangling pointers so a buggy late read trips an obvious
        // null-deref instead of reading stack garbage.
        enum_ctxs[cand_used].n_multiset = NULL;
        enum_ctxs[cand_used].mover_pick = NULL;
        enum_ctxs[cand_used].out_jobs = NULL;
        // cppcheck-suppress knownConditionTrueFalse ; peg_enum_outer_multiset
        // pushes jobs into all_jobs (raising .n) via the out_jobs pointer.
        if (cand_jobs_end > cand_jobs_start) {
          const int n_this_cand = cand_jobs_end - cand_jobs_start;
          void **args = malloc_or_die((size_t)n_this_cand * sizeof(void *));
          for (int j = 0; j < n_this_cand; j++) {
            args[j] = &all_jobs.items[cand_jobs_start + j];
          }
          peg_pool_submit_and_wait(executor, peg_scenario_worker_fn, args,
                                   n_this_cand, 0);
          free(args);
        }
        // Cand's scenarios are done; free the cache and (optionally) log
        // hit/miss stats. Clear from the ctx so any late access fails fast.
        if (cand_opp_pov_cache) {
          const int hits = atomic_load(&cand_opp_pov_cache->hits);
          const int misses = atomic_load(&cand_opp_pov_cache->misses);
          const int total = hits + misses;
          if (total > 0 && getenv("PASSPEG_OPP_POV_CACHE_STATS")) {
            (void)fprintf(stderr,
                          "[opp_pov_cache] %s: %d hits / %d total (%.1f%%), "
                          "%d misses\n",
                          cand_txt, hits, total, hits * 100.0 / total, misses);
          }
          enum_ctxs[cand_used].opp_pov_cache = NULL;
          peg_opp_pov_cache_destroy(cand_opp_pov_cache);
        }
        if (cand_shared_eg_tt) {
          if (getenv("PASSPEG_OPP_POV_CACHE_STATS")) {
            const long long tt_created =
                atomic_load(&cand_shared_eg_tt->created);
            const long long tt_lookups =
                atomic_load(&cand_shared_eg_tt->lookups);
            const long long tt_hits = atomic_load(&cand_shared_eg_tt->hits);
            (void)fprintf(
                stderr,
                "[shared_eg_tt] %s: created=%lld lookups=%lld hits=%lld "
                "(%.1f%% hit rate)\n",
                cand_txt, tt_created, tt_lookups, tt_hits,
                tt_lookups > 0 ? (double)tt_hits * 100.0 / (double)tt_lookups
                               : 0.0);
          }
          enum_ctxs[cand_used].shared_eg_tt = NULL;
          transposition_table_destroy(cand_shared_eg_tt);
        }
        // Destroy per-worker persistent EndgameCtx slots. All this
        // cand's scenarios have completed, so no worker is using one.
        if (per_worker_eg_ctx) {
          int slots_used = 0;
          for (int slot_idx = 0; slot_idx < per_worker_eg_ctx_n; slot_idx++) {
            if (per_worker_eg_ctx[slot_idx]) {
              endgame_ctx_destroy(per_worker_eg_ctx[slot_idx]);
              slots_used++;
            }
          }
          if (getenv("PASSPEG_OPP_POV_CACHE_STATS")) {
            (void)fprintf(stderr,
                          "[persist_eg_ctx] %s: reused across %d worker "
                          "slot(s)\n",
                          cand_txt, slots_used);
          }
          enum_ctxs[cand_used].per_worker_eg_ctx = NULL;
          free(per_worker_eg_ctx);
        }
        cand_used++;
      }
    }

    if (executor != NULL) {
      for (int j = 0; j < all_jobs.n; j++) {
        free(all_jobs.items[j].n_multiset);
        free(all_jobs.items[j].mover_pick);
      }
      free(all_jobs.items);
      free(enum_ctxs);
      free(cand_txts);
    }
    // Capture the top-priority candidate index before freeing cand_order: the
    // static fallback below (when n_ranked == 0) needs it after this free.
    const int fallback_ci = cand_order_n > 0 ? cand_order[0] : -1;
    free(cand_order);

    double wall = ctimer_elapsed_seconds(&t);

    // Sort results by u = q_win + 1e-4 × q_spread.
    typedef struct {
      int ci;
      double q_win, q_spread, u;
      int64_t weight_sum;
      int n_scen;
    } Ranked;
    Ranked *ranked =
        calloc_or_die((size_t)(n_results > 0 ? n_results : 1), sizeof(Ranked));
    int n_ranked = 0;
    int n_dropped_incomplete = 0;
    for (int r = 0; r < n_results; r++) {
      if (results[r].weight_sum <= 0) {
        continue;
      }
      if (results[r].incomplete) {
        // At least one inner endgame_solve returned depth=-1 (global
        // deadline hit before any IDS iteration). Aggregated win/spread
        // is contaminated by mover_lead defaults for those opp-POV states. Drop
        // the cand from the ranking rather than emit a misleading score.
        n_dropped_incomplete++;
        continue;
      }
      double q_win =
          (double)results[r].win_x2 / (2.0 * (double)results[r].weight_sum);
      double q_spread =
          (double)results[r].spread_sum / (double)results[r].weight_sum;
      ranked[n_ranked++] = (Ranked){results[r].ci,
                                    q_win,
                                    q_spread,
                                    q_win + 1e-4 * q_spread,
                                    results[r].weight_sum,
                                    results[r].n_scen};
    }
    for (int i = 0; i < n_ranked; i++) {
      for (int j = i + 1; j < n_ranked; j++) {
        if (ranked[j].u > ranked[i].u) {
          Ranked tmp = ranked[i];
          ranked[i] = ranked[j];
          ranked[j] = tmp;
        }
      }
    }
    int show = top_k < n_ranked ? top_k : n_ranked;
    const double budget_used = (budget_secs > 0.0) ? budget_secs : -1.0;
    (void)fprintf(stderr,
                  "[peggreedy] pos %d wall=%.3fs  budget=%.3fs  ranked=%d  "
                  "dropped_incomplete=%d  top-%d cands:\n",
                  pi, wall, budget_used, n_ranked, n_dropped_incomplete, show);
    for (int r = 0; r < show; r++) {
      const Move *m = move_list_get_move(ml_cands, ranked[r].ci);
      StringBuilder *sb = string_builder_create();
      string_builder_add_move(sb, game_get_board(game), m, ld, true);
      (void)fprintf(
          stderr,
          "  #%-2d  win=%.4f  spread=%+9.3f  scen=%d  weight=%lld  %s\n", r + 1,
          ranked[r].q_win, ranked[r].q_spread, ranked[r].n_scen,
          (long long)ranked[r].weight_sum, string_builder_peek(sb));
      string_builder_destroy(sb);
    }
    // Machine-readable rank emit for the cascade driver. Always write
    // ALL n_ranked cands (not just top show); the driver uses the full
    // list to apply bucket quotas at the next stage.
    if (result_f) {
      for (int r = 0; r < n_ranked; r++) {
        const Move *m = move_list_get_move(ml_cands, ranked[r].ci);
        StringBuilder *sb = string_builder_create();
        string_builder_add_move(sb, game_get_board(game), m, ld, true);
        const char *cand_txt = string_builder_peek(sb);
        const int tp = peg_count_tiles_played(cand_txt);
        const int bucket = peg_cand_bucket(tp, N, RACK_SIZE);
        (void)fprintf(result_f, "%d\t%d\t%.6f\t%.4f\t%d\t%lld\t%d\t%d\t%s\n",
                      pi, r + 1, ranked[r].q_win, ranked[r].q_spread,
                      ranked[r].n_scen, (long long)ranked[r].weight_sum, tp,
                      bucket, cand_txt);
        string_builder_destroy(sb);
      }
      (void)fflush(result_f);
    }
    // Static fallback: if budget expired before ANY cand completed a
    // scenario (n_ranked == 0), emit the single highest-static-equity
    // cand so downstream consumers always get a play.
    if (n_ranked == 0 && cand_iter_n > 0 && fallback_ci >= 0) {
      const Move *fallback_m = move_list_get_move(ml_cands, fallback_ci);
      StringBuilder *sb = string_builder_create();
      string_builder_add_move(sb, game_get_board(game), fallback_m, ld, true);
      (void)fprintf(stderr, "  #1   [fallback=static]  scen=0  weight=0  %s\n",
                    string_builder_peek(sb));
      string_builder_destroy(sb);
    }
    (void)fflush(stderr);

    free(ranked);
    free(results);
    move_list_destroy(ml_cands);
    if (peg_pruned_kwg) {
      game_set_override_kwgs(game, NULL, NULL, DUAL_LEXICON_MODE_IGNORANT);
      kwg_destroy(peg_pruned_kwg);
    }
    config_destroy(config);
  }

  if (tsv_f) {
    (void)fclose(tsv_f);
    (void)fprintf(stderr, "[peggreedy] TSV written to %s\n", tsv_path);
  }
  if (inner_tsv_f) {
    (void)fclose(inner_tsv_f);
    (void)fprintf(stderr, "[peggreedy] INNER TSV written to %s\n",
                  inner_tsv_path);
  }
  if (result_f) {
    (void)fclose(result_f);
    (void)fprintf(stderr, "[peggreedy] RESULT TSV written to %s\n",
                  result_path);
  }
  if (executor) {
    peg_pool_destroy(executor);
  }
  for (int i = 0; i < n_pos; i++) {
    free(cgps[i]);
  }
  free(cgps);
}
