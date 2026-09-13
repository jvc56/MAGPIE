#include "pat_move_choice_test.h"

#include "../src/def/game_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/pat_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "pat_overlap_pilot_test.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// The common-reference move-choice-benefit harness (design history and
// rationale in test/pat_hyperscale_fit_test.c, which now calls into
// this file): at each fresh position two choosers each pick their own
// move; positions where they agree carry no information and are
// discarded; where they disagree, both moves are scored by the SAME
// fixed reference continuation (the champion's own two plies, worlds
// paired by seed before either candidate is played, KLV leave value of
// the mover's final rack added when the bag is nonempty), and the paired
// difference candidate - baseline is the datum. Positions come one per
// seed from independent short champion self-play games to a random bag
// size in [10, 40], so they are independent of each other (no source-
// game clustering to account for).
//
// Reported per comparison: disagreement rate, mean paired effect with SE
// and 95% CI, the effect per sampled decision (mean times disagreement
// rate), and the within-position / between-position variance
// decomposition, which says whether more worlds per position or more
// positions is the better next spend.

#define PAT_MOVE_CHOICE_NUM_WORLDS 30
#define PAT_MOVE_CHOICE_MOVE_LIST_CAPACITY 3000
#define PAT_MOVE_CHOICE_CHAMPION "pat_dls_champion_v2"

Config *pat_move_choice_config_create(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1 "
      "-pat " PAT_MOVE_CHOICE_CHAMPION);
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  return config;
}

// See the file comment. game_seed runs before the candidate is played so
// both candidates' whole remaining random stream starts from an
// identical shuffle.
static double pat_move_choice_reference_value(const Game *game,
                                              const Move *move, int mover_index,
                                              const PATWeights *reference_pat,
                                              uint64_t world_seed) {
  Game *rollout_game = game_duplicate(game);
  game_seed(rollout_game, world_seed);
  player_set_pat(game_get_player(rollout_game, 0), reference_pat);
  player_set_pat(game_get_player(rollout_game, 1), reference_pat);
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
  const Player *mover = game_get_player(rollout_game, mover_index);
  const Player *opponent = game_get_player(rollout_game, opponent_index);
  double value =
      equity_to_double(player_get_score(mover) - player_get_score(opponent));
  if (game_get_game_end_reason(rollout_game) == GAME_END_REASON_NONE) {
    value += equity_to_double(
        klv_get_leave_value(player_get_klv(mover), player_get_rack(mover)));
  }
  game_destroy(rollout_game);
  return value;
}

// One reference value per world, world r of every candidate at a position
// sharing the same seed.
static void pat_move_choice_reference_values(const Game *game, const Move *move,
                                             int mover_index,
                                             const PATWeights *reference_pat,
                                             uint64_t base_seed,
                                             double *values_out) {
  for (int world = 0; world < PAT_MOVE_CHOICE_NUM_WORLDS; world++) {
    const uint64_t world_seed =
        base_seed + (uint64_t)world * 1000003ULL; // a prime stride
    values_out[world] = pat_move_choice_reference_value(
        game, move, mover_index, reference_pat, world_seed);
  }
}

