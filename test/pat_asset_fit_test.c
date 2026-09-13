#include "pat_asset_fit_test.h"

#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/pat_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/pat.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>

// A pilot for Astra (gpt-6-astra)'s recommended own_asset_discount fit: a
// bounded search over discount values, scored against a cheap two-ply
// reference rollout rather than the existing single-shot linear regression
// (which cannot fit this parameter at all -- it enters the worst-unit
// combination rule nonlinearly). Deliberately a simplified first version of
// Astra's full spec, not the whole thing:
//   - Q2 (the two-ply reference) uses an INDEPENDENT random opponent-rack
//     draw per candidate move, not Astra's shared-world pairing (the same
//     draw reused for both candidates in a pair, which would need explicit
//     PRNG-state control this version doesn't attempt). More positions
//     substitute for the variance reduction pairing would have given.
//   - The candidate pair is the top equity move against the highest-ranked
//     other candidate whose own-asset eligibility actually differs from it
//     (found by comparing each candidate's credit at discount 1.0, the
//     cheapest available proxy for "does this move's leave qualify for
//     credit somewhere," against the top move's own -- no public accessor
//     for the eligibility set itself exists outside pat.c). Astra's fuller
//     spec would use the real eligibility set directly; num_nonzero_credit_
//     pairs reports how often a differing pair was found at all, per
//     Astra's explicit caution to check this before trusting any result.
//   - The endgame value at rollout's end is the raw current spread, with no
//     out-bonus/remaining-rack adjustment.
// The point of this pass is to see whether the fitting methodology itself
// produces a sane, non-degenerate result at all, not to ship a final
// trained discount.

#define PAT_ASSET_FIT_NUM_POSITIONS 4000
#define PAT_ASSET_FIT_MOVE_LIST_CAPACITY 500
#define PAT_ASSET_FIT_NUM_DISCOUNTS 6
// How far down the sorted candidate list to look for a move whose own-asset
// eligibility differs from the top move's, before giving up on this
// position (see the file comment on pair selection).
#define PAT_ASSET_FIT_PAIR_SCAN_CAP 30
// Rollout samples averaged per candidate/discount, each sample index paired
// against the other candidate's same-index sample via a shared world_seed
// (see pat_asset_fit_rollout_spread and pat_hyperscale_fit_test.c, which
// found this necessary to see past the single-sample noise floor).
#define PAT_ASSET_FIT_NUM_ROLLOUT_SAMPLES 15
static const double pat_asset_fit_discounts[PAT_ASSET_FIT_NUM_DISCOUNTS] = {
    0.0, 0.2, 0.4, 0.6, 0.8, 1.0};

// Builds the PATEvalContext real move evaluation would build for this
// position (see validated_move.c's own construction of the same context),
// against `scratch`, whatever discount it currently carries: the context's
// unit_penalty/unit_hook_letters/units_by_hook_letter arrays this function
// needs are pure functions of the board, unseen tiles, and the ordinary
// feature weights -- own_asset_discount affects none of them (it only
// enters later, inside pat_eval_move_penalty itself), so one context here
// serves every discount value pat_eval_move_penalty is later asked about
// for the same (position, move) pair; only the weights object's discount
// field needs to change between those calls, not the whole context.
static void pat_asset_fit_load_context(const Game *game, PATWeights *scratch,
                                       int mover_index,
                                       PATEvalContext *ctx_out) {
  const Player *mover = game_get_player(game, mover_index);
  const LetterDistribution *ld = game_get_ld(game);
  const Board *board = game_get_board(game);
  const int cross_set_index = board_get_cross_set_index(
      game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG), mover_index);
  const Rack *opponent_rack =
      player_get_rack(game_get_player(game, 1 - mover_index));
  pat_eval_context_load(ctx_out, scratch,
                        board_get_readonly_lanes(board, cross_set_index), ld,
                        player_get_rack(mover), PAT_CLASS_MASK_ALL,
                        rack_get_total_letters(opponent_rack));
}

