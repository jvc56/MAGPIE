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
#include "peg_combinatorics.h"
#include "peg_pool.h"
#include "word_prune.h"
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

enum {
  // Candidate move-list capacity for the root generate_moves.
  PEG_CAND_LIST_CAP = 16384,
  // Greedy playout depth ceiling (a PEG playout terminates well before this).
  PEG_PLAYOUT_MAX_PLIES = 40,
  // Endgame depth for the exhaustive single stage (-pegtopk all / 0). An
  // emptier PEG leaf holds at most 2*RACK_SIZE tiles, so the endgame tree
  // bottoms out at terminal nodes well short of this; the overshoot is free and
  // just guarantees the solve runs to game end with no frontier truncation.
  PEG_EXHAUSTIVE_PLIES = 40,
  // Fixed endgame seed for deterministic leaf solves.
  PEG_ENDGAME_SEED = 1,
  // Pessimistic playout: capacity of the opponent reply list. The adversarial
  // 1-ply lookahead tries every generated reply (a late-game PEG position has
  // far fewer than this), so the opponent's true worst-for-mover reply — which
  // includes the rational best-equity one — is always considered.
  PEG_PESSIMISTIC_OPP_LIST_CAP = 1024,
  // Max recursion depth of the inline nested-PEG lookahead (one frame per
  // non-emptier ply). Bounds the per-level scratch stack and caps how far an
  // exhaustive nested solve can recurse through passes/exchanges before the
  // lookahead budget is spent. Comfortably above any realistic fidelity.
  PEG_MAX_NEST_DEPTH = 24,
  // Candidate list capacity for a nested-PEG level's move generation.
  PEG_NEST_CAND_LIST_CAP = 4096,
};

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
  bool
      disabled; // INVESTIGATION: when set, leaves use the root prune (no chain)
  // INVESTIGATION: per-solve leaf-rebuild accounting (build-cost break-even).
  _Atomic int64_t enum_ns;      // time in generate_possible_words
  _Atomic int64_t construct_ns; // time in make_kwg_from_words_small
  _Atomic int64_t n_builds;     // number of leaf KWGs built
  _Atomic int64_t total_words;  // summed pruned word counts (avg leaf KWG size)
} PegPruneCache;

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
  // Shared per-solve cache of per-candidate leaf prunes (see PegPruneCache).
  PegPruneCache *prune_cache;

  // Inline nested-PEG lookahead state (see peg_nested_value). Solve-level knobs
  // copied here so the recursion needn't thread them through every job/ctx.
  ThreadControl *thread_control; // needed by nested emptier endgame solves
  bool nested_enabled;
  int nested_cand_cap;
  int nested_stride;
  int nested_emptier_ply_cap;
  int nested_max_depth;
  // Per-recursion-level scratch (one frame per lookahead ply), lazily created.
  // A level's template/scratch game and move list stay live while deeper levels
  // recurse, so they cannot share the worker's top-level scratch.
  Game *nest_template[PEG_MAX_NEST_DEPTH];
  Game *nest_scratch[PEG_MAX_NEST_DEPTH];
  MoveList *nest_ml[PEG_MAX_NEST_DEPTH];
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

void peg_poll_reset(PegPoll *poll) {
  if (poll == NULL) {
    return;
  }
  cpthread_mutex_lock(&poll->mutex);
  memset(&poll->s, 0, sizeof(poll->s));
  poll->s.stage = -1;
  cpthread_mutex_unlock(&poll->mutex);
}

