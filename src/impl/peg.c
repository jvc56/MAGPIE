#include "peg.h"

#include "../compat/cpthread.h"
#include "../compat/ctime.h"
#include "../def/board_defs.h"
#include "../def/cpthread_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/kwg_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/peg_defs.h"
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
#include "../util/io_util.h"
#include "endgame.h"
#include "gameplay.h"
#include "kwg_maker.h"
#include "move_gen.h"
#include "peg_pool.h"
#include "word_prune.h"
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Default halving schedule for the cascade's stages 1..N. Stage 0 is the
// greedy seed (top-K = all); each halving stage narrows the surviving set
// while adding a ply of fidelity. The tail is top-2, never top-1 — a stage
// re-ranks a set, so its output needs >= 2 candidates to compare. The length
// of this table is the default number of halving stages; there is no fixed
// cap, so a caller may pass a longer schedule via PegArgs.stage_top_k.
static const int PEG_DEFAULT_HALVING_COUNTS[] = {32, 16, 8, 4, 2};

enum {
  // Candidate move-list capacity for the root generate_moves.
  PEG_CAND_LIST_CAP = 16384,
  // Greedy playout depth ceiling (a PEG playout terminates well before this).
  PEG_PLAYOUT_MAX_PLIES = 40,
  // Fixed endgame seed for deterministic leaf solves.
  PEG_ENDGAME_SEED = 1,
  // Pessimistic playout: capacity of the opponent reply list. The adversarial
  // 1-ply lookahead tries every generated reply (a late-game PEG position has
  // far fewer than this), so the opponent's true worst-for-mover reply — which
  // includes the rational best-equity one — is always considered.
  PEG_PESSIMISTIC_OPP_LIST_CAP = 1024,
};

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
  // Shared endgame TT, reused across every leaf solve this worker runs. Many
  // scenarios reach identical board states, so cross-scenario reuse is the
  // dominant endgame speedup.
  TranspositionTable *eg_tt;
} PegWorker;

// ----- live poll -----------------------------------------------------------
// Mutex-guarded leaderboard the solver refreshes while running; a separate
// thread reads consistent snapshots via peg_poll_read. Caller-owned (see
// peg.h). All update helpers are no-ops when poll == NULL.
struct PegPoll {
  cpthread_mutex_t mutex;
  PegPollSnapshot s;
};

PegPoll *peg_poll_create(void) {
  PegPoll *poll = malloc_or_die(sizeof(*poll));
  cpthread_mutex_init(&poll->mutex);
  memset(&poll->s, 0, sizeof(poll->s));
  poll->s.stage = -1;
  return poll;
}

void peg_poll_destroy(PegPoll *poll) {
  // No cpthread_mutex_destroy: project mutexes don't dynamically allocate.
  free(poll);
}

void peg_poll_read(PegPoll *poll, PegPollSnapshot *out) {
  cpthread_mutex_lock(&poll->mutex);
  *out = poll->s;
  cpthread_mutex_unlock(&poll->mutex);
}

static inline double peg_poll_key(const PegRankedCand *cand) {
  return cand->win_pct + 1e-4 * cand->mean_spread;
}

// Stage-boundary metadata, set before a stage's evaluation begins.
static void peg_poll_begin_stage(PegPoll *poll, int stage, int fidelity_plies,
                                 int field_size) {
  if (poll == NULL) {
    return;
  }
  cpthread_mutex_lock(&poll->mutex);
  poll->s.stage = stage;
  poll->s.fidelity_plies = fidelity_plies;
  poll->s.field_size = field_size;
  poll->s.version++;
  cpthread_mutex_unlock(&poll->mutex);
}

// Insert one finished candidate into the descending top-K (stage-0 liveness).
// Called from worker threads, so it locks.
static void peg_poll_upsert(PegPoll *poll, const PegRankedCand *cand) {
  if (poll == NULL) {
    return;
  }
  cpthread_mutex_lock(&poll->mutex);
  PegPollSnapshot *snap = &poll->s;
  const double key = peg_poll_key(cand);
  int i;
  if (snap->n_entries < PEG_POLL_MAX_ENTRIES) {
    i = snap->n_entries++;
  } else if (key > peg_poll_key(&snap->entries[PEG_POLL_MAX_ENTRIES - 1])) {
    i = PEG_POLL_MAX_ENTRIES - 1;
  } else {
    cpthread_mutex_unlock(&poll->mutex);
    return; // full and no better than the worst kept entry
  }
  while (i > 0 && peg_poll_key(&snap->entries[i - 1]) < key) {
    snap->entries[i] = snap->entries[i - 1];
    i--;
  }
  snap->entries[i] = *cand;
  snap->version++;
  cpthread_mutex_unlock(&poll->mutex);
}

// Replace the leaderboard with an authoritative ranking at a stage boundary.
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
  poll->s.version++;
  cpthread_mutex_unlock(&poll->mutex);
}

static void peg_poll_finish(PegPoll *poll) {
  if (poll == NULL) {
    return;
  }
  cpthread_mutex_lock(&poll->mutex);
  poll->s.done = true;
  poll->s.version++;
  cpthread_mutex_unlock(&poll->mutex);
}

// ----- combinatorics -------------------------------------------------------

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
  int64_t result = 1;
  for (int i = 0; i < k; i++) {
    result = result * (n - i) / (i + 1);
  }
  return result;
}

