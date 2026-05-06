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
};

// ---------------------------------------------------------------------------
// Local helpers (intentionally duplicated from peg.c so this file stays
// self-contained and the existing PEG implementation is untouched).
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

// ---------------------------------------------------------------------------
// Per-candidate state
// ---------------------------------------------------------------------------

typedef struct BaiCand {
  SmallMove move;
  Move move_full;
  int static_score;     // Move score (used to derive prior).
  Game *post_cand_game; // Post-move game state (board + cross-sets), shared.
  double prior;         // Softmax-normalized prior in [0, 1].
  int depth_evaluated;  // 0 = no endgame eval yet; N = N-ply endgame done.
  int visits;           // Number of (depth) evaluations performed.
  double time_paid;     // Cumulative wall time spent evaluating this cand.
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
  double sort_utility; // Transient: populated by bp_populate_sort_utility
                       // immediately before each qsort, read by the
                       // qsort comparator. Stale outside that window.
} BaiCand;

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

// Evaluate one candidate at a given depth across all bag-tile scenarios in
// parallel. Updates cand->q_mean_spread, q_win_pct, depth_evaluated, visits.
static void
bp_evaluate_cand(BaiCand *cand, int depth, int mover_idx, int opp_idx,
                 const uint8_t *unseen, int ld_size,
                 const MachineLetter *tile_types, const int *tile_counts,
                 int num_tile_types, ThreadControl *thread_control,
                 TranspositionTable **per_thread_tts, dual_lexicon_mode_t dlm,
                 double endgame_time_per_solve, int num_threads,
                 int thread_index_offset, int64_t external_deadline_ns) {
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

static int bp_compare_cands_by_score(const void *a, const void *b) {
  const BaiCand *ca = (const BaiCand *)a;
  const BaiCand *cb = (const BaiCand *)b;
  return cb->static_score - ca->static_score; // descending
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

  if (bag_get_letters(game_get_bag(args->game)) != 1) {
    error_stack_push(error_stack, ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY,
                     get_formatted_string(
                         "bai_peg_solve requires exactly 1 tile in the bag"));
    return;
  }

  uint8_t unseen[MAX_ALPHABET_SIZE];
  int total_unseen = bp_compute_unseen(args->game, mover_idx, unseen);
  if (total_unseen < 1 || total_unseen > RACK_SIZE + 1) {
    error_stack_push(
        error_stack, ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY,
        get_formatted_string("bai_peg_solve: invalid unseen count %d",
                             total_unseen));
    return;
  }

  // Build base game with empty bag and WMP disabled. Each scenario solve
  // will replay the bag tile into the mover's rack and populate the
  // opponent's rack from `unseen - bag_tile`.
  Game *base_game = game_duplicate(args->game);
  {
    Bag *base_bag = game_get_bag(base_game);
    for (int ml = 0; ml < ld_size; ml++) {
      while (bag_get_letter(base_bag, ml) > 0) {
        bag_draw_letter(base_bag, (MachineLetter)ml, mover_idx);
      }
    }
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

  // Generate candidate moves.
  MoveList *initial_ml = move_list_create_small(BAI_PEG_MOVELIST_CAPACITY);
  {
    const MoveGenArgs gen_args = {
        .game = base_game,
        .move_list = initial_ml,
        .move_record_type = MOVE_RECORD_ALL_SMALL,
        .move_sort_type = MOVE_SORT_SCORE,
        .override_kwg = NULL,
        .thread_index = 0,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&gen_args);
  }

  int num_candidates = initial_ml->count;
  if (num_candidates == 0) {
    small_move_list_destroy(initial_ml);
    if (pruned_kwgs[0]) {
      kwg_destroy(pruned_kwgs[0]);
    }
    if (pruned_kwgs[1]) {
      kwg_destroy(pruned_kwgs[1]);
    }
    game_destroy(base_game);
    error_stack_push(error_stack, ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY,
                     get_formatted_string("bai_peg_solve: no legal moves"));
    return;
  }

  // Build candidate array, top-K by static score. Skip pass candidates
  // until PEG recursion is wired in — bai_peg has no way to evaluate the
  // pass branch correctly without recursing into the post-pass position.
  BaiCand *cands = calloc_or_die(num_candidates, sizeof(BaiCand));
  int kept = 0;
  for (int ci = 0; ci < num_candidates; ci++) {
    if (small_move_is_pass(initial_ml->small_moves[ci])) {
      continue;
    }
    cands[kept].move = *initial_ml->small_moves[ci];
    small_move_to_move(&cands[kept].move_full, &cands[kept].move,
                       game_get_board(base_game));
    cands[kept].static_score = (int)small_move_get_score(&cands[kept].move);
    kept++;
  }
  small_move_list_destroy(initial_ml);
  num_candidates = kept;
  if (num_candidates == 0) {
    free(cands);
    if (pruned_kwgs[0]) {
      kwg_destroy(pruned_kwgs[0]);
    }
    if (pruned_kwgs[1]) {
      kwg_destroy(pruned_kwgs[1]);
    }
    game_destroy(base_game);
    error_stack_push(
        error_stack, ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY,
        get_formatted_string("bai_peg_solve: no legal non-pass moves"));
    return;
  }

  qsort(cands, num_candidates, sizeof(BaiCand), bp_compare_cands_by_score);
  // Progressive widening keeps ALL generated moves in the pool — the active
  // subset grows from a small seed as PUCT accumulates visits, so weak moves
  // don't get touched at short budgets.
  if (!args->progressive_widening && num_candidates > initial_top_k) {
    num_candidates = initial_top_k;
  }

  // Compute softmax priors over the top-K static scores. Use a scaled
  // temperature so the gap between top moves doesn't fully starve the rest.
  {
    int max_score = cands[0].static_score;
    double T = 25.0; // points per "natural" temperature unit
    double sum = 0.0;
    for (int ci = 0; ci < num_candidates; ci++) {
      double s = exp((double)(cands[ci].static_score - max_score) / T);
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
    bp_playout_batch(cands, num_candidates, 0, mover_idx, opp_idx, unseen,
                     ld_size, tile_types, tile_counts, num_tile_types,
                     args->thread_control, per_thread_tts, dlm,
                     args->endgame_time_per_solve, num_threads, 0,
                     bai_deadline_ns, args->pure_playout);
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
      bp_evaluate_cand(&cands[ci], 1, mover_idx, opp_idx, unseen, ld_size,
                       tile_types, tile_counts, num_tile_types,
                       args->thread_control, per_thread_tts, dlm, per_solve,
                       num_threads, 0, bai_deadline_ns);
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
    // touched, so we don't pre-stage. Build it now if needed.
    if (cands[chosen].post_cand_game == NULL) {
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
    Timer eval_timer;
    ctimer_start(&eval_timer);
    bp_evaluate_cand(&cands[chosen], next_depth, mover_idx, opp_idx, unseen,
                     ld_size, tile_types, tile_counts, num_tile_types,
                     args->thread_control, per_thread_tts, dlm, per_solve,
                     num_threads, 0, bai_deadline_ns);
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