void peg_poll_destroy(PegPoll *poll) {
  if (poll == NULL) {
    return;
  }
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
// Finalizes the previous stage entry (sets its end_ns) and opens a new one.
static void peg_poll_begin_stage(PegPoll *poll, int stage, int fidelity_plies,
                                 int field_size) {
  if (poll == NULL) {
    return;
  }
  const int64_t now_ns = ctimer_monotonic_ns();
  cpthread_mutex_lock(&poll->mutex);
  // Finalize the previous stage: record when it ended.
  if (poll->s.n_stage_history > 0) {
    poll->s.stage_history[poll->s.n_stage_history - 1].end_ns = now_ns;
  }
  // Open a new stage entry if there is room.
  if (poll->s.n_stage_history < PEG_POLL_MAX_STAGES) {
    PegStageSnapshot *st = &poll->s.stage_history[poll->s.n_stage_history];
    st->fidelity_plies = fidelity_plies;
    st->field_size = field_size;
    st->cands_done = 0;
    st->start_ns = now_ns;
    st->end_ns = 0;
    st->best_win_pct = -1.0;
    poll->s.n_stage_history++;
  }
  poll->s.stage = stage;
  poll->s.fidelity_plies = fidelity_plies;
  poll->s.field_size = field_size;
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
  const int k = n < PEG_POLL_MAX_ENTRIES ? n : PEG_POLL_MAX_ENTRIES;
  cpthread_mutex_lock(&poll->mutex);
  for (int i = 0; i < k; i++) {
    poll->s.stage_moves[i] = *moves[i];
  }
  poll->s.n_stage_moves = k;
  poll->s.version++;
  cpthread_mutex_unlock(&poll->mutex);
}

// Bump the cands_done counter for the current stage. Call once per candidate
// completion in any stage; the leaderboard upsert (below) is separate.
static void peg_poll_bump_cand_done(PegPoll *poll) {
  if (poll == NULL) {
    return;
  }
  cpthread_mutex_lock(&poll->mutex);
  if (poll->s.n_stage_history > 0) {
    poll->s.stage_history[poll->s.n_stage_history - 1].cands_done++;
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
  // Always count this candidate as done regardless of leaderboard outcome.
  if (snap->n_stage_history > 0) {
    snap->stage_history[snap->n_stage_history - 1].cands_done++;
  }
  const double key = peg_poll_key(cand);
  int i;
  if (snap->n_entries < PEG_POLL_MAX_ENTRIES) {
    i = snap->n_entries++;
  } else if (key > peg_poll_key(&snap->entries[PEG_POLL_MAX_ENTRIES - 1])) {
    i = PEG_POLL_MAX_ENTRIES - 1;
  } else {
    snap->version++;
    cpthread_mutex_unlock(&poll->mutex);
    return false; // full and outranked — still counted done, but not inserted
  }
  while (i > 0 && peg_poll_key(&snap->entries[i - 1]) < key) {
    snap->entries[i] = snap->entries[i - 1];
    i--;
  }
  snap->entries[i] = *cand;
  // entries[0] is always the best; reflect in the current stage.
  if (snap->n_stage_history > 0) {
    snap->stage_history[snap->n_stage_history - 1].best_win_pct =
        snap->entries[0].win_pct;
  }
  snap->version++;
  const bool reordered = old_n_entries == 0 || i < snap->n_entries - 1;
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
  if (poll->s.n_stage_history > 0 && k > 0) {
    poll->s.stage_history[poll->s.n_stage_history - 1].best_win_pct =
        ranked[0].win_pct;
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

// Mark the candidate at stage_moves[move_idx] as currently in-flight so the
// status display can show a live elapsed time and '*' depth marker. Call with
// move_idx = -1 / start_ns = 0 to clear (no candidate in flight).
static void peg_poll_set_evaluating(PegPoll *poll, int move_idx,
                                    int64_t start_ns) {
  if (poll == NULL) {
    return;
  }
  cpthread_mutex_lock(&poll->mutex);
  poll->s.currently_evaluating_move_idx = move_idx;
  poll->s.eval_start_ns = start_ns;
  poll->s.version++;
  cpthread_mutex_unlock(&poll->mutex);
}

static void peg_poll_finish(PegPoll *poll) {
  if (poll == NULL) {
    return;
  }
  const int64_t now_ns = ctimer_monotonic_ns();
  cpthread_mutex_lock(&poll->mutex);
  // Finalize the last stage.
  if (poll->s.n_stage_history > 0) {
    poll->s.stage_history[poll->s.n_stage_history - 1].end_ns = now_ns;
  }
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
                                          const MachineLetter *bag_remaining) {
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
  MachineLetter all_bag[PEG_MAX_BAG + 1];
  const int n_bag = k_drawn + n_bag_remaining;
  for (int i = 0; i < k_drawn; i++) {
    all_bag[i] = mover_drawn[i];
  }
  for (int i = 0; i < n_bag_remaining; i++) {
    all_bag[k_drawn + i] = bag_remaining[i];
  }
  peg_set_opp_rack(opp_rack, unseen, ld_size, all_bag, n_bag);
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
      mover_drawn, n_bag_remaining, bag_remaining);
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
                                       MoveList *playout_ml, int inner_top_k) {
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
      game_copy(branch, game);
      play_move(move_list_get_move(opp_ml, i), branch, NULL);
      const int32_t mover_net =
          peg_greedy_playout(branch, mover_idx, rollout_ml);
      if (mover_net < worst_for_mover) {
        worst_for_mover = mover_net;
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
} PegScenarioCapture;

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
  int inner_top_k;
  int fidelity_plies;
  int injection_cap;
  int64_t deadline_ns;
  ThreadControl *thread_control;
  PegWorker *workers;
  int cand_idx;
  const PegProgress *progress; // optional; cand_idx is the cand rank
  // Result (filled by the worker, reduced per-cand afterwards).
  double total_weight;
  double win_weight;
  double spread_weight;
  int64_t weight_sum;
  int64_t win_count;
  int64_t tie_count;
  int n_scenarios;
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
} PegEvalCtx;

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
  // Ample: distinct post-cand boards reaching an exact leaf are bounded by the
  // candidate field (hundreds), so this never approaches full in practice.
  cache->capacity = 1 << 14;
  cache->count = 0;
  cache->keys = calloc_or_die((size_t)cache->capacity, sizeof(uint64_t));
  cache->values = calloc_or_die((size_t)cache->capacity, sizeof(KWG *));
  atomic_store(&cache->enum_ns, 0);
  atomic_store(&cache->construct_ns, 0);
  atomic_store(&cache->n_builds, 0);
  atomic_store(&cache->total_words, 0);
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
  uint64_t hash = 1469598103934665603ULL; // FNV-1a over the board letters
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      hash ^= (uint64_t)board_get_letter(board, row, col);
      hash *= 1099511628211ULL;
    }
  }
  return hash == 0 ? 1 : hash; // reserve 0 as the empty-slot sentinel
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
  const int64_t t0 = ctimer_monotonic_ns();
  generate_possible_words(game, parent_kwg, word_list);
  const int64_t t1 = ctimer_monotonic_ns();
  KWG *built = make_kwg_from_words_small(word_list, KWG_MAKER_OUTPUT_GADDAG,
                                         KWG_MAKER_MERGE_EXACT);
  const int64_t t2 = ctimer_monotonic_ns();
  atomic_fetch_add(&cache->enum_ns, t1 - t0);
  atomic_fetch_add(&cache->construct_ns, t2 - t1);
  atomic_fetch_add(&cache->n_builds, 1);
  atomic_fetch_add(&cache->total_words,
                   dictionary_word_list_get_count(word_list));
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

static int32_t peg_nested_value(PegWorker *worker, int level, Game *game,
                                int lookahead, int64_t deadline_ns);

// Effective recursion-depth cap: the knob, clamped to the scratch-stack size.
static int peg_nested_depth_cap(const PegWorker *worker) {
  const int knob = worker->nested_max_depth;
  if (knob > 0 && knob < PEG_MAX_NEST_DEPTH) {
    return knob;
  }
  return PEG_MAX_NEST_DEPTH - 1;
}

// The nested recursion can be deep and serial, so it must observe the same stop
// signals the outer solve does — the wall-clock deadline and a user interrupt —
// at every node and between candidates, not just hand a deadline to the leaf
// endgame. Once stopped, nodes degrade to the cheap greedy floor immediately.
static bool peg_nested_should_stop(const PegWorker *worker,
                                   int64_t deadline_ns) {
  if (deadline_ns != 0 && ctimer_monotonic_ns() >= deadline_ns) {
    return true;
  }
  return worker->thread_control != NULL &&
         thread_control_get_status(worker->thread_control) ==
             THREAD_CONTROL_STATUS_USER_INTERRUPT;
}

// Greedy-rollout floor value (on-turn perspective) on a scratch copy, since the
// playout mutates the game it walks.
static int32_t peg_nested_floor(PegWorker *worker, int level, Game *game,
                                int on_turn) {
  Game **slot = &worker->nest_scratch[level];
  if (*slot == NULL) {
    *slot = game_duplicate(game);
  } else {
    game_copy(*slot, game);
  }
  return peg_greedy_playout(*slot, on_turn, worker->playout_ml);
}

// Exact endgame value at an emptier nested leaf, from the on-turn perspective.
// Plies are capped at min(lookahead, nested_emptier_ply_cap) so emptiers
// reached deep in the lookahead need not get the full depth the top-level
// emptiers do.
static int32_t peg_nested_endgame_value(PegWorker *worker, Game *game,
                                        int on_turn, int lookahead,
                                        int64_t deadline_ns) {
  int plies = lookahead;
  if (worker->nested_emptier_ply_cap > 0 &&
      worker->nested_emptier_ply_cap < plies) {
    plies = worker->nested_emptier_ply_cap;
  }
  if (plies < 1) {
    plies = 1;
  }
  EndgameArgs ea;
  memset(&ea, 0, sizeof(ea));
  ea.thread_control = worker->thread_control;
  ea.game = game;
  ea.plies = plies;
  ea.initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  ea.num_threads = 1;
  ea.max_workers = 0; // nested endgames are small and many; no core injection
  ea.use_heuristics = true;
  ea.num_top_moves = 1;
  ea.external_deadline_ns = deadline_ns;
  ea.shared_tt = worker->eg_tt;
  if (plies >= 2 && !worker->prune_cache->disabled) {
    const KWG *leaf_kwg =
        peg_prune_cache_get(worker->prune_cache, game, on_turn);
    game_set_override_kwgs(game, leaf_kwg, NULL, DUAL_LEXICON_MODE_IGNORANT);
  }
  ea.skip_word_pruning = true;
  ea.seed = PEG_ENDGAME_SEED;
  endgame_results_reset(worker->eg_results);
  endgame_solve_inline(&worker->eg_ctx, &ea, worker->eg_results);
  if (endgame_results_get_depth(worker->eg_results, ENDGAME_RESULT_BEST) < 0) {
    return peg_greedy_playout(game, on_turn, worker->playout_ml);
  }
  const int32_t eg_val =
      endgame_results_get_value(worker->eg_results, ENDGAME_RESULT_BEST);
  const Player *me = game_get_player(game, on_turn);
  const Player *op = game_get_player(game, 1 - on_turn);
  const int32_t lead =
      equity_to_int(player_get_score(me) - player_get_score(op));
  return lead + eg_val; // on_turn is on move, so eg_val is its future delta
}

// Scenario accumulator + context for one nested candidate's split enumeration.
typedef struct PegNestCtx {
  PegWorker *worker;
  int level;                // recursion level whose scratch slots we own
  const Game *template_src; // nest_template[level]: cand played + cross-sets
  int mover_idx;            // on-turn player at this level
  const uint8_t *unseen;
  int ld_size;
  int k_drawn;
  int child_lookahead;
  int64_t deadline_ns;
  int scenario_stride;
  int64_t sampled_weight_seen;
  int64_t weight_sum;
  double value_weight; // sum of full_weight * (mover_idx-perspective spread)
} PegNestCtx;

// Distribute the unseen pool into (mover draws k_drawn, bag keeps n_bag_rem,
// opp gets the rest); for each resulting scenario recurse and fold the negamax
// value into the weighted accumulator. Mirrors peg_enum_splits, but the leaf
// action is the recursive solve rather than a win/spread tally.
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
    Game *sub = peg_make_post_cand_game_into(
        &nc->worker->nest_scratch[nc->level], nc->template_src, nc->mover_idx,
        nc->unseen, nc->ld_size, nc->k_drawn, mover_drawn, n_bag_rem,
        bag_remaining);
    const int32_t child = peg_nested_value(
        nc->worker, nc->level + 1, sub, nc->child_lookahead, nc->deadline_ns);
    const int child_turn = game_get_player_on_turn_index(sub);
    const int32_t mover_spread = (child_turn == nc->mover_idx) ? child : -child;
    nc->weight_sum += full_weight;
    nc->value_weight += (double)full_weight * (double)mover_spread;
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
      for (int i = 0; i < to_mover; i++) {
        mover_drawn[n_mover + i] = (MachineLetter)ml;
      }
      for (int i = 0; i < to_bag; i++) {
        bag_remaining[n_bag_rem + i] = (MachineLetter)ml;
      }
      peg_nest_enum_splits(nc, ml + 1, mover_left - to_mover,
                           bag_rem_left - to_bag, weight * add_weight,
                           mover_drawn, n_mover + to_mover, bag_remaining,
                           n_bag_rem + to_bag);
    }
  }
}