// In-place lexicographic next-permutation; only enumerates distinct orderings
// (skips duplicates). Caller sorts ascending before the first call. Returns
// false at the last permutation.
static bool peg_next_perm(MachineLetter *arr, int n) {
  if (n <= 1) {
    return false;
  }
  int pivot = n - 2;
  while (pivot >= 0 && arr[pivot] >= arr[pivot + 1]) {
    pivot--;
  }
  if (pivot < 0) {
    return false;
  }
  int swap_idx = n - 1;
  while (arr[pivot] >= arr[swap_idx]) {
    swap_idx--;
  }
  MachineLetter tmp = arr[pivot];
  arr[pivot] = arr[swap_idx];
  arr[swap_idx] = tmp;
  int lo = pivot + 1;
  int hi = n - 1;
  while (lo < hi) {
    tmp = arr[lo];
    arr[lo] = arr[hi];
    arr[hi] = tmp;
    lo++;
    hi--;
  }
  return true;
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
static void peg_build_template(PegWorker *worker, const Game *prepared_base,
                               const Move *cand) {
  if (worker->template_game == NULL) {
    worker->template_game = game_duplicate(prepared_base);
  } else {
    game_copy(worker->template_game, prepared_base);
  }
  Game *template_game = worker->template_game;
  play_move_without_drawing_tiles(cand, template_game);
  game_gen_all_cross_sets(template_game);
}

// Build the post-cand game for one (mover_drawn, bag_remaining) split by
// copying the per-cand template (which already has the cand played, cross-sets,
// and the pruned-KWG override) and resetting only the racks/bag: bag holds
// (mover_drawn
// ++ bag_remaining), opp rack is unseen minus the bag, and the mover draws
// their k_drawn tiles onto the leave. Returns the scratch game (worker-owned;
// not destroyed per call). Racks/bag don't affect cross-sets, so the copied
// ones stay valid and the leaf endgame can skip regenerating them.
static Game *peg_make_post_cand_game(PegWorker *worker,
                                     const Game *template_src, int mover_idx,
                                     const uint8_t *unseen, int ld_size,
                                     int k_drawn,
                                     const MachineLetter *mover_drawn,
                                     int n_bag_remaining,
                                     const MachineLetter *bag_remaining) {
  if (worker->scratch_game == NULL) {
    worker->scratch_game = game_duplicate(template_src);
  } else {
    game_copy(worker->scratch_game, template_src);
  }
  Game *game = worker->scratch_game;
  Bag *bag = game_get_bag(game);
  Rack *opp_rack = player_get_rack(game_get_player(game, 1 - mover_idx));
  Rack *mover_rack = player_get_rack(game_get_player(game, mover_idx));

  MachineLetter all_bag[PEG_MAX_BAG + 1];
  const int n_bag = k_drawn + n_bag_remaining;
  for (int i = 0; i < k_drawn; i++) {
    all_bag[i] = mover_drawn[i];
  }
  for (int i = 0; i < n_bag_remaining; i++) {
    all_bag[k_drawn + i] = bag_remaining[i];
  }
  bag_set_to_tiles(bag, all_bag, n_bag);
  peg_set_opp_rack(opp_rack, unseen, ld_size, all_bag, n_bag);
  // The template's mover rack holds the leave; add this scenario's draws.
  for (int i = 0; i < k_drawn; i++) {
    rack_add_letter(mover_rack, mover_drawn[i]);
    (void)bag_draw_letter(bag, mover_drawn[i], 0);
  }
  // Playing the cand in the template may have flagged GAME_END_REASON_STANDARD
  // (rack emptied in the no-draw world). We re-stock here, so clear the stale
  // flag unless the rack is genuinely empty.
  if (!rack_is_empty(mover_rack)) {
    game_set_game_end_reason(game, GAME_END_REASON_NONE);
  }
  return game;
}

// Greedy playout to game end; returns signed mover spread (points), with the
// usual rack-leave adjustment when the game has not actually ended.
static int32_t peg_greedy_playout(Game *game, int mover_idx,
                                  MoveList *playout_ml) {
  const LetterDistribution *ld = game_get_ld(game);
  for (int ply = 0; ply < PEG_PLAYOUT_MAX_PLIES; ply++) {
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      break;
    }
    const bool bag_has_tiles = bag_get_letters(game_get_bag(game)) > 0;
    const MoveGenArgs args = {
        .game = game,
        .move_record_type = MOVE_RECORD_BEST,
        .move_sort_type = bag_has_tiles ? MOVE_SORT_EQUITY : MOVE_SORT_SCORE,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
        .move_list = playout_ml,
        .tiles_played_bv = NULL,
        .initial_tiles_bv = 0,
    };
    generate_moves(&args);
    if (move_list_get_count(playout_ml) == 0) {
      break;
    }
    play_move(move_list_get_move(playout_ml, 0), game, NULL);
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

// Pessimistic playout: the mover plays greedily, but at each opponent turn the
// opponent plays the reply that minimizes the mover's final spread — a 1-ply
// adversarial choice scored by a greedy rollout of the remainder. This is the
// PEG_OPP_PESSIMISTIC leaf for non-emptier scenarios; its mover spread is
// always
// <= the greedy (rational) playout's. Returns the signed mover spread with the
// same rack-leave adjustment as peg_greedy_playout.
static int32_t peg_pessimistic_playout(Game *game, int mover_idx,
                                       MoveList *playout_ml) {
  const LetterDistribution *ld = game_get_ld(game);
  Game *branch = NULL;         // lazily created scratch for opp-reply trials
  MoveList *opp_ml = NULL;     // opponent's candidate replies
  MoveList *rollout_ml = NULL; // greedy rollout of each trial
  for (int ply = 0; ply < PEG_PLAYOUT_MAX_PLIES; ply++) {
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
          .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
          .move_list = playout_ml,
          .tiles_played_bv = NULL,
          .initial_tiles_bv = 0,
      };
      generate_moves(&ga);
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
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
        .move_list = opp_ml,
        .tiles_played_bv = NULL,
        .initial_tiles_bv = 0,
    };
    generate_moves(&ga);
    const int n_opp = move_list_get_count(opp_ml);
    if (n_opp == 0) {
      break;
    }
    int worst_idx = 0;
    int32_t worst_for_mover = INT32_MAX;
    for (int i = 0; i < n_opp; i++) {
      game_copy(branch, game);
      play_move(move_list_get_move(opp_ml, i), branch, NULL);
      const int32_t mt = peg_greedy_playout(branch, mover_idx, rollout_ml);
      if (mt < worst_for_mover) {
        worst_for_mover = mt;
        worst_idx = i;
      }
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

typedef struct PegScenarioJobList PegScenarioJobList;

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
  // Collect mode: when out_jobs is non-NULL, peg_eval_split pushes one scenario
  // job per split (for scenario-level parallelism) instead of evaluating it
  // inline. cand_idx tags the pushed jobs; workers is the shared scratch array.
  PegScenarioJobList *out_jobs;
  int cand_idx;
  PegWorker *workers;
  // accumulators
  double total_weight;
  double win_weight; // wins + 0.5 * draws, weighted
  double spread_weight;
  int64_t weight_sum;
  int n_scenarios;
} PegEvalCtx;

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
  MachineLetter mover_drawn[PEG_MAX_BAG + 1];
  MachineLetter bag_remaining[PEG_MAX_BAG + 1];
  int64_t weight;
  PegOppModel opp_model;
  int fidelity_plies;
  int injection_cap;
  int64_t deadline_ns;
  ThreadControl *thread_control;
  PegWorker *workers;
  int cand_idx;
  // Result (filled by the worker, reduced per-cand afterwards).
  double total_weight;
  double win_weight;
  double spread_weight;
  int64_t weight_sum;
  int n_scenarios;
} PegScenarioJob;

struct PegScenarioJobList {
  PegScenarioJob *jobs;
  int count;
  int cap;
};

static void peg_scenario_joblist_push(PegScenarioJobList *list,
                                      const PegScenarioJob *job) {
  if (list->count == list->cap) {
    list->cap = list->cap ? list->cap * 2 : 256;
    list->jobs =
        realloc_or_die(list->jobs, (size_t)list->cap * sizeof(PegScenarioJob));
  }
  list->jobs[list->count++] = *job;
}

// Evaluate the leaf of one fully-resolved scenario (a specific post-cand game).
// Returns mover's signed spread (points) — exact via endgame_solve for emptier
// scenarios at fidelity > 0, else the greedy playout.
static int32_t peg_eval_leaf(PegEvalCtx *ctx, Game *game) {
  const bool emptier = bag_get_letters(game_get_bag(game)) == 0 &&
                       game_get_game_end_reason(game) == GAME_END_REASON_NONE;
  if (ctx->fidelity_plies <= 0 || !emptier) {
    // Non-emptier (or stage-0) leaf: the opponent still draws, so the model
    // matters. Emptier leaves fall through to the exact endgame below, which is
    // already adversarial-optimal and thus identical for both models.
    if (ctx->opp_model == PEG_OPP_PESSIMISTIC) {
      return peg_pessimistic_playout(game, ctx->mover_idx,
                                     ctx->worker->playout_ml);
    }
    return peg_greedy_playout(game, ctx->mover_idx, ctx->worker->playout_ml);
  }
  // Exact endgame leaf. After the mover plays and draws it is the opponent's
  // turn, so the solved value is from the on-turn player's perspective; fold
  // it into the mover lead accordingly.
  EndgameArgs ea;
  memset(&ea, 0, sizeof(ea));
  ea.thread_control = ctx->thread_control;
  ea.game = game;
  ea.plies = ctx->fidelity_plies;
  ea.initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  ea.num_threads = 1;
  // > num_threads (1) opens the injection window so the monitor can lend idle
  // cores to this (potentially long) endgame mid-solve.
  ea.max_workers = ctx->injection_cap;
  ea.use_heuristics = true;
  ea.num_top_moves = 1;
  ea.external_deadline_ns = ctx->deadline_ns;
  ea.shared_tt = ctx->worker->eg_tt;
  // The scratch game already carries the root pruned KWG (override) and valid
  // cross-sets (copied from the prepared base, incrementally updated by the
  // cand play), so the endgame must not rebuild a pruned KWG or regenerate all
  // cross-sets per solve — that regeneration was the profiled bottleneck.
  ea.skip_word_pruning = true;
  ea.seed = PEG_ENDGAME_SEED;
  endgame_results_reset(ctx->worker->eg_results);
  endgame_solve_inline(&ctx->worker->eg_ctx, &ea, ctx->worker->eg_results);
  const int eg_val =
      endgame_results_get_value(ctx->worker->eg_results, ENDGAME_RESULT_BEST);
  const Player *me = game_get_player(game, ctx->mover_idx);
  const Player *op = game_get_player(game, 1 - ctx->mover_idx);
  const int32_t mover_lead =
      equity_to_int(player_get_score(me) - player_get_score(op));
  const int turn = game_get_player_on_turn_index(game);
  return (turn == ctx->mover_idx) ? mover_lead + eg_val : mover_lead - eg_val;
}

// Evaluate one (mover_drawn, bag_remaining) split: walk the distinct orderings
// of bag_remaining (each equally likely), evaluate each leaf, and fold the
// multiset weight into the accumulator.
static void peg_eval_split(PegEvalCtx *ctx, const MachineLetter *mover_drawn,
                           int n_bag_remaining,
                           const MachineLetter *bag_remaining, int64_t weight) {
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
    job.fidelity_plies = ctx->fidelity_plies;
    job.injection_cap = ctx->injection_cap;
    job.deadline_ns = ctx->deadline_ns;
    job.thread_control = ctx->thread_control;
    job.workers = ctx->workers;
    job.cand_idx = ctx->cand_idx;
    peg_scenario_joblist_push(ctx->out_jobs, &job);
    return;
  }
  // Permute a LOCAL copy: peg_next_perm reorders the whole array in place, and
  // the caller's bag_remaining buffer is shared across the peg_enum_splits
  // recursion (ancestor letter-branches own its earlier positions). Mutating it
  // here would scramble those positions for subsequent sibling branches and
  // corrupt their multisets. Copy first so the caller's buffer is untouched.
  MachineLetter perm[PEG_MAX_BAG + 1];
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
  double ordering_win = 0.0;
  double ordering_spread = 0.0;
  int n_orderings = 0;
  do {
    Game *game = peg_make_post_cand_game(
        ctx->worker, ctx->template_src, ctx->mover_idx, ctx->unseen,
        ctx->ld_size, ctx->k_drawn, mover_drawn, n_bag_remaining, bag_perm);
    const int32_t value = peg_eval_leaf(ctx, game);
    if (value > 0) {
      ordering_win += 1.0;
    } else if (value == 0) {
      ordering_win += 0.5;
    }
    ordering_spread += (double)value;
    n_orderings++;
  } while (peg_next_perm(bag_perm, n_bag_remaining));

  // Each ordering is equally likely within this multiset, so the multiset's
  // weight is split evenly across its orderings.
  ctx->total_weight += (double)weight;
  ctx->win_weight += (double)weight * (ordering_win / n_orderings);
  ctx->spread_weight += (double)weight * (ordering_spread / n_orderings);
  ctx->weight_sum += weight;
  ctx->n_scenarios += n_orderings;
}

