#include "peg.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../compat/ctime.h"
#include "../def/game_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../util/io_util.h"
#include "gameplay.h"
#include "move_gen.h"

// Stage table (see peg.h). Stage 0 is the greedy seed (top-K = all, greedy
// leaf); stages 1..5 halve the surviving candidate set while adding a ply of
// fidelity. The tail is top-2, never top-1 — a stage re-ranks a set, so its
// output needs >= 2 candidates to compare.
const int PEG_STAGE_TOP_K[PEG_NUM_STAGES] = {INT32_MAX, 32, 16, 8, 4, 2};
const int PEG_STAGE_NONEMPTY_INNER_D[PEG_NUM_STAGES] = {0, 0, 1, 2, 3, 4};
const int PEG_STAGE_EMPTIER_PLIES[PEG_NUM_STAGES] = {0, 2, 3, 4, 5, 6};

enum {
  // Candidate move-list capacity for the root generate_moves.
  PEG_CAND_LIST_CAP = 16384,
  // Greedy playout depth ceiling (a PEG playout terminates well before this).
  PEG_PLAYOUT_MAX_PLIES = 40,
  // How many ranked candidates the result carries (stage-1 input width).
  PEG_RESULT_TOP_K = 32,
};

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

// Build the post-cand game for one (mover_drawn, bag_remaining) split: bag holds
// (mover_drawn ++ bag_remaining), opp rack is unseen minus the bag, cand is
// played, then the mover draws their k_drawn tiles. Caller game_destroy()s it.
static Game *peg_make_post_cand_game(const Game *base_game, int mover_idx,
                                     const uint8_t *unseen, int ld_size,
                                     const Move *cand, int k_drawn,
                                     const MachineLetter *mover_drawn,
                                     int n_bag_remaining,
                                     const MachineLetter *bag_remaining) {
  Game *game = game_duplicate(base_game);
  game_set_endgame_solving_mode(game);
  game_set_backup_mode(game, BACKUP_MODE_OFF);
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
  play_move_without_drawing_tiles(cand, game);
  for (int i = 0; i < k_drawn; i++) {
    rack_add_letter(mover_rack, mover_drawn[i]);
    (void)bag_draw_letter(bag, mover_drawn[i], 0);
  }
  // play_move_without_drawing_tiles may flag GAME_END_REASON_STANDARD when the
  // rack empties post-placement (the no-draw endgame world). We re-stock the
  // rack right after, so clear the stale flag unless the rack is genuinely
  // empty — otherwise the greedy playout bails before simulating anything.
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
        .thread_index = 0,
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

// ----- Stage 0: greedy scenario evaluation ---------------------------------

typedef struct PegStage0Acc {
  double total_weight;
  double win_weight; // wins + 0.5 * draws, weighted
  double spread_weight;
  int64_t weight_sum;
  int n_scenarios;
} PegStage0Acc;

// Evaluate one (mover_drawn, bag_remaining) split: walk the distinct orderings
// of bag_remaining (each equally likely), greedy-play each, and fold the
// multiset weight into the accumulator.
static void peg_eval_split(const Game *base_game, int mover_idx,
                           const uint8_t *unseen, int ld_size, const Move *cand,
                           int k_drawn, const MachineLetter *mover_drawn,
                           int n_bag_remaining, MachineLetter *bag_remaining,
                           int64_t weight, MoveList *playout_ml,
                           PegStage0Acc *acc) {
  // Sort bag_remaining ascending so next_perm enumerates distinct orderings.
  for (int i = 1; i < n_bag_remaining; i++) {
    MachineLetter key = bag_remaining[i];
    int j = i - 1;
    while (j >= 0 && bag_remaining[j] > key) {
      bag_remaining[j + 1] = bag_remaining[j];
      j--;
    }
    bag_remaining[j + 1] = key;
  }
  double ordering_win = 0.0;
  double ordering_spread = 0.0;
  int n_orderings = 0;
  do {
    Game *game =
        peg_make_post_cand_game(base_game, mover_idx, unseen, ld_size, cand,
                                k_drawn, mover_drawn, n_bag_remaining,
                                bag_remaining);
    const int32_t spread = peg_greedy_playout(game, mover_idx, playout_ml);
    game_destroy(game);
    ordering_win += (spread > 0) ? 1.0 : ((spread == 0) ? 0.5 : 0.0);
    ordering_spread += (double)spread;
    n_orderings++;
  } while (peg_next_perm(bag_remaining, n_bag_remaining));

  // Each ordering is equally likely within this multiset, so the multiset's
  // weight is split evenly across its orderings.
  acc->total_weight += (double)weight;
  acc->win_weight += (double)weight * (ordering_win / n_orderings);
  acc->spread_weight += (double)weight * (ordering_spread / n_orderings);
  acc->weight_sum += weight;
  acc->n_scenarios += n_orderings;
}

// Recursively choose, per machine letter, how many tiles go to the mover's
// draw (m) and to the bag remainder (b), with m+b <= unseen[ml], mover total ==
// k_drawn and bag-remainder total == n_bag_remaining. Opp gets the complement.
static void peg_enum_splits(const Game *base_game, int mover_idx,
                            const uint8_t *unseen, int ld_size,
                            const Move *cand, int k_drawn, int ml,
                            int mover_left, int bag_rem_left, int64_t weight,
                            MachineLetter *mover_drawn, int n_mover,
                            MachineLetter *bag_remaining, int n_bag_rem,
                            MoveList *playout_ml, PegStage0Acc *acc) {
  if (ml == ld_size) {
    if (mover_left == 0 && bag_rem_left == 0) {
      // k_drawn! accounts for the order in which the mover draws its tiles.
      int64_t full_weight = weight;
      for (int f = 2; f <= k_drawn; f++) {
        full_weight *= f;
      }
      peg_eval_split(base_game, mover_idx, unseen, ld_size, cand, k_drawn,
                     mover_drawn, n_bag_rem, bag_remaining, full_weight,
                     playout_ml, acc);
    }
    return;
  }
  const int avail = unseen[ml];
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
      peg_enum_splits(base_game, mover_idx, unseen, ld_size, cand, k_drawn,
                      ml + 1, mover_left - m, bag_rem_left - b,
                      weight * add_weight, mover_drawn, n_mover + m,
                      bag_remaining, n_bag_rem + b, playout_ml, acc);
    }
  }
}