// Expected (over the candidate's scenarios) on-turn spread of playing `cand`.
static int32_t peg_nested_cand_value(PegWorker *worker, int level,
                                     Game *parent_game, int mover_idx,
                                     const Move *cand, const uint8_t *unseen,
                                     int ld_size, int lookahead,
                                     int64_t deadline_ns) {
  const int bag = bag_get_letters(game_get_bag(parent_game));
  const int tiles_played = move_get_tiles_played(cand);
  const int k_drawn = tiles_played < bag ? tiles_played : bag;
  const int bag_rem = bag - k_drawn;
  peg_build_template_into(&worker->nest_template[level], parent_game, cand);
  PegNestCtx nc;
  memset(&nc, 0, sizeof(nc));
  nc.worker = worker;
  nc.level = level;
  nc.template_src = worker->nest_template[level];
  nc.mover_idx = mover_idx;
  nc.unseen = unseen;
  nc.ld_size = ld_size;
  nc.k_drawn = k_drawn;
  nc.child_lookahead = lookahead - 1;
  nc.deadline_ns = deadline_ns;
  nc.scenario_stride = worker->nested_stride > 0 ? worker->nested_stride : 1;
  MachineLetter mover_drawn[PEG_MAX_BAG + 1];
  MachineLetter bag_remaining[PEG_MAX_BAG + 1];
  peg_nest_enum_splits(&nc, 0, k_drawn, bag_rem, 1, mover_drawn, 0,
                       bag_remaining, 0);
  if (nc.weight_sum == 0) {
    return peg_greedy_playout(parent_game, mover_idx, worker->playout_ml);
  }
  const double avg = nc.value_weight / (double)nc.weight_sum;
  return (int32_t)(avg >= 0 ? avg + 0.5 : avg - 0.5);
}