// move's own-asset credit at `discount` relative to no discount at all,
// against the position-level context `ctx` (see pat_asset_fit_load_context
// -- one context serves every move and every discount at this position).
// Mutates and restores scratch's own_asset_discount; not safe to call
// concurrently with another such call sharing the same scratch object.
static double pat_asset_fit_credit_at_discount(PATWeights *scratch,
                                               const PATEvalContext *ctx,
                                               const Move *move,
                                               const Rack *leave,
                                               double discount) {
  pat_set_own_asset_discount(scratch, 0.0);
  const double zero = equity_to_double(pat_eval_move_penalty(ctx, move, leave));
  pat_set_own_asset_discount(scratch, discount);
  const double at_discount =
      equity_to_double(pat_eval_move_penalty(ctx, move, leave));
  return at_discount - zero;
}

// Plays move on a duplicate of game, then samples one hypothetical
// continuation (a uniformly random opponent rack, the opponent's own
// top-equity reply, and the mover's own top-equity reply after that) and
// returns the resulting spread from the mover's side -- a cheap proxy for a
// two-ply reference equity.
//
// The rollout's own players are pointed at rollout_pat (via
// player_set_pat) before either reply is chosen, so the whole rollout's
// move choices are self-consistent with whatever discount is currently
// set on it -- a rollout whose subsequent-ply choices always follow one
// fixed policy (e.g. discount always 0) can only validate discounts that
// happen to agree with that policy's own worldview, which is a referee
// bias, not a neutral ground truth (see pat_hyperscale_fit_test.c, which
// found the same bias reversed that pilot's own conclusion entirely).
// Caller sets rollout_pat's own_asset_discount to whichever value this
// specific call is meant to validate before calling this.
//
// world_seed reseeds the bag's PRNG (game_seed -> bag_seed, which
// reshuffles whatever is currently in the bag; it touches no other game
// state) immediately before the opponent's random draw, and should be the
// same value for both candidates (and every discount) at one position and
// sample index, correlating the draws instead of leaving them fully
// independent -- see pat_hyperscale_fit_test.c's identical technique.
static int pat_asset_fit_rollout_spread(const Game *game, const Move *move,
                                        int mover_index,
                                        const PATWeights *rollout_pat,
                                        uint64_t world_seed) {
  Game *rollout_game = game_duplicate(game);
  player_set_pat(game_get_player(rollout_game, 0), rollout_pat);
  player_set_pat(game_get_player(rollout_game, 1), rollout_pat);
  play_move(move, rollout_game, NULL);
  const int opponent_index = 1 - mover_index;
  if (game_get_game_end_reason(rollout_game) == GAME_END_REASON_NONE) {
    game_seed(rollout_game, world_seed);
    set_random_rack(rollout_game, opponent_index, NULL);
    MoveList *reply_list = move_list_create(1);
    const Move *opponent_reply = get_top_equity_move(rollout_game, reply_list);
    play_move(opponent_reply, rollout_game, NULL);
    move_list_destroy(reply_list);
  }
  if (game_get_game_end_reason(rollout_game) == GAME_END_REASON_NONE) {
    MoveList *reply_list = move_list_create(1);
    const Move *mover_reply = get_top_equity_move(rollout_game, reply_list);
    play_move(mover_reply, rollout_game, NULL);
    move_list_destroy(reply_list);
  }
  const Equity spread =
      player_get_score(game_get_player(rollout_game, mover_index)) -
      player_get_score(game_get_player(rollout_game, opponent_index));
  game_destroy(rollout_game);
  return equity_to_int(spread);
}

// Mean of PAT_ASSET_FIT_NUM_ROLLOUT_SAMPLES independent rollouts, each
// sample index using the same world_seed as the same-index call for
// whichever other candidate/discount this move is being compared against.
static double pat_asset_fit_average_rollout(const Game *game, const Move *move,
                                            int mover_index,
                                            const PATWeights *rollout_pat,
                                            uint64_t base_seed) {
  double total = 0.0;
  for (int sample_idx = 0; sample_idx < PAT_ASSET_FIT_NUM_ROLLOUT_SAMPLES;
       sample_idx++) {
    const uint64_t world_seed =
        base_seed + (uint64_t)sample_idx * 1000003ULL; // a prime stride
    total += (double)pat_asset_fit_rollout_spread(game, move, mover_index,
                                                  rollout_pat, world_seed);
  }
  return total / PAT_ASSET_FIT_NUM_ROLLOUT_SAMPLES;
}