static void peg_eval_cand_stage0(const Game *base_game, int mover_idx,
                                 const uint8_t *unseen, int ld_size,
                                 const Move *cand, int bag_size,
                                 MoveList *playout_ml, PegRankedCand *out) {
  const int tiles_played = move_get_tiles_played(cand);
  const int k_drawn = tiles_played < bag_size ? tiles_played : bag_size;
  const int n_bag_remaining = bag_size - k_drawn;
  MachineLetter mover_drawn[PEG_MAX_BAG + 1];
  MachineLetter bag_remaining[PEG_MAX_BAG + 1];
  PegStage0Acc acc = {0};
  peg_enum_splits(base_game, mover_idx, unseen, ld_size, cand, k_drawn,
                  /*ml=*/0, k_drawn, n_bag_remaining, /*weight=*/1, mover_drawn,
                  0, bag_remaining, 0, playout_ml, &acc);
  out->move = *cand;
  out->win_pct = acc.total_weight > 0 ? acc.win_weight / acc.total_weight : 0.0;
  out->mean_spread =
      acc.total_weight > 0 ? acc.spread_weight / acc.total_weight : 0.0;
  out->weight_sum = acc.weight_sum;
  out->n_scenarios = acc.n_scenarios;
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
        get_formatted_string(
            "PEG requires a bag of %d..%d tiles, but found %d", PEG_MIN_BAG,
            PEG_MAX_BAG, bag_size));
    return;
  }

  const int mover_idx = game_get_player_on_turn_index(game);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  uint8_t unseen[MAX_ALPHABET_SIZE];
  peg_compute_unseen(game, mover_idx, unseen);

  // Generate the mover's candidate moves (full list, equity-sorted).
  MoveList *cand_ml = move_list_create(PEG_CAND_LIST_CAP);
  const MoveGenArgs gen_args = {
      .game = game,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .thread_index = 0,
      .move_list = cand_ml,
      .tiles_played_bv = NULL,
      .initial_tiles_bv = 0,
  };
  generate_moves(&gen_args);
  const int n_cands = move_list_get_count(cand_ml);

  // Stage 0: greedy scenario evaluation of every candidate. Single-threaded
  // for now; per-cand and per-scenario parallelism (args->num_threads) plug in
  // with the halving stages, where the work is heavy enough to pay for it.
  PegRankedCand *ranked =
      malloc_or_die((size_t)(n_cands > 0 ? n_cands : 1) * sizeof(PegRankedCand));
  MoveList *playout_ml = move_list_create(1);
  for (int i = 0; i < n_cands; i++) {
    peg_eval_cand_stage0(game, mover_idx, unseen, ld_size,
                         move_list_get_move(cand_ml, i), bag_size, playout_ml,
                         &ranked[i]);
  }
  move_list_destroy(playout_ml);
  move_list_destroy(cand_ml);

  qsort(ranked, (size_t)n_cands, sizeof(PegRankedCand), peg_rank_cmp);

  const int top_k = n_cands < PEG_RESULT_TOP_K ? n_cands : PEG_RESULT_TOP_K;
  if (top_k > 0) {
    out->top_cands = malloc_or_die((size_t)top_k * sizeof(PegRankedCand));
    memcpy(out->top_cands, ranked, (size_t)top_k * sizeof(PegRankedCand));
    out->n_top_cands = top_k;
    out->best_move = ranked[0].move;
    out->best_win = ranked[0].win_pct;
    out->best_spread = ranked[0].mean_spread;
    out->last_completed_stage = 0;
  }
  free(ranked);
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