// The chooser's move at the game's current position, copied into
// move_out. Leaves the mover's PAT pointing at chooser->pat; callers
// restore it.
static void pat_move_choice_choose(Game *game, int mover_index,
                                   const PATMoveChooser *chooser,
                                   MoveList *move_list, Move *move_out) {
  player_set_pat(game_get_player(game, mover_index), chooser->pat);
  if (chooser->degrade_margin <= 0.0 && chooser->overlap_correction == 0.0) {
    move_copy(move_out, get_top_equity_move(game, move_list));
    return;
  }
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&args);
  move_list_sort_moves(move_list);
  const int num_moves = move_list_get_count(move_list);
  assert(num_moves > 0);
  const double best_equity =
      equity_to_double(move_get_equity(move_list_get_move(move_list, 0)));
  int chosen = 0;
  if (chooser->degrade_margin > 0.0) {
    // The best move at least degrade_margin below the top; the top itself
    // when nothing is that far back.
    for (int i = 1; i < num_moves; i++) {
      const double equity =
          equity_to_double(move_get_equity(move_list_get_move(move_list, i)));
      if (best_equity - equity >= chooser->degrade_margin) {
        chosen = i;
        break;
      }
    }
  } else {
    // Exhaustive rerank: every recorded move's resulting board is scored.
    // h(before) is the same for every candidate, so subtracting it would
    // not change the argmax; non-placement moves leave the board as is.
    const int narrow_before = pat_overlap_narrow_measure(game, mover_index);
    double best_adjusted = 0.0;
    game_set_backup_mode(game, BACKUP_MODE_SIMULATION);
    for (int i = 0; i < num_moves; i++) {
      const Move *move = move_list_get_move(move_list, i);
      int narrow_after = narrow_before;
      if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
        play_move_without_drawing_tiles(move, game);
        narrow_after = pat_overlap_narrow_measure(game, mover_index);
        game_unplay_last_move(game);
      }
      const double adjusted = equity_to_double(move_get_equity(move)) -
                              chooser->overlap_correction * narrow_after;
      // Strictly greater: ties keep the higher raw-equity move.
      if (i == 0 || adjusted > best_adjusted) {
        best_adjusted = adjusted;
        chosen = i;
      }
    }
    game_set_backup_mode(game, BACKUP_MODE_OFF);
  }
  move_copy(move_out, move_list_get_move(move_list, chosen));
}