// Recursively choose, per machine letter, how many tiles go to the mover's
// draw (m) and to the bag remainder (b), with m+b <= unseen[ml], mover total ==
// k_drawn and bag-remainder total == n_bag_remaining. Opp gets the complement.
static void peg_enum_splits(PegEvalCtx *ctx, int ml, int mover_left,
                            int bag_rem_left, int64_t weight,
                            MachineLetter *mover_drawn, int n_mover,
                            MachineLetter *bag_remaining, int n_bag_rem) {
  if (ml == ctx->ld_size) {
    if (mover_left == 0 && bag_rem_left == 0) {
      // k_drawn! accounts for the order in which the mover draws its tiles.
      int64_t full_weight = weight;
      for (int f = 2; f <= ctx->k_drawn; f++) {
        full_weight *= f;
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
  for (int m = 0; m <= max_mover; m++) {
    const int max_bag = bag_rem_left < (avail - m) ? bag_rem_left : (avail - m);
    for (int b = 0; b <= max_bag; b++) {
      const int64_t add_weight =
          peg_binomial(avail, m) * peg_binomial(avail - m, b);
      for (int i = 0; i < m; i++) {
        mover_drawn[n_mover + i] = (MachineLetter)ml;
      }
      for (int i = 0; i < b; i++) {
        bag_remaining[n_bag_rem + i] = (MachineLetter)ml;
      }
      peg_enum_splits(ctx, ml + 1, mover_left - m, bag_rem_left - b,
                      weight * add_weight, mover_drawn, n_mover + m,
                      bag_remaining, n_bag_rem + b);
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
  int fidelity_plies;
  int scenario_stride;
  int64_t deadline_ns;
  ThreadControl *thread_control;
  PegWorker *workers; // array; indexed by worker_idx
  PegRankedCand *out;
  PegPoll *poll; // optional live leaderboard; NULL = no polling
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
  MachineLetter mover_drawn[PEG_MAX_BAG + 1] = {0};
  MachineLetter bag_remaining[PEG_MAX_BAG + 1] = {0};
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
  ctx->total_weight = 1.0;
  if (value > 0) {
    ctx->win_weight = 1.0;
  } else if (value == 0) {
    ctx->win_weight = 0.5;
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
  if (job->deadline_ns != 0 && ctimer_monotonic_ns() >= job->deadline_ns) {
    job->out->move = *job->cand;
    job->out->win_pct = -1.0;
    job->out->mean_spread = 0.0;
    job->out->weight_sum = 0;
    job->out->n_scenarios = 0;
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
  ctx.fidelity_plies = job->fidelity_plies;
  ctx.scenario_stride = job->scenario_stride;
  ctx.deadline_ns = job->deadline_ns;
  ctx.thread_control = job->thread_control;
  ctx.worker = &job->workers[worker_idx];
  // Post-cand board + cross-sets, built once for all this cand's scenarios
  // (job->base_game is the pruned prepared base).
  peg_build_template(ctx.worker, job->base_game, job->cand);
  ctx.template_src = ctx.worker->template_game;
  const int tiles_played = move_get_tiles_played(job->cand);
  ctx.k_drawn = tiles_played < job->bag_size ? tiles_played : job->bag_size;
  const int n_bag_remaining = job->bag_size - ctx.k_drawn;
  if (job->eval_bag_order_len > 0) {
    // Pinned single scenario: evaluate exactly the caller's bag ordering.
    peg_eval_fixed_ordering(&ctx, job->eval_bag_order, job->eval_bag_order_len);
  } else {
    MachineLetter mover_drawn[PEG_MAX_BAG + 1];
    MachineLetter bag_remaining[PEG_MAX_BAG + 1];
    peg_enum_splits(&ctx, /*ml=*/0, ctx.k_drawn, n_bag_remaining, /*weight=*/1,
                    mover_drawn, 0, bag_remaining, 0);
  }
  job->out->move = *job->cand;
  job->out->win_pct =
      ctx.total_weight > 0 ? ctx.win_weight / ctx.total_weight : 0.0;
  job->out->mean_spread =
      ctx.total_weight > 0 ? ctx.spread_weight / ctx.total_weight : 0.0;
  job->out->weight_sum = ctx.weight_sum;
  job->out->n_scenarios = ctx.n_scenarios;
  // Live poll: surface this finished candidate into the leaderboard so a
  // poller sees stage 0 fill in as candidates resolve.
  peg_poll_upsert(job->poll, job->out);
}

static int peg_rank_cmp(const void *lhs, const void *rhs) {
  const PegRankedCand *a = (const PegRankedCand *)lhs;
  const PegRankedCand *b = (const PegRankedCand *)rhs;
  const double a_key = a->win_pct + 1e-4 * a->mean_spread;
  const double b_key = b->win_pct + 1e-4 * b->mean_spread;
  if (a_key < b_key) {
    return 1;
  }
  if (a_key > b_key) {
    return -1;
  }
  return 0;
}

// True if `move` matches one of the protected move-similarity keys (snoprune
// analogue). Cheap linear scan: the protected set is tiny.
static bool peg_move_protected(const Move *move, const Rack *rack,
                               const uint64_t *protect_keys, int n_protect) {
  if (n_protect <= 0) {
    return false;
  }
  const uint64_t key = move_get_similarity_key(move, rack);
  for (int protect_idx = 0; protect_idx < n_protect; protect_idx++) {
    if (protect_keys[protect_idx] == key) {
      return true;
    }
  }
  return false;
}

// In-place select of the candidates that advance from a sorted ranked[0..
// live_count): the top `keep` entries plus any protected move below the cut,
// compacted into [0, return) with descending order preserved. Returns the new
// live count. With no protected stragglers this is just the plain top-`keep`.
static int peg_select_survivors(PegRankedCand *ranked, int live_count, int keep,
                                const Rack *rack, const uint64_t *protect_keys,
                                int n_protect) {
  if (keep >= live_count) {
    return live_count;
  }
  int write = keep;
  for (int read = keep; read < live_count; read++) {
    if (peg_move_protected(&ranked[read].move, rack, protect_keys, n_protect)) {
      if (read != write) {
        ranked[write] = ranked[read];
      }
      write++;
    }
  }
  return write;
}

// Evaluate `n` candidate moves at `fidelity_plies`, writing ranked[i] for each.
// Parallel across candidates when a pool is present, else inline.
static void peg_eval_candidates(PegPool *pool, PegWorker *workers,
                                const Game *game, int mover_idx,
                                const uint8_t *unseen, int ld_size,
                                int bag_size, const Move *const *cands, int n,
                                PegOppModel opp_model, int fidelity_plies,
                                int scenario_stride, int64_t deadline_ns,
                                ThreadControl *thread_control, PegPoll *poll,
                                const MachineLetter *eval_bag_order,
                                int eval_bag_order_len, PegRankedCand *ranked) {
  PegCandJob *jobs = malloc_or_die((size_t)n * sizeof(PegCandJob));
  for (int i = 0; i < n; i++) {
    jobs[i].base_game = game;
    jobs[i].mover_idx = mover_idx;
    jobs[i].unseen = unseen;
    jobs[i].ld_size = ld_size;
    jobs[i].cand = cands[i];
    jobs[i].bag_size = bag_size;
    jobs[i].opp_model = opp_model;
    jobs[i].fidelity_plies = fidelity_plies;
    jobs[i].scenario_stride = scenario_stride;
    jobs[i].deadline_ns = deadline_ns;
    jobs[i].thread_control = thread_control;
    jobs[i].workers = workers;
    jobs[i].out = &ranked[i];
    jobs[i].poll = poll;
    jobs[i].eval_bag_order = eval_bag_order;
    jobs[i].eval_bag_order_len = eval_bag_order_len;
  }
  if (pool) {
    void **ptrs = malloc_or_die((size_t)n * sizeof(void *));
    for (int i = 0; i < n; i++) {
      ptrs[i] = &jobs[i];
    }
    // Helper (main) thread uses the scratch slot past the worker range.
    peg_pool_submit_and_wait(pool, peg_cand_worker_fn, ptrs, n,
                             peg_pool_num_workers(pool));
    free(ptrs);
  } else {
    for (int i = 0; i < n; i++) {
      peg_cand_worker_fn(&jobs[i], 0);
    }
  }
  free(jobs);
}

// Evaluate one scenario job (a single cand x mover-draw split), folding its
// orderings into the job's own result fields.
static void peg_scenario_worker_fn(void *arg, int worker_idx) {
  PegScenarioJob *job = (PegScenarioJob *)arg;
  PegEvalCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.template_src = job->template_src;
  ctx.mover_idx = job->mover_idx;
  ctx.unseen = job->unseen;
  ctx.ld_size = job->ld_size;
  ctx.k_drawn = job->k_drawn;
  ctx.opp_model = job->opp_model;
  ctx.fidelity_plies = job->fidelity_plies;
  ctx.injection_cap = job->injection_cap;
  ctx.deadline_ns = job->deadline_ns;
  ctx.thread_control = job->thread_control;
  ctx.worker = &job->workers[worker_idx];
  // peg_eval_split reads bag_remaining (permuting a local copy), so passing the
  // job's own copy directly is fine.
  peg_eval_split(&ctx, job->mover_drawn, job->n_bag_remaining,
                 job->bag_remaining, job->weight);
  job->total_weight = ctx.total_weight;
  job->win_weight = ctx.win_weight;
  job->spread_weight = ctx.spread_weight;
  job->weight_sum = ctx.weight_sum;
  job->n_scenarios = ctx.n_scenarios;
}

// Scenario-level evaluation: build one shared post-cand template per candidate,
// expand every (cand, mover-draw split) into a job, run them all across the
// pool, then reduce per candidate. This keeps all cores busy even when a stage
// has only a few candidates (the deep stages), where per-candidate parallelism
// would leave most cores idle.
static void peg_eval_candidates_scenario(
    PegPool *pool, PegWorker *workers, const Game *prepared_base, int mover_idx,
    const uint8_t *unseen, int ld_size, int bag_size, const Move *const *cands,
    int n, PegOppModel opp_model, int fidelity_plies, int scenario_stride,
    int64_t deadline_ns, ThreadControl *thread_control, PegRankedCand *ranked) {
  // Shared per-cand templates: post-cand board + cross-sets + pruned override,
  // built once and read concurrently by the scenario workers.
  Game **templates = malloc_or_die((size_t)n * sizeof(Game *));
  for (int i = 0; i < n; i++) {
    templates[i] = game_duplicate(prepared_base);
    play_move_without_drawing_tiles(cands[i], templates[i]);
    game_gen_all_cross_sets(templates[i]);
  }

  // Expand all (cand, split) jobs. The injection cap (== pool size) opens each
  // leaf endgame's window so the monitor can lend idle cores to the long ones.
  const int injection_cap = pool ? peg_pool_num_workers(pool) : 0;
  PegScenarioJobList list = {0};
  for (int i = 0; i < n; i++) {
    PegEvalCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.template_src = templates[i];
    ctx.mover_idx = mover_idx;
    ctx.unseen = unseen;
    ctx.ld_size = ld_size;
    ctx.opp_model = opp_model;
    ctx.fidelity_plies = fidelity_plies;
    ctx.injection_cap = injection_cap;
    ctx.scenario_stride = scenario_stride;
    ctx.deadline_ns = deadline_ns;
    ctx.thread_control = thread_control;
    ctx.workers = workers;
    ctx.out_jobs = &list;
    ctx.cand_idx = i;
    const int tiles_played = move_get_tiles_played(cands[i]);
    ctx.k_drawn = tiles_played < bag_size ? tiles_played : bag_size;
    const int n_bag_remaining = bag_size - ctx.k_drawn;
    MachineLetter mover_drawn[PEG_MAX_BAG + 1];
    MachineLetter bag_remaining[PEG_MAX_BAG + 1];
    peg_enum_splits(&ctx, /*ml=*/0, ctx.k_drawn, n_bag_remaining, /*weight=*/1,
                    mover_drawn, 0, bag_remaining, 0);
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
      peg_scenario_worker_fn(&list.jobs[j], 0);
    }
  }

  // Reduce the per-split results back into per-candidate rankings.
  double *total_w = calloc_or_die((size_t)n, sizeof(double));
  double *win_w = calloc_or_die((size_t)n, sizeof(double));
  double *spread_w = calloc_or_die((size_t)n, sizeof(double));
  for (int i = 0; i < n; i++) {
    ranked[i].move = *cands[i];
    ranked[i].weight_sum = 0;
    ranked[i].n_scenarios = 0;
  }
  for (int j = 0; j < list.count; j++) {
    const PegScenarioJob *job = &list.jobs[j];
    const int i = job->cand_idx;
    total_w[i] += job->total_weight;
    win_w[i] += job->win_weight;
    spread_w[i] += job->spread_weight;
    ranked[i].weight_sum += job->weight_sum;
    ranked[i].n_scenarios += job->n_scenarios;
  }
  for (int i = 0; i < n; i++) {
    ranked[i].win_pct = total_w[i] > 0 ? win_w[i] / total_w[i] : 0.0;
    ranked[i].mean_spread = total_w[i] > 0 ? spread_w[i] / total_w[i] : 0.0;
  }
  free(total_w);
  free(win_w);
  free(spread_w);
  free(list.jobs);
  for (int i = 0; i < n; i++) {
    game_destroy(templates[i]);
  }
  free(templates);
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

// ----- injection monitor (rung 5) ------------------------------------------

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
      for (int w = 0; w < inj->n_workers; w++) {
        EndgameCtx *ctx = inj->workers[w].eg_ctx;
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
    const struct timespec nap = {0, 2L * 1000 * 1000}; // 2 ms
    nanosleep(&nap, NULL);
  }
  return NULL;
}

// ----- public entry --------------------------------------------------------

void peg_solve(const PegArgs *args, PegResult *out, ErrorStack *error_stack) {
  memset(out, 0, sizeof(*out));
  out->last_completed_stage = -1;

  Timer timer;
  ctimer_start(&timer);

  const Game *game = args->game;
  const int bag_size = bag_get_letters(game_get_bag(game));
  if (bag_size < PEG_MIN_BAG || bag_size > PEG_MAX_BAG) {
    error_stack_push(
        error_stack, ERROR_STATUS_PEG_BAG_OUT_OF_RANGE,
        get_formatted_string("PEG requires a bag of %d..%d tiles, but found %d",
                             PEG_MIN_BAG, PEG_MAX_BAG, bag_size));
    return;
  }

  const int mover_idx = game_get_player_on_turn_index(game);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  uint8_t unseen[MAX_ALPHABET_SIZE];
  peg_compute_unseen(game, mover_idx, unseen);

  // Validate an optional pinned bag ordering: it must list exactly bag_size
  // tiles, all drawable from the unseen pool (the opponent gets the remainder).
  if (args->eval_bag_order_len > 0) {
    bool order_ok = args->eval_bag_order_len == bag_size;
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
      return;
    }
  }

  // Protected ("never prune") moves: precompute their similarity keys against
  // the mover's rack so each stage can carry them past its top-K cut.
  const Rack *mover_rack = player_get_rack(game_get_player(game, mover_idx));
  const int n_protect = args->n_protect_moves;
  uint64_t *protect_keys = NULL;
  if (n_protect > 0) {
    protect_keys = malloc_or_die((size_t)n_protect * sizeof(uint64_t));
    for (int protect_idx = 0; protect_idx < n_protect; protect_idx++) {
      protect_keys[protect_idx] =
          move_get_similarity_key(args->protect_moves[protect_idx], mover_rack);
    }
  }

  // Build the root pruned KWG once and install it on a prepared base game with
  // cross-sets generated a single time. The pre-cand board's playable words are
  // a superset of any post-cand position's, so this one pruned KWG is valid for
  // every (cand, scenario) leaf. Each leaf game_copy's this prepared base —
  // carrying the override KWG and pruned cross-sets — and play_move only
  // incrementally updates the cross-sets, so no leaf rebuilds a pruned KWG or
  // regenerates all cross-sets (the profiled hotspot).
  DictionaryWordList *word_list = dictionary_word_list_create();
  const KWG *full_kwg = player_get_kwg(game_get_player(game, mover_idx));
  generate_possible_words(game, full_kwg, word_list);
  KWG *pruned_kwg = make_kwg_from_words_small(
      word_list, KWG_MAKER_OUTPUT_GADDAG, KWG_MAKER_MERGE_EXACT);
  dictionary_word_list_destroy(word_list);
  Game *prepared_base = game_duplicate(game);
  game_set_endgame_solving_mode(prepared_base);
  game_set_backup_mode(prepared_base, BACKUP_MODE_OFF);
  game_set_override_kwgs(prepared_base, pruned_kwg, NULL,
                         DUAL_LEXICON_MODE_IGNORANT);
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

  // Scenario sampling: opt-in via scenario_stride > 1, and only for bag >= 3
  // (the bag <= 2 scenario space is too small to sample without destroying
  // coverage). Default (<= 1) is full enumeration — exact, just slower.
  // Applied to every stage including the greedy seed (stage 0), whose full
  // bag-3+ enumeration over thousands of candidates is the dominant cost.
  const int scenario_stride =
      (bag_size >= 3 && args->scenario_stride > 1) ? args->scenario_stride : 1;

  // Wall-clock deadline (0 = unbounded). Each endgame leaf is also capped by
  // this so a single deep solve cannot overrun the budget.
  const double budget = args->time_budget_seconds;
  const int64_t deadline_ns =
      budget > 0.0 ? ctimer_monotonic_ns() + (int64_t)(budget * 1.0e9) : 0;

  // Per-worker scratch. One extra slot for the main thread when it helps the
  // pool drain the queue (helper index == num_workers).
  const int n_threads = args->num_threads > 1 ? args->num_threads : 1;
  PegPool *pool = n_threads > 1 ? peg_pool_create(n_threads, 0) : NULL;
  if (pool) {
    // Leaf endgames at the deep stages legitimately run for minutes; the
    // no-progress watchdog would false-positive on them (work proceeds, but a
    // single job doesn't complete within the window). Disable it for peg.
    peg_pool_set_stuck_timeout_seconds(pool, 0);
  }
  const int n_scratch = pool ? n_threads + 1 : 1;
  // Per-worker endgame TT. Shallow PEG endgames need little, and the total
  // across workers stays well under the 50%-RAM ceiling.
  double tt_fraction = 0.25 / (double)n_scratch;
  if (tt_fraction > 0.05) {
    tt_fraction = 0.05;
  }
  PegWorker *workers = malloc_or_die((size_t)n_scratch * sizeof(PegWorker));
  for (int w = 0; w < n_scratch; w++) {
    workers[w].playout_ml = move_list_create(1);
    workers[w].eg_results = endgame_results_create();
    // Pre-created so the injection monitor reads a stable, non-NULL ctx pointer
    // (no race with lazy creation inside endgame_solve_inline).
    workers[w].eg_ctx = endgame_ctx_create();
    workers[w].template_game = NULL;
    workers[w].scratch_game = NULL;
    workers[w].eg_tt = transposition_table_create(tt_fraction);
  }

  // Injection monitor: lends idle cores to in-flight leaf endgames (rung 5).
  // Only meaningful with a pool; the deep stages have few candidates, so their
  // long endgames would otherwise leave most cores idle.
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

  // Candidate set: either the caller-supplied "only solve" list (used as-is),
  // or the full generated, equity-sorted move list. Stage 0 re-ranks by win%
  // regardless, so the only-solve input order does not matter.
  MoveList *cand_ml = NULL;
  int n_cands;
  if (args->n_only_moves > 0) {
    n_cands = args->n_only_moves;
  } else {
    cand_ml = move_list_create(PEG_CAND_LIST_CAP);
    const MoveGenArgs gen_args = {
        .game = game,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
        .move_list = cand_ml,
        .tiles_played_bv = NULL,
        .initial_tiles_bv = 0,
    };
    generate_moves(&gen_args);
    n_cands = move_list_get_count(cand_ml);
  }

  if (n_cands > 0) {
    PegRankedCand *ranked =
        malloc_or_die((size_t)n_cands * sizeof(PegRankedCand));
    const Move **moves = malloc_or_die((size_t)n_cands * sizeof(Move *));

    // Stage 0: greedy evaluation of every candidate.
    for (int i = 0; i < n_cands; i++) {
      moves[i] = args->n_only_moves > 0 ? args->only_moves[i]
                                        : move_list_get_move(cand_ml, i);
    }
    peg_poll_begin_stage(args->poll, /*stage=*/0, /*fidelity_plies=*/0,
                         n_cands);
    peg_eval_candidates(pool, workers, prepared_base, mover_idx, unseen,
                        ld_size, bag_size, moves, n_cands, args->opp_model,
                        /*fidelity_plies=*/0, scenario_stride, deadline_ns,
                        args->thread_control, args->poll, args->eval_bag_order,
                        args->eval_bag_order_len, ranked);
    qsort(ranked, (size_t)n_cands, sizeof(PegRankedCand), peg_rank_cmp);
    // Drop budget-skipped sentinels (win_pct < 0) from the carried set: they
    // sort to the bottom, so the real candidates occupy [0, n_real).
    int n_real = 0;
    while (n_real < n_cands && ranked[n_real].win_pct >= 0.0) {
      n_real++;
    }
    if (n_real == 0) {
      n_real = n_cands; // nothing finished in budget; keep what we have
    }
    const int keep0 = n_real < counts[0] ? n_real : counts[0];
    // Survivors carried forward = top-K plus any protected straggler. Tracked
    // as a shrinking live_count so protected moves never leave a stale copy in
    // the unscanned tail.
    int live_count = peg_select_survivors(ranked, n_real, keep0, mover_rack,
                                          protect_keys, n_protect);
    peg_publish(out, ranked, live_count, /*stage=*/0);
    peg_poll_replace(args->poll, ranked, live_count, /*stage=*/0,
                     /*fidelity_plies=*/0, live_count);

    // Halving stages. Stage s re-evaluates the surviving top counts[s-1] (plus
    // protected stragglers) at one more ply of fidelity, then re-ranks; a stage
    // needs >= 2 candidates to be meaningful, and is skipped once the budget is
    // spent.
    for (int s = 1; s <= num_stages; s++) {
      const int keep = live_count < counts[s - 1] ? live_count : counts[s - 1];
      const int eval_count = peg_select_survivors(
          ranked, live_count, keep, mover_rack, protect_keys, n_protect);
      if (eval_count < 2) {
        break;
      }
      if (deadline_ns != 0 && ctimer_monotonic_ns() >= deadline_ns) {
        break;
      }
      peg_poll_begin_stage(args->poll, s, /*fidelity_plies=*/s + 1, eval_count);
      for (int i = 0; i < eval_count; i++) {
        moves[i] = &ranked[i].move;
      }
      // Evaluate into a separate buffer: moves[] aliases ranked[].move, so the
      // worker must not overwrite ranked[i] while later moves still point into
      // it.
      PegRankedCand *restaged =
          malloc_or_die((size_t)eval_count * sizeof(PegRankedCand));
      // Scenario-level parallelism: a halving stage has few candidates, so
      // splitting each into its scenarios keeps all cores busy.
      peg_eval_candidates_scenario(
          pool, workers, prepared_base, mover_idx, unseen, ld_size, bag_size,
          moves, eval_count, args->opp_model, /*fidelity_plies=*/s + 1,
          scenario_stride, deadline_ns, args->thread_control, restaged);
      qsort(restaged, (size_t)eval_count, sizeof(PegRankedCand), peg_rank_cmp);
      memcpy(ranked, restaged, (size_t)eval_count * sizeof(PegRankedCand));
      free(restaged);
      live_count = eval_count;
      peg_publish(out, ranked, eval_count, s);
      peg_poll_replace(args->poll, ranked, eval_count, s,
                       /*fidelity_plies=*/s + 1, eval_count);
    }

    free(moves);
    free(ranked);
  }

  // Mark the live poll done so a poller's read loop can terminate.
  peg_poll_finish(args->poll);

  // Stop the injection monitor before tearing down the workers it observes.
  if (injector_running) {
    atomic_store(&injector.stop, 1);
    cpthread_join(injector_thread);
  }

  if (cand_ml) {
    move_list_destroy(cand_ml);
  }
  free(protect_keys);
  for (int w = 0; w < n_scratch; w++) {
    move_list_destroy(workers[w].playout_ml);
    endgame_ctx_destroy(workers[w].eg_ctx);
    endgame_results_destroy(workers[w].eg_results);
    if (workers[w].template_game) {
      game_destroy(workers[w].template_game);
    }
    if (workers[w].scratch_game) {
      game_destroy(workers[w].scratch_game);
    }
    transposition_table_destroy(workers[w].eg_tt);
  }
  free(workers);
  if (pool) {
    peg_pool_destroy(pool);
  }
  // Destroy the games that reference the pruned KWG (override pointer) before
  // freeing the KWG itself.
  game_destroy(prepared_base);
  kwg_destroy(pruned_kwg);
  out->elapsed_seconds = ctimer_elapsed_seconds(&timer);
}

void peg_result_destroy(PegResult *r) {
  if (!r) {
    return;
  }
  free(r->top_cands);
  r->top_cands = NULL;
  r->n_top_cands = 0;
  free(r->per_scenario);
  r->per_scenario = NULL;
  r->n_per_scenario = 0;
}