static int32_t peg_nested_value(PegWorker *worker, int level, Game *game,
                                int lookahead, int64_t deadline_ns) {
  const int on_turn = game_get_player_on_turn_index(game);
  if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
    const Player *me = game_get_player(game, on_turn);
    const Player *op = game_get_player(game, 1 - on_turn);
    return equity_to_int(player_get_score(me) - player_get_score(op));
  }
  // Stop signal is checked BEFORE the emptier endgame: once the budget is spent
  // (or the user interrupts) even the exact base case degrades to the cheap
  // floor, so the whole nested subtree collapses fast instead of running every
  // remaining emptier leaf to a full solve.
  if (peg_nested_should_stop(worker, deadline_ns)) {
    return peg_nested_floor(worker, level, game, on_turn);
  }
  if (bag_get_letters(game_get_bag(game)) == 0) {
    return peg_nested_endgame_value(worker, game, on_turn, lookahead,
                                    deadline_ns);
  }
  if (lookahead <= 0 || level >= peg_nested_depth_cap(worker)) {
    return peg_nested_floor(worker, level, game, on_turn);
  }
  // Generate the on-turn player's candidates (WMP path: override_kwg = NULL).
  if (worker->nest_ml[level] == NULL) {
    worker->nest_ml[level] = move_list_create(PEG_NEST_CAND_LIST_CAP);
  }
  MoveList *ml = worker->nest_ml[level];
  const MoveGenArgs ga = {
      .game = game,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .move_list = ml,
      .tiles_played_bv = NULL,
      .initial_tiles_bv = 0,
  };
  generate_moves(&ga);
  const int n = move_list_get_count(ml);
  if (n == 0) {
    return peg_nested_floor(worker, level, game, on_turn);
  }
  move_list_sort_moves(ml);
  int cap = n;
  if (worker->nested_cand_cap > 0 && worker->nested_cand_cap < cap) {
    cap = worker->nested_cand_cap;
  }
  uint8_t unseen[MAX_ALPHABET_SIZE];
  const int ld_size = (int)ld_get_size(game_get_ld(game));
  peg_compute_unseen(game, on_turn, unseen);
  int32_t best = INT32_MIN;
  for (int i = 0; i < cap; i++) {
    // Between candidates, bail on the stop signal: return the best move found
    // so far (candidates are equity-sorted, so the strongest are evaluated
    // first).
    if (i > 0 && peg_nested_should_stop(worker, deadline_ns)) {
      break;
    }
    const Move *cand = move_list_get_move(ml, i);
    const int32_t value =
        peg_nested_cand_value(worker, level, game, on_turn, cand, unseen,
                              ld_size, lookahead, deadline_ns);
    if (value > best) {
      best = value;
    }
  }
  return best;
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
    //
    // Nested lookahead: at fidelity > 0, recursively solve the opponent's
    // sub-pre-endgame (matching the emptiers' move-count horizon) instead of a
    // flat rollout, so non-emptier candidates gain depth as the cascade
    // deepens.
    if (!emptier && ctx->fidelity_plies > 0 && ctx->worker->nested_enabled) {
      const int32_t on_turn_val =
          peg_nested_value(ctx->worker, /*level=*/0, game, ctx->fidelity_plies,
                           ctx->deadline_ns);
      const int turn = game_get_player_on_turn_index(game);
      return (turn == ctx->mover_idx) ? on_turn_val : -on_turn_val;
    }
    if (ctx->opp_model == PEG_OPP_PESSIMISTIC) {
      return peg_pessimistic_playout(game, ctx->mover_idx,
                                     ctx->worker->playout_ml, ctx->inner_top_k);
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
  if (ctx->fidelity_plies >= 2 && !ctx->worker->prune_cache->disabled) {
    const KWG *leaf_kwg =
        peg_prune_cache_get(ctx->worker->prune_cache, game, ctx->mover_idx);
    game_set_override_kwgs(game, leaf_kwg, NULL, DUAL_LEXICON_MODE_IGNORANT);
  }
  ea.skip_word_pruning = true;
  ea.seed = PEG_ENDGAME_SEED;
  endgame_results_reset(ctx->worker->eg_results);
  endgame_solve_inline(&ctx->worker->eg_ctx, &ea, ctx->worker->eg_results);
  // If the solver was interrupted before completing any search depth (depth
  // remains -1 after reset), eg_results still holds a stale value from a prior
  // solve. Fall back to greedy rather than misreporting the scenario outcome.
  if (endgame_results_get_depth(ctx->worker->eg_results, ENDGAME_RESULT_BEST) <
      0) {
    return peg_greedy_playout(game, ctx->mover_idx, ctx->worker->playout_ml);
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
  row->scenario_idx = capture->count;
  peg_tiles_to_string(capture->ld, mover_drawn, k_drawn, row->drawn,
                      sizeof(row->drawn));
  peg_tiles_to_string(capture->ld, bag_order, n_bag_remaining, row->remaining,
                      sizeof(row->remaining));
  row->weight = weight;
  row->mover_total = mover_total;
  capture->count++;
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
    job.inner_top_k = ctx->inner_top_k;
    job.fidelity_plies = ctx->fidelity_plies;
    job.injection_cap = ctx->injection_cap;
    job.deadline_ns = ctx->deadline_ns;
    job.thread_control = ctx->thread_control;
    job.workers = ctx->workers;
    job.cand_idx = ctx->cand_idx;
    job.progress = ctx->progress;
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
  int ordering_wins = 0;
  int ordering_ties = 0;
  do {
    Game *game = peg_make_post_cand_game(
        ctx->worker, ctx->template_src, ctx->mover_idx, ctx->unseen,
        ctx->ld_size, ctx->k_drawn, mover_drawn, n_bag_remaining, bag_perm);
    const int32_t value = peg_eval_leaf(ctx, game);
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
  if (job->deadline_ns != 0 && ctimer_monotonic_ns() >= job->deadline_ns) {
    job->out->move = *job->cand;
    job->out->win_pct = -1.0;
    job->out->mean_spread = 0.0;
    job->out->weight_sum = 0;
    job->out->win_count = 0;
    job->out->tie_count = 0;
    job->out->n_scenarios = 0;
    job->out->eval_seconds = 0;
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
  job->out->win_count = ctx.win_count;
  job->out->tie_count = ctx.tie_count;
  job->out->n_scenarios = ctx.n_scenarios;
  job->out->eval_seconds = 0; // greedy stage is not per-candidate timed
  // Live poll: surface this finished candidate into the leaderboard so a
  // poller sees stage 0 fill in as candidates resolve.
  const bool reordered = peg_poll_upsert(job->poll, job->out);
  if (job->progress != NULL && job->progress->on_cand_done != NULL) {
    job->progress->on_cand_done(job->progress->stage_idx, job->cand_rank,
                                &job->out->move, job->out->win_pct,
                                job->out->mean_spread, job->out->n_scenarios,
                                reordered, job->progress->user_data);
  }
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
  int write_idx = keep;
  for (int read_idx = keep; read_idx < live_count; read_idx++) {
    if (peg_move_protected(&ranked[read_idx].move, rack, protect_keys,
                           n_protect)) {
      if (read_idx != write_idx) {
        ranked[write_idx] = ranked[read_idx];
      }
      write_idx++;
    }
  }
  return write_idx;
}

// Append src[from..to) to the graded list, each tagged with the fidelity (ply
// count) at which it was last scored — i.e. the deepest stage it reached.
static void peg_graded_append(PegRankedCand *graded, int *graded_fidelity,
                              int *n_graded, const PegRankedCand *src, int from,
                              int to, int fidelity) {
  for (int i = from; i < to; i++) {
    graded[*n_graded] = src[i];
    graded_fidelity[*n_graded] = fidelity;
    (*n_graded)++;
  }
}

// Evaluate `n` candidate moves at `fidelity_plies`, writing ranked[i] for each.
// Parallel across candidates when a pool is present, else inline.
static void peg_eval_candidates(
    PegPool *pool, PegWorker *workers, const Game *game, int mover_idx,
    const uint8_t *unseen, int ld_size, int bag_size, const Move *const *cands,
    int n, PegOppModel opp_model, int inner_top_k, int fidelity_plies,
    int scenario_stride, int64_t deadline_ns, ThreadControl *thread_control,
    PegPoll *poll, const PegProgress *progress,
    const MachineLetter *eval_bag_order, int eval_bag_order_len,
    PegRankedCand *ranked) {
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
  // Past the deadline: skip this scenario entirely (its result fields are
  // already zero from job creation) so a candidate whose evaluation straddles
  // the cutoff winds down within one in-flight job instead of running every
  // remaining scenario to completion. The caller drops such partial candidates.
  if (job->deadline_ns != 0 && ctimer_monotonic_ns() >= job->deadline_ns) {
    return;
  }
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
}

// Scenario-level evaluation: build one shared post-cand template per candidate,
// expand every (cand, mover-draw split) into a job, run them all across the
// pool, then reduce per candidate. This keeps all cores busy even when a stage
// has only a few candidates (the deep stages), where per-candidate parallelism
// would leave most cores idle.
static void peg_eval_candidates_scenario(
    PegPool *pool, PegWorker *workers, const Game *prepared_base, int mover_idx,
    const uint8_t *unseen, int ld_size, int bag_size, const Move *const *cands,
    int n, PegOppModel opp_model, int inner_top_k, int fidelity_plies,
    int scenario_stride, int64_t deadline_ns, ThreadControl *thread_control,
    const PegProgress *progress, PegPoll *poll, PegRankedCand *ranked) {
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
    ranked[i].win_count = 0;
    ranked[i].tie_count = 0;
    ranked[i].n_scenarios = 0;
    ranked[i].eval_seconds = 0; // set per candidate by the live caller
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
  }
  for (int i = 0; i < n; i++) {
    ranked[i].win_pct = total_w[i] > 0 ? win_w[i] / total_w[i] : 0.0;
    ranked[i].mean_spread = total_w[i] > 0 ? spread_w[i] / total_w[i] : 0.0;
    if (progress != NULL && progress->on_cand_done != NULL) {
      // This barrier path (benchmarks) has no incremental sorted insert, so
      // there is no append-vs-reorder distinction: treat each as a full redraw.
      progress->on_cand_done(progress->stage_idx, i, &ranked[i].move,
                             ranked[i].win_pct, ranked[i].mean_spread,
                             ranked[i].n_scenarios, /*reordered=*/true,
                             progress->user_data);
    }
    peg_poll_bump_cand_done(poll);
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
    const struct timespec nap = {0, 2L * 1000 * 1000}; // 2 ms
    nanosleep(&nap, NULL);
  }
  return NULL;
}

// ----- public entry --------------------------------------------------------

void peg_solve(const PegArgs *args, PegResult *out, ErrorStack *error_stack) {
  memset(out, 0, sizeof(*out));
  out->last_completed_stage = -1;

  ctimer_start(&out->timer);
  // Anchor the wall-clock deadline at the very start so the whole solve —
  // including KWG pruning and the initial greedy move generation, not just the
  // halving stages — counts against the time budget (0 = unbounded). Each
  // endgame leaf is also capped by this so a single deep solve cannot overrun.
  const double budget = args->time_budget_seconds;
  const int64_t deadline_ns =
      budget > 0.0 ? ctimer_monotonic_ns() + (int64_t)(budget * 1.0e9) : 0;
  const Game *game = args->game;
  const int mover_idx = game_get_player_on_turn_index(game);
  const int raw_bag_size = bag_get_letters(game_get_bag(game));
  // The game bag holds the real remaining bag tiles plus any opponent tiles
  // unknown to the mover: (RACK_SIZE - opp_rack_size) tiles are assumed to be
  // in the bag as the opponent's unknown holdings. Tiles explicitly on the
  // opponent's rack are already known and not counted here.
  const Rack *opp_rack_in_game =
      player_get_rack(game_get_player(game, 1 - mover_idx));
  const int opp_rack_size = (int)rack_get_total_letters(opp_rack_in_game);
  const int opp_unknown = RACK_SIZE - opp_rack_size;
  const int bag_size = raw_bag_size - opp_unknown;
  if (bag_size < PEG_MIN_BAG || bag_size > PEG_MAX_BAG) {
    error_stack_push(
        error_stack, ERROR_STATUS_PEG_BAG_OUT_OF_RANGE,
        get_formatted_string("PEG requires a bag of %d..%d tiles, but found %d",
                             PEG_MIN_BAG, PEG_MAX_BAG, bag_size));
    peg_poll_finish(args->poll); // so a waiting poller's read loop terminates
    return;
  }
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  uint8_t unseen[MAX_ALPHABET_SIZE];
  peg_compute_unseen(game, mover_idx, unseen);

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
      peg_poll_finish(args->poll);
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
      peg_poll_finish(args->poll);
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
  // regenerates all cross-sets (the dominant per-leaf cost).
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

  // Exhaustive mode: a single uncapped stage (-pegtopk all / 0). With no
  // narrowing the per-stage fidelity ramp is pointless — re-evaluating the
  // whole field at 2,3,4,... ply just to keep all of it every time — so
  // collapse to one deep stage that runs the full-depth endgame over the entire
  // field with full scenario enumeration. The greedy stage 0 still seeds the
  // ranking.
  const bool exhaustive = num_stages == 1 && counts[0] == INT_MAX;

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
  // One prune cache shared by every worker (cross-worker board reuse).
  PegPruneCache *prune_cache = peg_prune_cache_create();
  prune_cache->disabled =
      args->reprune_disabled; // INVESTIGATION (do not merge)
  PegWorker *workers = malloc_or_die((size_t)n_scratch * sizeof(PegWorker));
  for (int worker_idx = 0; worker_idx < n_scratch; worker_idx++) {
    workers[worker_idx].playout_ml = move_list_create(1);
    workers[worker_idx].eg_results = endgame_results_create();
    // Pre-created so the injection monitor reads a stable, non-NULL ctx pointer
    // (no race with lazy creation inside endgame_solve_inline).
    workers[worker_idx].eg_ctx = endgame_ctx_create();
    workers[worker_idx].template_game = NULL;
    workers[worker_idx].scratch_game = NULL;
    workers[worker_idx].eg_tt = transposition_table_create(tt_fraction);
    workers[worker_idx].prune_cache = prune_cache;
    // Nested-PEG lookahead config + per-level scratch (lazily created).
    workers[worker_idx].thread_control = args->thread_control;
    workers[worker_idx].nested_enabled = args->nested_enabled;
    workers[worker_idx].nested_cand_cap = args->nested_cand_cap;
    workers[worker_idx].nested_stride = args->nested_stride;
    workers[worker_idx].nested_emptier_ply_cap = args->nested_emptier_ply_cap;
    workers[worker_idx].nested_max_depth = args->nested_max_depth;
    for (int d = 0; d < PEG_MAX_NEST_DEPTH; d++) {
      workers[worker_idx].nest_template[d] = NULL;
      workers[worker_idx].nest_scratch[d] = NULL;
      workers[worker_idx].nest_ml[d] = NULL;
    }
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

    // Progress callbacks (NULL members are simply not fired). stage_idx is
    // updated before each stage's dispatch and read by the workers.
    PegProgress progress = {
        .on_cand_done = args->on_cand_done,
        .on_scenario_done = args->on_scenario_done,
        .user_data = args->user_data,
        .stage_idx = 0,
    };

    // Stage 0: greedy evaluation of every candidate. When cand_max_tiles_played
    // is set (investigation), drop generated candidates that play more tiles
    // than the cap, shrinking the field for a cheap to-completion solve.
    if (args->n_only_moves > 0) {
      for (int i = 0; i < n_cands; i++) {
        moves[i] = args->only_moves[i];
      }
    } else {
      int kept = 0;
      for (int i = 0; i < n_cands; i++) {
        const Move *cand = move_list_get_move(cand_ml, i);
        if (args->cand_max_tiles_played > 0 &&
            move_get_tiles_played(cand) > args->cand_max_tiles_played) {
          continue;
        }
        moves[kept++] = cand;
      }
      n_cands = kept;
    }
    peg_poll_begin_stage(args->poll, /*stage=*/0, /*fidelity_plies=*/0,
                         n_cands);
    if (args->on_stage_start != NULL) {
      args->on_stage_start(/*stage_idx=*/0, n_cands, /*inner_d=*/0,
                           /*emptier_plies=*/0, args->user_data);
    }
    peg_eval_candidates(pool, workers, prepared_base, mover_idx, unseen,
                        ld_size, bag_size, moves, n_cands, args->opp_model,
                        args->inner_top_k, /*fidelity_plies=*/0,
                        scenario_stride, deadline_ns, args->thread_control,
                        args->poll, &progress, args->eval_bag_order,
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
    const int num_kept_after_stage0 = n_real < counts[0] ? n_real : counts[0];
    // Survivors carried forward = top-K plus any protected straggler. Tracked
    // as a shrinking live_count so protected moves never leave a stale copy in
    // the unscanned tail.
    int live_count = peg_select_survivors(ranked, n_real, num_kept_after_stage0,
                                          mover_rack, protect_keys, n_protect);
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

    // Halving stages. Stage s re-evaluates the surviving top counts[s-1] (plus
    // protected stragglers) at one more ply of fidelity, then re-ranks; a stage
    // needs >= 2 candidates to be meaningful, and is skipped once the budget is
    // spent. In exhaustive mode the lone stage runs at full endgame depth
    // instead of the stage_idx+1 ramp (see `exhaustive` above).
    for (int stage_idx = 1; stage_idx <= num_stages; stage_idx++) {
      const int stage_fidelity =
          exhaustive ? PEG_EXHAUSTIVE_PLIES : stage_idx + 1;
      const int keep = live_count < counts[stage_idx - 1]
                           ? live_count
                           : counts[stage_idx - 1];
      const int eval_count = peg_select_survivors(
          ranked, live_count, keep, mover_rack, protect_keys, n_protect);
      if (eval_count < 2) {
        break;
      }
      if (deadline_ns != 0 && ctimer_monotonic_ns() >= deadline_ns) {
        break;
      }
      if (thread_control_get_status(args->thread_control) ==
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
      if (args->poll != NULL) {
        // Live mode: evaluate one candidate at a time so each completion
        // updates the pollable leaderboard (for `sta`/`shpeg`) and so we can
        // record each candidate's own wall-clock time. Each candidate still
        // gets the whole pool for its scenarios. Greedy stage 0 is not routed
        // here.
        // Publish the whole stage's moves so a live renderer can fix the move
        // column width up front (moves[] aliases ranked[].move for [0,eval)).
        peg_poll_set_stage_moves(args->poll, moves, eval_count);
        PegProgress inner = progress;
        inner.on_cand_done = NULL; // fired below, once, after the poll upsert
        done_count = 0;
        for (int i = 0; i < eval_count; i++) {
          // Stop deepening once the budget or a user interrupt hits. Checked
          // before starting a candidate so done_count counts only candidates
          // evaluated within budget; the finished ones (if >= 2) still form a
          // usable, if partial, tier at this stage's depth.
          if (deadline_ns != 0 && ctimer_monotonic_ns() >= deadline_ns) {
            break;
          }
          if (thread_control_get_status(args->thread_control) ==
              THREAD_CONTROL_STATUS_USER_INTERRUPT) {
            break;
          }
          Timer cand_timer;
          const int64_t cand_start_ns = ctimer_monotonic_ns();
          peg_poll_set_evaluating(args->poll, i, cand_start_ns);
          ctimer_start(&cand_timer);
          peg_eval_candidates_scenario(
              pool, workers, prepared_base, mover_idx, unseen, ld_size,
              bag_size, &moves[i], 1, args->opp_model, args->inner_top_k,
              stage_fidelity, scenario_stride, deadline_ns,
              args->thread_control, &inner, /*poll=*/NULL, &restaged[i]);
          restaged[i].eval_seconds = ctimer_elapsed_seconds(&cand_timer);
          peg_poll_set_evaluating(args->poll, -1, 0);
          // If the deadline passed while this candidate was evaluating, some of
          // its scenarios bailed (above), so its score is incomplete — drop it
          // rather than show or rank a partial result, and stop the stage.
          if (deadline_ns != 0 && ctimer_monotonic_ns() >= deadline_ns) {
            break;
          }
          done_count = i + 1;
          // Surface this finished candidate into the leaderboard, then stream
          // the updated ranking to the caller right away — every candidate's
          // result prints as soon as it finishes, so the deep stages fill in
          // row by row. reordered tells the renderer whether the candidate
          // slotted in above the bottom (redraw the whole list) or sorted to
          // the bottom as the new worst (just append its row).
          const bool reordered = peg_poll_upsert(args->poll, &restaged[i]);
          if (args->on_cand_done != NULL) {
            args->on_cand_done(stage_idx, i, &restaged[i].move,
                               restaged[i].win_pct, restaged[i].mean_spread,
                               restaged[i].n_scenarios, reordered,
                               args->user_data);
          }
        }
      } else {
        // Fast path (no live poll, e.g. benchmarks): scenario-level parallelism
        // across all candidates at once — a halving stage has few candidates,
        // so pooling their scenarios keeps all cores busy with a single
        // barrier.
        peg_eval_candidates_scenario(
            pool, workers, prepared_base, mover_idx, unseen, ld_size, bag_size,
            moves, eval_count, args->opp_model, args->inner_top_k,
            stage_fidelity, scenario_stride, deadline_ns, args->thread_control,
            &progress, args->poll, restaged);
      }
      // Fewer than 2 finished: nothing to compare at this depth, so discard the
      // partial work and undo this stage's drop capture, leaving every
      // candidate at its previous depth (as if the stage never ran).
      if (done_count < 2) {
        n_graded = n_graded_before_stage;
        free(restaged);
        // This stage cleared the live poll at its start but contributed
        // nothing, so restore the previous stage's ranking (still in `ranked`)
        // instead of leaving the final snapshot empty.
        peg_poll_replace(args->poll, ranked, live_count, stage_idx - 1,
                         prev_fidelity, live_count);
        break;
      }
      qsort(restaged, (size_t)done_count, sizeof(PegRankedCand), peg_rank_cmp);
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

    // Optional per-scenario detail for the published best cand. A single-cand,
    // single-threaded re-evaluation at the deepest fidelity actually reached,
    // recording each scenario's draw/remainder/weight/value. Off by default
    // (include_per_scenario) since it doubles the top cand's leaf work.
    if (args->include_per_scenario && out->n_top_cands > 0) {
      int capture_fidelity = 0;
      if (out->last_completed_stage > 0) {
        capture_fidelity =
            exhaustive ? PEG_EXHAUSTIVE_PLIES : out->last_completed_stage + 1;
      }
      PegScenarioCapture capture = {0};
      capture.ld = ld;
      PegEvalCtx ctx;
      memset(&ctx, 0, sizeof(ctx));
      ctx.mover_idx = mover_idx;
      ctx.unseen = unseen;
      ctx.ld_size = ld_size;
      ctx.opp_model = args->opp_model;
      ctx.inner_top_k = args->inner_top_k;
      ctx.fidelity_plies = capture_fidelity;
      ctx.scenario_stride = scenario_stride;
      ctx.thread_control = args->thread_control;
      ctx.worker = &workers[0];
      ctx.capture = &capture;
      peg_build_template(ctx.worker, prepared_base, &out->top_cands[0].move);
      ctx.template_src = ctx.worker->template_game;
      const int tiles_played = move_get_tiles_played(&out->top_cands[0].move);
      ctx.k_drawn = tiles_played < bag_size ? tiles_played : bag_size;
      const int n_bag_remaining = bag_size - ctx.k_drawn;
      MachineLetter mover_drawn[PEG_MAX_BAG + 1];
      MachineLetter bag_remaining[PEG_MAX_BAG + 1];
      peg_enum_splits(&ctx, /*ml=*/0, ctx.k_drawn, n_bag_remaining,
                      /*weight=*/1, mover_drawn, 0, bag_remaining, 0);
      out->per_scenario = capture.rows;
      out->n_per_scenario = capture.count;
    }

    free(moves);
    free(ranked);
  }

  // Mark the live poll done so a poller's read loop can terminate.
  peg_poll_finish(args->poll);

  if (args->poll) {
    PegPollSnapshot poll_snap;
    peg_poll_read(args->poll, &poll_snap);
    out->n_stage_history = poll_snap.n_stage_history;
    memcpy(out->stage_history, poll_snap.stage_history,
           (size_t)poll_snap.n_stage_history * sizeof(PegStageSnapshot));
  }

  // Stop the injection monitor before tearing down the workers it observes.
  if (injector_running) {
    atomic_store(&injector.stop, 1);
    cpthread_join(injector_thread);
  }

  if (cand_ml) {
    move_list_destroy(cand_ml);
  }
  free(protect_keys);
  for (int worker_idx = 0; worker_idx < n_scratch; worker_idx++) {
    move_list_destroy(workers[worker_idx].playout_ml);
    endgame_ctx_destroy(workers[worker_idx].eg_ctx);
    endgame_results_destroy(workers[worker_idx].eg_results);
    if (workers[worker_idx].template_game) {
      game_destroy(workers[worker_idx].template_game);
    }
    if (workers[worker_idx].scratch_game) {
      game_destroy(workers[worker_idx].scratch_game);
    }
    for (int d = 0; d < PEG_MAX_NEST_DEPTH; d++) {
      if (workers[worker_idx].nest_template[d]) {
        game_destroy(workers[worker_idx].nest_template[d]);
      }
      if (workers[worker_idx].nest_scratch[d]) {
        game_destroy(workers[worker_idx].nest_scratch[d]);
      }
      if (workers[worker_idx].nest_ml[d]) {
        move_list_destroy(workers[worker_idx].nest_ml[d]);
      }
    }
    transposition_table_destroy(workers[worker_idx].eg_tt);
  }
  free(workers);
  // Safe now that every worker's scratch game (which referenced cached KWGs via
  // its override pointer) has been destroyed.
  out->build_enum_ns = atomic_load(&prune_cache->enum_ns);
  out->build_construct_ns = atomic_load(&prune_cache->construct_ns);
  out->build_n = atomic_load(&prune_cache->n_builds);
  out->build_total_words = atomic_load(&prune_cache->total_words);
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
  free(r->per_scenario);
  r->per_scenario = NULL;
  r->n_per_scenario = 0;
}
