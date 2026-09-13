#include "pat_hyperscale_fit_test.h"

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

// Directly tests whether the hypergeometric-scaled hook/float_flex channels
// (pat_dls_champion_v2 has zero weight on them; pat_hyper_frozen_v1, a
// single frozen-policy patgen generation over 150K games -- see that
// commit -- has real trained weight there) predict real two-ply outcomes
// better than the raw-only champion, reusing the position generation and
// rollout machinery from pat_asset_fit_test.c. Much more direct than a
// whole-game autoplay match: whole-game win rate has to resolve a small
// per-move effect through a huge amount of unrelated variance (the exact
// problem that made the earlier 40,000-game-pair autoplay match against
// this same file come back at 66.6% confidence, indistinguishable from
// noise), where comparing predicted-vs-actual outcome on the same
// candidate pairs isolates the one thing that changed between the two
// files instead of drowning it in whichever side drew the better bag.
//
// Simpler than pat_asset_fit_test.c's pair selection: hook/floater
// features are live on most midgame positions with any open premium
// square (unlike own-asset eligibility, which needed a specific leave/hook
// letter match), so an arbitrary top-2 pair is expected to carry signal
// often enough without scanning for one that differs.

#define PAT_HYPERSCALE_FIT_NUM_POSITIONS 4000
#define PAT_HYPERSCALE_FIT_MOVE_LIST_CAPACITY 500

// One context per (position, weights file), reused for both candidate
// moves: it depends on the board and unseen tiles, never on which move is
// later scored against it.
static void pat_hyperscale_fit_load_context(const Game *game,
                                            const PATWeights *weights,
                                            int mover_index,
                                            PATEvalContext *ctx_out) {
  const Player *mover = game_get_player(game, mover_index);
  const LetterDistribution *ld = game_get_ld(game);
  const Board *board = game_get_board(game);
  const int cross_set_index = board_get_cross_set_index(
      game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG), mover_index);
  const Rack *opponent_rack =
      player_get_rack(game_get_player(game, 1 - mover_index));
  pat_eval_context_load(ctx_out, weights,
                        board_get_readonly_lanes(board, cross_set_index), ld,
                        player_get_rack(mover), PAT_CLASS_MASK_ALL,
                        rack_get_total_letters(opponent_rack));
}

// Plays move on a duplicate of game, then samples one hypothetical
// continuation (a uniformly random opponent rack, the opponent's own
// top-equity reply, and the mover's own top-equity reply after that),
// always under the champion's own equity function regardless of which
// weights file this pair's prediction is being scored under -- Q2 is
// meant as one common ground truth, not a moving target. Returns the
// resulting spread from the mover's side.
static int pat_hyperscale_fit_rollout_spread(const Game *game, const Move *move,
                                             int mover_index) {
  Game *rollout_game = game_duplicate(game);
  play_move(move, rollout_game, NULL);
  const int opponent_index = 1 - mover_index;
  if (game_get_game_end_reason(rollout_game) == GAME_END_REASON_NONE) {
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

void test_pat_hyperscale_fit(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1 "
      "-pat pat_dls_champion_v2");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  const PATWeights *raw_pat = player_get_pat(game_get_player(game, 0));
  assert(raw_pat);

  ErrorStack *error_stack = error_stack_create();
  PATWeights *scaled_pat = pat_create(config_get_data_paths(config),
                                      "pat_hyper_frozen_v1", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(scaled_pat);
  error_stack_destroy(error_stack);

  MoveList *candidate_list =
      move_list_create(PAT_HYPERSCALE_FIT_MOVE_LIST_CAPACITY);
  MoveList *setup_move_list = move_list_create(1);

  double squared_error_raw = 0.0;
  double squared_error_scaled = 0.0;
  int num_pairs = 0;

  for (int attempt = 0; attempt < PAT_HYPERSCALE_FIT_NUM_POSITIONS; attempt++) {
    const uint64_t seed = 700000000ULL + (uint64_t)attempt;
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
    const Move *move_b = move_list_get_move(candidate_list, 1);
    if (move_get_type(move_a) != GAME_EVENT_TILE_PLACEMENT_MOVE ||
        move_get_type(move_b) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }

    Rack leave_a;
    Rack leave_b;
    get_leave_for_move(move_a, game, &leave_a);
    get_leave_for_move(move_b, game, &leave_b);
    // Under the champion's own weights, move_get_equity is already the
    // full score+leave+PAT equity movegen recorded -- E0_raw for free.
    const double e0_raw_a = equity_to_double(move_get_equity(move_a));
    const double e0_raw_b = equity_to_double(move_get_equity(move_b));

    PATEvalContext ctx_raw;
    PATEvalContext ctx_scaled;
    pat_hyperscale_fit_load_context(game, raw_pat, mover_index, &ctx_raw);
    pat_hyperscale_fit_load_context(game, scaled_pat, mover_index, &ctx_scaled);
    // E0_scaled differs from E0_raw only in the PAT term (score and leave
    // value do not depend on which PAT file is loaded), so it is E0_raw
    // plus the delta between the two files' PAT terms for the same move.
    const double pat_delta_a =
        equity_to_double(pat_eval_move_penalty(&ctx_scaled, move_a, &leave_a)) -
        equity_to_double(pat_eval_move_penalty(&ctx_raw, move_a, &leave_a));
    const double pat_delta_b =
        equity_to_double(pat_eval_move_penalty(&ctx_scaled, move_b, &leave_b)) -
        equity_to_double(pat_eval_move_penalty(&ctx_raw, move_b, &leave_b));
    const double e0_scaled_a = e0_raw_a + pat_delta_a;
    const double e0_scaled_b = e0_raw_b + pat_delta_b;

    Move move_a_copy;
    Move move_b_copy;
    move_copy(&move_a_copy, move_a);
    move_copy(&move_b_copy, move_b);
    const int q2_a =
        pat_hyperscale_fit_rollout_spread(game, &move_a_copy, mover_index);
    const int q2_b =
        pat_hyperscale_fit_rollout_spread(game, &move_b_copy, mover_index);
    const double q2_diff = (double)(q2_a - q2_b);

    const double error_raw = q2_diff - (e0_raw_a - e0_raw_b);
    const double error_scaled = q2_diff - (e0_scaled_a - e0_scaled_b);
    squared_error_raw += error_raw * error_raw;
    squared_error_scaled += error_scaled * error_scaled;
    num_pairs++;
  }

  move_list_destroy(candidate_list);
  move_list_destroy(setup_move_list);
  pat_destroy(scaled_pat);

  const double mse_raw = num_pairs > 0 ? squared_error_raw / num_pairs : 0.0;
  const double mse_scaled =
      num_pairs > 0 ? squared_error_scaled / num_pairs : 0.0;
  printf("\nhyperscale fit pilot: %d pairs\n", num_pairs);
  printf("raw-only (champion) MSE against 2-ply reference:    %.6f\n", mse_raw);
  printf("hypergeometric-scaled MSE against 2-ply reference:  %.6f\n",
         mse_scaled);
  printf("%s\n", mse_scaled < mse_raw
                     ? "scaled model predicts real outcomes better"
                     : "scaled model does not predict real outcomes better");

  assert(num_pairs > 0);
  config_destroy(config);
}