void test_pat_own_asset_discount_fit(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1 "
      "-pat pat_dls_champion_v2");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  const PATWeights *live_pat = player_get_pat(game_get_player(game, 0));
  assert(live_pat);

  // Built once: hook_flex/through_score/through_count depend only on the
  // lexicon (see pat_prepare_hook_flex), and every ordinary feature weight
  // and combine_gamma are frozen at the champion's values for this whole
  // pilot, so nothing here needs rebuilding per position. Only
  // own_asset_discount is ever mutated, immediately before each
  // pat_eval_move_penalty call below.
  PATWeights *scratch = pat_create_zeroed(pat_get_name(live_pat));
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    pat_set_weight(scratch, feature_index,
                   pat_get_weight(live_pat, feature_index));
  }
  pat_set_combine_gamma(scratch, pat_get_combine_gamma(live_pat));
  pat_prepare_hook_flex(scratch, player_get_kwg(game_get_player(game, 0)),
                        game_get_ld(game));

  MoveList *candidate_list = move_list_create(PAT_ASSET_FIT_MOVE_LIST_CAPACITY);
  MoveList *setup_move_list = move_list_create(1);

  double squared_error_train[PAT_ASSET_FIT_NUM_DISCOUNTS] = {0};
  double squared_error_test[PAT_ASSET_FIT_NUM_DISCOUNTS] = {0};
  int num_train = 0;
  int num_test = 0;
  int num_nonzero_credit_pairs = 0;

  for (int attempt = 0; attempt < PAT_ASSET_FIT_NUM_POSITIONS; attempt++) {
    const uint64_t seed = 900000000ULL + (uint64_t)attempt;
    game_reset(game);
    game_seed(game, seed);
    draw_starting_racks(game);
    const int target_bag = 10 + (int)(seed % 31); // spans [10, 40]
    bool position_ok = true;
    while (bag_get_letters(game_get_bag(game)) > target_bag) {
      const Move *setup_move = get_top_equity_move(game, setup_move_list);
      play_move(setup_move, game, NULL);
      if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
        position_ok = false;
        break;
      }
    }
    if (!position_ok || bag_get_letters(game_get_bag(game)) == 0) {
      continue;
    }
    const Board *board = game_get_board(game);
    if (board_get_transposed(board) || !board_get_cross_sets_valid(board)) {
      continue;
    }

    const int mover_index = game_get_player_on_turn_index(game);
    const MoveGenArgs args = {
        .game = game,
        .move_list = candidate_list,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&args);
    move_list_sort_moves(candidate_list);
    if (move_list_get_count(candidate_list) < 2) {
      continue;
    }
    const Move *move_a = move_list_get_move(candidate_list, 0);
    if (move_get_type(move_a) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }

    // One context per position, reused for every candidate considered
    // below and every discount value in the grid (see
    // pat_asset_fit_load_context): it depends on the board and unseen
    // tiles, never on which move or leave is being scored against it.
    PATEvalContext ctx;
    pat_asset_fit_load_context(game, scratch, mover_index, &ctx);

    Rack leave_a;
    get_leave_for_move(move_a, game, &leave_a);
    const double credit_full_a =
        pat_asset_fit_credit_at_discount(scratch, &ctx, move_a, &leave_a, 1.0);

    // Scan down the already-generated, already-sorted candidate list for
    // the highest-ranked other move whose own-asset eligibility actually
    // differs from move_a's, rather than blindly pairing against rank 1:
    // most candidates share identical eligibility (own-asset situations
    // are common somewhere on a midgame board, per num_nonzero_credit_
    // pairs, but any two arbitrary candidates usually don't differ in
    // which of those units their own particular leave reaches), so an
    // arbitrary pair contributes far less signal per rollout than one
    // chosen to disagree. Cheap: reuses the one context above, and
    // credit_at_discount(1.0) needs no more than pat_eval_move_penalty
    // calls against it, no rescanning.
    const Move *move_b = NULL;
    Rack leave_b;
    const int scan_limit =
        move_list_get_count(candidate_list) < PAT_ASSET_FIT_PAIR_SCAN_CAP + 1
            ? move_list_get_count(candidate_list)
            : PAT_ASSET_FIT_PAIR_SCAN_CAP + 1;
    for (int candidate_idx = 1; candidate_idx < scan_limit; candidate_idx++) {
      const Move *candidate = move_list_get_move(candidate_list, candidate_idx);
      if (move_get_type(candidate) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
        continue;
      }
      Rack candidate_leave;
      get_leave_for_move(candidate, game, &candidate_leave);
      const double credit_full_candidate = pat_asset_fit_credit_at_discount(
          scratch, &ctx, candidate, &candidate_leave, 1.0);
      if (credit_full_candidate != credit_full_a) {
        move_b = candidate;
        leave_b = candidate_leave;
        break;
      }
    }
    if (!move_b) {
      continue;
    }

    const Equity e0_a = move_get_equity(move_a);
    const Equity e0_b = move_get_equity(move_b);

    // Copied off the move list before it can be overwritten by a later
    // attempt's generate_moves call, and before rollout mutates a
    // duplicated game rather than this one.
    Move move_a_copy;
    Move move_b_copy;
    move_copy(&move_a_copy, move_a);
    move_copy(&move_b_copy, move_b);

    const uint64_t world_seed = seed + 500000000ULL;
    const double e0_a_double = equity_to_double(e0_a);
    const double e0_b_double = equity_to_double(e0_b);
    bool any_nonzero_credit = false;
    const bool is_train = (attempt % 5) != 0; // 80/20 split
    for (int d_idx = 0; d_idx < PAT_ASSET_FIT_NUM_DISCOUNTS; d_idx++) {
      const double discount = pat_asset_fit_discounts[d_idx];
      // Leaves scratch's own_asset_discount at `discount`, which is
      // exactly the policy the rollout below needs to be self-consistent
      // with this specific grid point (see pat_asset_fit_rollout_spread).
      const double credit_a = pat_asset_fit_credit_at_discount(
          scratch, &ctx, move_a, &leave_a, discount);
      const double credit_b = pat_asset_fit_credit_at_discount(
          scratch, &ctx, move_b, &leave_b, discount);
      if (credit_a - credit_b != 0.0) {
        any_nonzero_credit = true;
      }
      const double e0_discounted_a = e0_a_double + credit_a;
      const double e0_discounted_b = e0_b_double + credit_b;

      const double q2_a = pat_asset_fit_average_rollout(
          game, &move_a_copy, mover_index, scratch, world_seed);
      const double q2_b = pat_asset_fit_average_rollout(
          game, &move_b_copy, mover_index, scratch, world_seed);
      const double error = (q2_a - q2_b) - (e0_discounted_a - e0_discounted_b);
      if (is_train) {
        squared_error_train[d_idx] += error * error;
      } else {
        squared_error_test[d_idx] += error * error;
      }
    }
    if (any_nonzero_credit) {
      num_nonzero_credit_pairs++;
    }
    if (is_train) {
      num_train++;
    } else {
      num_test++;
    }
  }

  move_list_destroy(candidate_list);
  move_list_destroy(setup_move_list);
  pat_destroy(scratch);

  printf("\nown_asset_discount fit pilot: %d train pairs, %d test pairs, "
         "%d pairs with nonzero credit at some tested discount\n",
         num_train, num_test, num_nonzero_credit_pairs);
  int best_train_idx = 0;
  for (int d_idx = 0; d_idx < PAT_ASSET_FIT_NUM_DISCOUNTS; d_idx++) {
    const double train_mse =
        num_train > 0 ? squared_error_train[d_idx] / num_train : 0.0;
    const double test_mse =
        num_test > 0 ? squared_error_test[d_idx] / num_test : 0.0;
    printf("discount %.2f: train MSE %.6f, test MSE %.6f\n",
           pat_asset_fit_discounts[d_idx], train_mse, test_mse);
    if (squared_error_train[d_idx] < squared_error_train[best_train_idx]) {
      best_train_idx = d_idx;
    }
  }
  printf("best discount by train MSE: %.2f\n",
         pat_asset_fit_discounts[best_train_idx]);

  assert(num_train + num_test > 0);
  config_destroy(config);
}