void pat_move_choice_compare(Config *config, const PATMoveChooser *baseline,
                             const PATMoveChooser *candidate,
                             uint64_t seed_base, int num_positions,
                             PATMoveChoiceResult *result_out) {
  Game *game = config_get_game(config);
  const PATWeights *reference_pat = player_get_pat(game_get_player(game, 0));
  assert(reference_pat);
  MoveList *setup_move_list = move_list_create(1);
  MoveList *choice_move_list =
      move_list_create(PAT_MOVE_CHOICE_MOVE_LIST_CAPACITY);

  double sum_means = 0.0;
  double sum_means_sq = 0.0;
  double sum_within = 0.0;
  int num_disagreements = 0;
  int num_positions_considered = 0;
  double values_baseline[PAT_MOVE_CHOICE_NUM_WORLDS];
  double values_candidate[PAT_MOVE_CHOICE_NUM_WORLDS];

  for (int attempt = 0; attempt < num_positions; attempt++) {
    const uint64_t seed = seed_base + (uint64_t)attempt;
    game_reset(game);
    game_seed(game, seed);
    draw_starting_racks(game);
    player_set_pat(game_get_player(game, 0), reference_pat);
    player_set_pat(game_get_player(game, 1), reference_pat);
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
    num_positions_considered++;
    const int mover_index = game_get_player_on_turn_index(game);

    Move move_baseline;
    Move move_candidate;
    pat_move_choice_choose(game, mover_index, baseline, choice_move_list,
                           &move_baseline);
    pat_move_choice_choose(game, mover_index, candidate, choice_move_list,
                           &move_candidate);
    player_set_pat(game_get_player(game, mover_index), reference_pat);

    if (move_get_type(&move_baseline) != GAME_EVENT_TILE_PLACEMENT_MOVE ||
        move_get_type(&move_candidate) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    if (compare_moves_without_equity(&move_baseline, &move_candidate, true) ==
        -1) {
      continue;
    }
    num_disagreements++;

    const uint64_t world_seed = seed + 500000000ULL;
    pat_move_choice_reference_values(game, &move_baseline, mover_index,
                                     reference_pat, world_seed,
                                     values_baseline);
    pat_move_choice_reference_values(game, &move_candidate, mover_index,
                                     reference_pat, world_seed,
                                     values_candidate);
    double sum_d = 0.0;
    double sum_d_sq = 0.0;
    for (int world = 0; world < PAT_MOVE_CHOICE_NUM_WORLDS; world++) {
      const double d = values_candidate[world] - values_baseline[world];
      sum_d += d;
      sum_d_sq += d * d;
    }
    const double position_mean = sum_d / PAT_MOVE_CHOICE_NUM_WORLDS;
    const double position_var =
        (sum_d_sq / PAT_MOVE_CHOICE_NUM_WORLDS -
         position_mean * position_mean) *
        ((double)PAT_MOVE_CHOICE_NUM_WORLDS / (PAT_MOVE_CHOICE_NUM_WORLDS - 1));
    sum_means += position_mean;
    sum_means_sq += position_mean * position_mean;
    sum_within += position_var;
  }

  move_list_destroy(setup_move_list);
  move_list_destroy(choice_move_list);

  memset(result_out, 0, sizeof(*result_out));
  result_out->positions_considered = num_positions_considered;
  result_out->disagreements = num_disagreements;
  const int n = num_disagreements;
  printf("\n[%s] vs [%s]: %d positions considered, %d disagreements "
         "(%.2f%%)\n",
         candidate->label, baseline->label, num_positions_considered, n,
         num_positions_considered > 0 ? 100.0 * n / num_positions_considered
                                      : 0.0);
  if (n > 1) {
    const double mean = sum_means / n;
    const double var_means =
        (sum_means_sq / n - mean * mean) * ((double)n / (n - 1));
    const double se = sqrt(var_means / n);
    const double within = sum_within / n;
    const double between = var_means - within / PAT_MOVE_CHOICE_NUM_WORLDS;
    result_out->mean = mean;
    result_out->se = se;
    result_out->within_variance = within;
    result_out->between_variance = between;
    printf("  paired effect (candidate - baseline): mean %.4f, SE %.4f, 95%% "
           "CI [%.4f, %.4f]\n",
           mean, se, mean - 1.96 * se, mean + 1.96 * se);
    printf("  effect per sampled decision (mean x disagreement rate): "
           "%.4f\n",
           mean * n / num_positions_considered);
    printf("  variance decomposition: within-position %.2f, "
           "between-position %.2f (R = %d worlds); shares of Var(mean): "
           "between %.1f%%, within %.1f%%\n",
           within, between, PAT_MOVE_CHOICE_NUM_WORLDS,
           100.0 * (between / n) / var_means * n,
           100.0 * (within / (n * PAT_MOVE_CHOICE_NUM_WORLDS)) / var_means * n);
  }
}

// Whole-game reference for a chooser: games where chooser_a is player 0
// and chooser_b player 1, then the same seeds with the sides swapped.
// Reports chooser_a's mean spread with SE and win rate.
static void pat_move_choice_whole_game(Config *config,
                                       const PATMoveChooser *chooser_a,
                                       const PATMoveChooser *chooser_b,
                                       uint64_t seed_base, int num_games) {
  Game *game = config_get_game(config);
  const PATWeights *reference_pat = player_get_pat(game_get_player(game, 0));
  MoveList *move_list = move_list_create(PAT_MOVE_CHOICE_MOVE_LIST_CAPACITY);
  double sum_spread = 0.0;
  double sum_spread_sq = 0.0;
  int games = 0;
  double wins = 0.0;
  for (int pair = 0; pair < num_games; pair++) {
    for (int a_index = 0; a_index < 2; a_index++) {
      game_reset(game);
      game_seed(game, seed_base + (uint64_t)pair);
      draw_starting_racks(game);
      const PATMoveChooser *choosers[2];
      choosers[a_index] = chooser_a;
      choosers[1 - a_index] = chooser_b;
      while (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
        const int mover_index = game_get_player_on_turn_index(game);
        Move move;
        pat_move_choice_choose(game, mover_index, choosers[mover_index],
                               move_list, &move);
        play_move(&move, game, NULL);
      }
      player_set_pat(game_get_player(game, 0), reference_pat);
      player_set_pat(game_get_player(game, 1), reference_pat);
      const double spread = equity_to_double(
          player_get_score(game_get_player(game, a_index)) -
          player_get_score(game_get_player(game, 1 - a_index)));
      sum_spread += spread;
      sum_spread_sq += spread * spread;
      games++;
      if (spread > 0) {
        wins += 1.0;
      } else if (spread == 0) {
        wins += 0.5;
      }
    }
  }
  move_list_destroy(move_list);
  const double mean = sum_spread / games;
  const double var =
      (sum_spread_sq / games - mean * mean) * ((double)games / (games - 1));
  printf("  whole-game reference, [%s] vs [%s], %d games: mean spread "
         "%.2f, SE %.2f, win rate %.1f%%\n",
         chooser_a->label, chooser_b->label, games, mean, sqrt(var / games),
         100.0 * wins / games);
}

// Controls, per Astra's review, before any further feature test:
// implementation controls (identical choosers disagree nowhere; the same
// move and world always scores the same; swapping labels negates the
// mean and preserves the SE), then degradation controls at two levels (a
// fixed policy that picks moves at least 2 equity below the champion's
// top -- established as a loser by whole-game play first -- and the
// champion with hook_d1, its largest weight, zeroed) with the variance
// decomposition alongside.
void test_pat_move_choice_controls(void) {
  Config *config = pat_move_choice_config_create();
  Game *game = config_get_game(config);
  const PATWeights *champion = player_get_pat(game_get_player(game, 0));
  const PATMoveChooser champion_chooser = {.label = "champion",
                                           .pat = champion,
                                           .degrade_margin = 0.0,
                                           .overlap_correction = 0.0};
  PATMoveChoiceResult result;

  // Identical choosers.
  pat_move_choice_compare(config, &champion_chooser, &champion_chooser,
                          900000000ULL, 2000, &result);
  assert(result.positions_considered > 1500);
  assert(result.disagreements == 0);

  // Same move, same world, same value.
  {
    game_reset(game);
    game_seed(game, 123456789ULL);
    draw_starting_racks(game);
    MoveList *move_list = move_list_create(1);
    for (int i = 0; i < 6; i++) {
      play_move(get_top_equity_move(game, move_list), game, NULL);
    }
    const int mover_index = game_get_player_on_turn_index(game);
    Move move;
    move_copy(&move, get_top_equity_move(game, move_list));
    move_list_destroy(move_list);
    const double first = pat_move_choice_reference_value(
        game, &move, mover_index, champion, 987654321ULL);
    const double second = pat_move_choice_reference_value(
        game, &move, mover_index, champion, 987654321ULL);
    assert(first == second);
  }

  // Obvious degradation, and the label-swap control on it.
  const PATMoveChooser degraded_2 = {.label = "champion, best move >= 2 "
                                              "equity below top",
                                     .pat = champion,
                                     .degrade_margin = 2.0,
                                     .overlap_correction = 0.0};
  printf("\nobvious degradation control:\n");
  pat_move_choice_whole_game(config, &degraded_2, &champion_chooser,
                             910000000ULL, 100);
  pat_move_choice_compare(config, &champion_chooser, &degraded_2, 920000000ULL,
                          12000, &result);
  PATMoveChoiceResult swapped;
  pat_move_choice_compare(config, &degraded_2, &champion_chooser, 920000000ULL,
                          12000, &swapped);
  assert(swapped.disagreements == result.disagreements);
  assert(fabs(swapped.mean + result.mean) < 1e-9);
  assert(fabs(swapped.se - result.se) < 1e-9);
  printf("  label swap: mean negated, SE preserved\n");

  // A smaller fixed degradation, to see where sensitivity runs out.
  const PATMoveChooser degraded_half = {.label = "champion, best move >= 0.5 "
                                                 "equity below top",
                                        .pat = champion,
                                        .degrade_margin = 0.5,
                                        .overlap_correction = 0.0};
  printf("\nsmall degradation control:\n");
  pat_move_choice_compare(config, &champion_chooser, &degraded_half,
                          930000000ULL, 12000, &result);

  // Targeted degradation: hook_d1 zeroed, nothing else changed.
  ErrorStack *error_stack = error_stack_create();
  PATWeights *no_hook_d1 = pat_create(config_get_data_paths(config),
                                      PAT_MOVE_CHOICE_CHAMPION, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  assert(pat_get_weight(no_hook_d1, PAT_FEATURE_HOOK_START) < 0);
  pat_set_weight(no_hook_d1, PAT_FEATURE_HOOK_START, 0);
  const PATMoveChooser no_hook_d1_chooser = {.label = "champion with hook_d1 "
                                                      "zeroed",
                                             .pat = no_hook_d1,
                                             .degrade_margin = 0.0,
                                             .overlap_correction = 0.0};
  printf("\ntargeted degradation control:\n");
  pat_move_choice_whole_game(config, &no_hook_d1_chooser, &champion_chooser,
                             940000000ULL, 1500);
  pat_move_choice_compare(config, &champion_chooser, &no_hook_d1_chooser,
                          950000000ULL, 12000, &result);
  pat_destroy(no_hook_d1);
  config_destroy(config);
}

// Step 3 of Astra's stopping rule for the narrow overlap measure: the
// champion reranked by equity - c * narrow_overlap(resulting board), for
// c = -2 (credit) and c = +2 (penalty), each against the champion. This
// is the development batch; one sign (or neither) is chosen from it and
// frozen for test_pat_overlap_step3_confirm on a separate seed range.
void test_pat_overlap_step3_dev(void) {
  Config *config = pat_move_choice_config_create();
  Game *game = config_get_game(config);
  const PATWeights *champion = player_get_pat(game_get_player(game, 0));
  const PATMoveChooser champion_chooser = {.label = "champion",
                                           .pat = champion,
                                           .degrade_margin = 0.0,
                                           .overlap_correction = 0.0};
  const PATMoveChooser credit = {.label = "champion, -2 per narrow overlap "
                                          "(credit)",
                                 .pat = champion,
                                 .degrade_margin = 0.0,
                                 .overlap_correction = -2.0};
  const PATMoveChooser penalty = {.label = "champion, +2 per narrow overlap "
                                           "(penalty)",
                                  .pat = champion,
                                  .degrade_margin = 0.0,
                                  .overlap_correction = 2.0};
  PATMoveChoiceResult result;
  pat_move_choice_compare(config, &champion_chooser, &credit, 830000000ULL,
                          12000, &result);
  pat_move_choice_compare(config, &champion_chooser, &penalty, 830000000ULL,
                          12000, &result);
  config_destroy(config);
}

// Frozen after the development batch: set PAT_OVERLAP_STEP3_CONFIRM_C to
// the chosen sign's correction (0 means neither was chosen and this test
// does nothing).
#define PAT_OVERLAP_STEP3_CONFIRM_C 0.0

void test_pat_overlap_step3_confirm(void) {
  if (PAT_OVERLAP_STEP3_CONFIRM_C == 0.0) {
    printf("no correction frozen for confirmation\n");
    return;
  }
  Config *config = pat_move_choice_config_create();
  Game *game = config_get_game(config);
  const PATWeights *champion = player_get_pat(game_get_player(game, 0));
  const PATMoveChooser champion_chooser = {.label = "champion",
                                           .pat = champion,
                                           .degrade_margin = 0.0,
                                           .overlap_correction = 0.0};
  const PATMoveChooser frozen = {.label = "champion, frozen narrow overlap "
                                          "correction",
                                 .pat = champion,
                                 .degrade_margin = 0.0,
                                 .overlap_correction =
                                     PAT_OVERLAP_STEP3_CONFIRM_C};
  PATMoveChoiceResult result;
  pat_move_choice_compare(config, &champion_chooser, &frozen, 840000000ULL,
                          48000, &result);
  config_destroy(config);
}
