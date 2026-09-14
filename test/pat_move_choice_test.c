#include "pat_move_choice_test.h"

#include "../src/def/game_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/pat_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"
#include "../src/ent/kwg.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/util/io_util.h"
#include "pat_overlap_pilot_test.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
// How many plies the reference continuation plays after the candidate:
// an even number, so the mover always makes the last of them and the
// horizon valuation (spread plus each side's last leave) keeps one
// convention.
// 2 is what every comparison before 2026-09-14 used; it cannot credit
// defense that pays off after the opponent's first reply, and preferred
// models that whole-game play rejects (the 2-ply-label retrain: harness
// +0.44 +/- 0.08, whole game -0.53 +/- 0.12). Set per run through the
// spec's optional trailing field.
static int pat_move_choice_reference_plies = 2;

static double pat_move_choice_reference_value(const Game *game,
                                              const Move *move, int mover_index,
                                              const PATWeights *reference_pat,
                                              uint64_t world_seed) {
  Game *rollout_game = game_duplicate(game);
  game_seed(rollout_game, world_seed);
  player_set_pat(game_get_player(rollout_game, 0), reference_pat);
  player_set_pat(game_get_player(rollout_game, 1), reference_pat);
  const int opponent_index = 1 - mover_index;
  // Each player's leave from their last move inside the rollout: the
  // rack they kept before drawing, which the KLV values (a full seven-tile
  // rack after the draw is not in the KLV and would read as zero, which
  // an earlier version of this leaf silently did).
  Rack last_leave[2];
  bool has_leave[2] = {false, false};
  for (int p = 0; p < 2; p++) {
    rack_set_dist_size(&last_leave[p], ld_get_size(game_get_ld(game)));
    rack_reset(&last_leave[p]);
  }
  play_move(move, rollout_game, &last_leave[mover_index]);
  has_leave[mover_index] = true;
  if (game_get_game_end_reason(rollout_game) == GAME_END_REASON_NONE) {
    // The opponent's rack is unknown to the mover: a fresh draw from the
    // seeded bag, once, before their first reply.
    set_random_rack(rollout_game, opponent_index, NULL);
  }
  MoveList *reply_list = move_list_create(1);
  for (int ply = 0; ply < pat_move_choice_reference_plies; ply++) {
    if (game_get_game_end_reason(rollout_game) != GAME_END_REASON_NONE) {
      break;
    }
    const int on_turn = game_get_player_on_turn_index(rollout_game);
    const Move *reply = get_top_equity_move(rollout_game, reply_list);
    play_move(reply, rollout_game, &last_leave[on_turn]);
    has_leave[on_turn] = true;
  }
  move_list_destroy(reply_list);
  const Player *mover = game_get_player(rollout_game, mover_index);
  const Player *opponent = game_get_player(rollout_game, opponent_index);
  double value =
      equity_to_double(player_get_score(mover) - player_get_score(opponent));
  if (game_get_game_end_reason(rollout_game) == GAME_END_REASON_NONE) {
    // Symmetric leaf: what each side kept, from the mover's perspective.
    // Skipped when the rollout ended the game, whose settlements are
    // already in the spread.
    if (has_leave[mover_index]) {
      value += equity_to_double(
          klv_get_leave_value(player_get_klv(mover), &last_leave[mover_index]));
    }
    if (has_leave[opponent_index]) {
      value -= equity_to_double(klv_get_leave_value(
          player_get_klv(opponent), &last_leave[opponent_index]));
    }
  }
  game_destroy(rollout_game);
  return value;
}

// One reference value per world, world r of every candidate at a position
// sharing the same seed.
static void pat_move_choice_reference_values(const Game *game, const Move *move,
                                             int mover_index,
                                             const PATWeights *reference_pat,
                                             uint64_t base_seed, int num_worlds,
                                             double *values_out) {
  for (int world = 0; world < num_worlds; world++) {
    const uint64_t world_seed =
        base_seed + (uint64_t)world * 1000003ULL; // a prime stride
    values_out[world] = pat_move_choice_reference_value(
        game, move, mover_index, reference_pat, world_seed);
  }
}

// Scratch context for the pat_scale chooser (single-threaded tests).
static PATEvalContext *pat_move_choice_scale_ctx(void) {
  static PATEvalContext *ctx = NULL;
  if (ctx == NULL) {
    ctx = malloc_or_die(sizeof(PATEvalContext));
  }
  return ctx;
}

// A move's equity as a double, with the pass sentinel mapped far below
// anything a real move scores (MOVE_RECORD_ALL lists include the pass).
static double pat_move_choice_move_equity(const Move *move) {
  if (move_get_type(move) == GAME_EVENT_PASS) {
    return -1.0e9;
  }
  return equity_to_double(move_get_equity(move));
}

// The chooser's move at the game's current position, copied into
// move_out. Leaves the mover's PAT pointing at chooser->pat; callers
// restore it. Every chooser picks from the same exhaustively generated,
// fully sorted list -- including a plain one, which takes its top entry
// -- rather than from MOVE_RECORD_BEST: best-only generation can prune
// an exactly tied move that exhaustive generation keeps, and the
// comparator's tie-break may then prefer the kept one, so two choosers
// that value every move identically could otherwise "disagree" on ties.
static void pat_move_choice_choose(Game *game, int mover_index,
                                   const PATMoveChooser *chooser,
                                   MoveList *move_list, Move *move_out) {
  player_set_pat(game_get_player(game, mover_index), chooser->pat);
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
      pat_move_choice_move_equity(move_list_get_move(move_list, 0));
  int chosen = 0;
  const bool scaled = chooser->pat_scale != 0.0 && chooser->pat_scale != 1.0;
  if (chooser->degrade_margin <= 0.0 && chooser->overlap_correction == 0.0 &&
      !scaled) {
    // Plain: the top of the sorted list.
  } else if (scaled) {
    // The PAT term of each candidate, recomputed from the position
    // context, rescaled: equity - pat + pat_scale * pat. Ties keep the
    // higher raw-equity move.
    const Player *mover = game_get_player(game, mover_index);
    const int csi = board_get_cross_set_index(
        game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG), mover_index);
    PATEvalContext *ctx = pat_move_choice_scale_ctx();
    pat_eval_context_load(
        ctx, chooser->pat, board_get_readonly_lanes(game_get_board(game), csi),
        game_get_ld(game), player_get_rack(mover), PAT_CLASS_MASK_ALL,
        rack_get_total_letters(
            player_get_rack(game_get_player(game, 1 - mover_index))));
    double best_adjusted = 0.0;
    for (int i = 0; i < num_moves; i++) {
      const Move *move = move_list_get_move(move_list, i);
      double adjusted = pat_move_choice_move_equity(move);
      if (move_get_type(move) != GAME_EVENT_PASS) {
        Rack leave;
        get_leave_for_move(move, game, &leave);
        const double pat =
            equity_to_double(pat_eval_move_penalty(ctx, move, &leave));
        adjusted += (chooser->pat_scale - 1.0) * pat;
      }
      if (i == 0 || adjusted > best_adjusted) {
        best_adjusted = adjusted;
        chosen = i;
      }
    }
  } else if (chooser->degrade_margin > 0.0) {
    // The best move at least degrade_margin below the top; the top itself
    // when nothing is that far back.
    for (int i = 1; i < num_moves; i++) {
      const Move *move = move_list_get_move(move_list, i);
      if (move_get_type(move) == GAME_EVENT_PASS) {
        continue;
      }
      const double equity = pat_move_choice_move_equity(move);
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
      const double adjusted = pat_move_choice_move_equity(move) -
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
                             int num_worlds, PATMoveChoiceResult *result_out) {
  pat_move_choice_compare_shard(config, baseline, candidate, seed_base,
                                num_positions, num_worlds, 0, 1, result_out);
}

void pat_move_choice_compare_shard(Config *config,
                                   const PATMoveChooser *baseline,
                                   const PATMoveChooser *candidate,
                                   uint64_t seed_base, int num_positions,
                                   int num_worlds, int shard, int num_shards,
                                   PATMoveChoiceResult *result_out) {
  pat_move_choice_compare_range(config, baseline, candidate, seed_base,
                                num_positions, num_worlds, shard, num_shards,
                                PAT_MOVE_CHOICE_DEFAULT_BAG_LO,
                                PAT_MOVE_CHOICE_DEFAULT_BAG_HI, result_out);
}

void pat_move_choice_compare_range(Config *config,
                                   const PATMoveChooser *baseline,
                                   const PATMoveChooser *candidate,
                                   uint64_t seed_base, int num_positions,
                                   int num_worlds, int shard, int num_shards,
                                   int bag_lo, int bag_hi,
                                   PATMoveChoiceResult *result_out) {
  assert(num_worlds > 1 && num_worlds <= PAT_MOVE_CHOICE_MAX_WORLDS);
  assert(num_shards >= 1 && shard >= 0 && shard < num_shards);
  assert(bag_lo >= 1 && bag_hi >= bag_lo);
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
  double values_baseline[PAT_MOVE_CHOICE_MAX_WORLDS];
  double values_candidate[PAT_MOVE_CHOICE_MAX_WORLDS];

  for (int attempt = 0; attempt < num_positions; attempt++) {
    // Shards interleave attempts so every shard sees the same mix of
    // seeds; pooling the POOL lines below over all shards reproduces the
    // unsharded run exactly.
    if (attempt % num_shards != shard) {
      continue;
    }
    const uint64_t seed = seed_base + (uint64_t)attempt;
    game_reset(game);
    game_seed(game, seed);
    draw_starting_racks(game);
    player_set_pat(game_get_player(game, 0), reference_pat);
    player_set_pat(game_get_player(game, 1), reference_pat);
    const int target_bag =
        bag_lo + (int)(seed % (uint64_t)(bag_hi - bag_lo + 1));
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
                                     reference_pat, world_seed, num_worlds,
                                     values_baseline);
    pat_move_choice_reference_values(game, &move_candidate, mover_index,
                                     reference_pat, world_seed, num_worlds,
                                     values_candidate);
    double sum_d = 0.0;
    double sum_d_sq = 0.0;
    for (int world = 0; world < num_worlds; world++) {
      const double d = values_candidate[world] - values_baseline[world];
      sum_d += d;
      sum_d_sq += d * d;
    }
    const double position_mean = sum_d / num_worlds;
    const double position_var =
        (sum_d_sq / num_worlds - position_mean * position_mean) *
        ((double)num_worlds / (num_worlds - 1));
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
  // Raw accumulators, for exact pooling across shards (see
  // test/pat_move_choice_pool.py).
  printf("POOL candidate=\"%s\" baseline=\"%s\" shard=%d/%d positions=%d "
         "disagreements=%d worlds=%d bag=%d-%d plies=%d sum_means=%.17g "
         "sum_means_sq=%.17g sum_within=%.17g\n",
         candidate->label, baseline->label, shard, num_shards,
         num_positions_considered, n, num_worlds, bag_lo, bag_hi,
         pat_move_choice_reference_plies, sum_means, sum_means_sq, sum_within);
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
    const double between = var_means - within / num_worlds;
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
           within, between, num_worlds, 100.0 * between / var_means,
           100.0 * (within / num_worlds) / var_means);
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
                          900000000ULL, 2000, PAT_MOVE_CHOICE_DEFAULT_WORLDS,
                          &result);
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
                          4000, PAT_MOVE_CHOICE_DEFAULT_WORLDS, &result);
  PATMoveChoiceResult swapped;
  pat_move_choice_compare(config, &degraded_2, &champion_chooser, 920000000ULL,
                          4000, PAT_MOVE_CHOICE_DEFAULT_WORLDS, &swapped);
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
                          930000000ULL, 4000, PAT_MOVE_CHOICE_DEFAULT_WORLDS,
                          &result);

  config_destroy(config);
  test_pat_move_choice_targeted_controls();
}

// The reload and targeted-degradation controls, separately runnable.
void test_pat_move_choice_targeted_controls(void) {
  Config *config = pat_move_choice_config_create();
  Game *game = config_get_game(config);
  const PATWeights *champion = player_get_pat(game_get_player(game, 0));
  const PATMoveChooser champion_chooser = {.label = "champion",
                                           .pat = champion,
                                           .degrade_margin = 0.0,
                                           .overlap_correction = 0.0};
  PATMoveChoiceResult result;

  // A file loaded straight through pat_create has no lexicon tables
  // (hook_flex, through_score, through_count): only pat_prepare_hook_flex
  // installs them, and evaluation refuses a model without them (an
  // earlier revision of this harness measured such a candidate at 12%
  // disagreements with the same weights prepared). A reloaded, prepared
  // champion must be indistinguishable from the config's own.
  printf("\nreload controls:\n");
  ErrorStack *error_stack = error_stack_create();
  PATWeights *reloaded = pat_create(config_get_data_paths(config),
                                    PAT_MOVE_CHOICE_CHAMPION, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(!pat_get_prepared(reloaded));
  pat_prepare_hook_flex(reloaded, player_get_kwg(game_get_player(game, 0)),
                        game_get_ld(game));
  assert(pat_get_prepared(reloaded));
  const PATMoveChooser reloaded_prepared = {.label = "champion reloaded, "
                                                     "lexicon tables prepared",
                                            .pat = reloaded,
                                            .degrade_margin = 0.0,
                                            .overlap_correction = 0.0};
  pat_move_choice_compare(config, &champion_chooser, &reloaded_prepared,
                          960000000ULL, 2000, PAT_MOVE_CHOICE_DEFAULT_WORLDS,
                          &result);
  assert(result.disagreements == 0);
  pat_destroy(reloaded);

  // No PAT at all: the champion's whole term removed. Must disagree often.
  PATWeights *zeroed = pat_create_zeroed("zeroed");
  pat_prepare_hook_flex(zeroed, player_get_kwg(game_get_player(game, 0)),
                        game_get_ld(game));
  const PATMoveChooser zeroed_chooser = {.label = "all PAT weights zero",
                                         .pat = zeroed,
                                         .degrade_margin = 0.0,
                                         .overlap_correction = 0.0};
  pat_move_choice_compare(config, &champion_chooser, &zeroed_chooser,
                          960000000ULL, 1000, PAT_MOVE_CHOICE_DEFAULT_WORLDS,
                          &result);
  pat_destroy(zeroed);

  // Targeted degradation: hook_d1 zeroed, nothing else changed.
  PATWeights *no_hook_d1 = pat_create(config_get_data_paths(config),
                                      PAT_MOVE_CHOICE_CHAMPION, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  pat_prepare_hook_flex(no_hook_d1, player_get_kwg(game_get_player(game, 0)),
                        game_get_ld(game));
  assert(pat_get_weight(no_hook_d1, PAT_FEATURE_HOOK_START) < 0);
  pat_set_weight(no_hook_d1, PAT_FEATURE_HOOK_START, 0);
  const PATMoveChooser no_hook_d1_chooser = {.label = "champion with hook_d1 "
                                                      "zeroed",
                                             .pat = no_hook_d1,
                                             .degrade_margin = 0.0,
                                             .overlap_correction = 0.0};
  printf("\ntargeted degradation control:\n");
  pat_move_choice_whole_game(config, &no_hook_d1_chooser, &champion_chooser,
                             940000000ULL, 300);
  pat_move_choice_compare(config, &champion_chooser, &no_hook_d1_chooser,
                          950000000ULL, 3000, PAT_MOVE_CHOICE_DEFAULT_WORLDS,
                          &result);
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
                          12000, PAT_MOVE_CHOICE_DEFAULT_WORLDS, &result);
  pat_move_choice_compare(config, &champion_chooser, &penalty, 830000000ULL,
                          12000, PAT_MOVE_CHOICE_DEFAULT_WORLDS, &result);
  config_destroy(config);
}

// Frozen after the development batch: set to the chosen sign's correction
// (0 means neither was chosen and this test does nothing). An earlier
// development batch was run while the WMP exhaustive-list path dropped
// the PAT term entirely (fixed in move_gen.c alongside this harness), so
// its choice of +2 was withdrawn and the batch rerun.
#define PAT_OVERLAP_STEP3_CONFIRM_C 0.0
// The development batch's decomposition put within-position noise at about
// half of Var(mean) with 30 worlds, and a world costs far less than the
// ~77 exhaustively reranked positions behind each disagreement, so the
// confirmation batch spends more worlds per disagreement.
#define PAT_OVERLAP_STEP3_CONFIRM_WORLDS 200

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
                          48000, PAT_OVERLAP_STEP3_CONFIRM_WORLDS, &result);
  config_destroy(config);
}

// A chooser from a short spec: "champion"; "overlap<c>" (the champion
// reranked by equity - c * narrow_overlap, e.g. overlap+2, overlap-2);
// "degrade<m>" (the champion's best move at least m equity below the
// top); anything else is a PAT file name, loaded with its lexicon tables
// prepared. *owned_out receives a PATWeights to destroy, or NULL.
static PATMoveChooser
pat_move_choice_chooser_from_spec(Config *config, const char *spec,
                                  PATWeights **owned_out) {
  Game *game = config_get_game(config);
  const PATWeights *champion = player_get_pat(game_get_player(game, 0));
  PATMoveChooser chooser = {.label = spec,
                            .pat = champion,
                            .degrade_margin = 0.0,
                            .overlap_correction = 0.0,
                            .pat_scale = 0.0};
  *owned_out = NULL;
  if (strcmp(spec, "champion") == 0) {
    return chooser;
  }
  if (strncmp(spec, "overlap", 7) == 0) {
    chooser.overlap_correction = strtod(spec + 7, NULL);
    assert(chooser.overlap_correction != 0.0);
    return chooser;
  }
  if (strncmp(spec, "degrade", 7) == 0) {
    chooser.degrade_margin = strtod(spec + 7, NULL);
    assert(chooser.degrade_margin > 0.0);
    return chooser;
  }
  if (strncmp(spec, "patscale", 8) == 0) {
    // "patscale<s>" rescales the config champion's PAT term by s;
    // "patscale<s>@<file>" rescales that file's term instead.
    char *end = NULL;
    chooser.pat_scale = strtod(spec + 8, &end);
    assert(chooser.pat_scale > 0.0);
    if (end != NULL && *end == '@') {
      PATWeights *owned = NULL;
      const PATMoveChooser file_chooser =
          pat_move_choice_chooser_from_spec(config, end + 1, &owned);
      chooser.pat = file_chooser.pat;
      *owned_out = owned;
    }
    return chooser;
  }
  ErrorStack *error_stack = error_stack_create();
  PATWeights *pat =
      pat_create(config_get_data_paths(config), spec, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("could not load PAT file '%s'", spec);
  }
  error_stack_destroy(error_stack);
  pat_prepare_hook_flex(pat, player_get_kwg(game_get_player(game, 0)),
                        game_get_ld(game));
  chooser.pat = pat;
  *owned_out = pat;
  return chooser;
}

// Runs one (possibly sharded) comparison from a colon-separated spec:
//   <baseline>:<candidate>:<seed_base>:<num_positions>:<num_worlds>
//       [:<shard>:<num_shards>[:<bag_lo>:<bag_hi>[:<reference_plies>]]]
// so that N shards can run as N processes and be pooled exactly (see
// test/pat_move_choice_shard.sh). Invoked as the test name
// "patmovechoice:<spec>". Choosers: "champion", a PAT file name,
// "overlap<c>", "degrade<m>", "patscale<s>".
void pat_move_choice_run_spec(const char *spec) {
  char buffer[512];
  snprintf(buffer, sizeof(buffer), "%s", spec);
  char *fields[10] = {NULL, NULL, NULL, NULL, NULL,
                      NULL, NULL, NULL, NULL, NULL};
  int num_fields = 0;
  char *save = NULL;
  for (char *tok = strtok_r(buffer, ":", &save); tok && num_fields < 10;
       tok = strtok_r(NULL, ":", &save)) {
    fields[num_fields++] = tok;
  }
  if (num_fields != 5 && num_fields != 7 && num_fields != 9 &&
      num_fields != 10) {
    log_fatal("patmovechoice spec needs 5, 7, 9 or 10 colon-separated "
              "fields: %s",
              spec);
  }
  pat_move_choice_reference_plies = (num_fields == 10) ? atoi(fields[9]) : 2;
  if (pat_move_choice_reference_plies < 2 ||
      pat_move_choice_reference_plies % 2 != 0) {
    log_fatal("reference plies must be a positive even number: %s", spec);
  }
  const uint64_t seed_base = strtoull(fields[2], NULL, 10);
  const int num_positions = atoi(fields[3]);
  const int num_worlds = atoi(fields[4]);
  const int shard = (num_fields >= 7) ? atoi(fields[5]) : 0;
  const int num_shards = (num_fields >= 7) ? atoi(fields[6]) : 1;
  const int bag_lo =
      (num_fields >= 9) ? atoi(fields[7]) : PAT_MOVE_CHOICE_DEFAULT_BAG_LO;
  const int bag_hi =
      (num_fields >= 9) ? atoi(fields[8]) : PAT_MOVE_CHOICE_DEFAULT_BAG_HI;
  Config *config = pat_move_choice_config_create();
  PATWeights *owned_baseline = NULL;
  PATWeights *owned_candidate = NULL;
  const PATMoveChooser baseline =
      pat_move_choice_chooser_from_spec(config, fields[0], &owned_baseline);
  const PATMoveChooser candidate =
      pat_move_choice_chooser_from_spec(config, fields[1], &owned_candidate);
  PATMoveChoiceResult result;
  pat_move_choice_compare_range(config, &baseline, &candidate, seed_base,
                                num_positions, num_worlds, shard, num_shards,
                                bag_lo, bag_hi, &result);
  if (owned_baseline) {
    pat_destroy(owned_baseline);
  }
  if (owned_candidate) {
    pat_destroy(owned_candidate);
  }
  config_destroy(config);
}

// Training/runtime parity diagnostic (Astra's third audit item): for a
// candidate move, compare (a) the runtime PAT term, computed from the
// pre-move context plus the move overlay, against (b) the exact combined
// penalty of the post-move board scored by the same runtime machinery, and
// (c) the training row (pat_extract_features_combined on the post-move
// board with the leave) dotted with the weights. (b) and (c) should agree
// to rounding; (a) differs from (b) wherever the overlay approximates a
// hook or floater the move itself creates (their real cross and extension
// sets do not exist before the move is played), and this reports how often
// and by how much.
void test_pat_train_runtime_parity(void) { pat_train_runtime_parity_for(NULL); }

// pat_name NULL means the config champion.
void pat_train_runtime_parity_for(const char *pat_name) {
  Config *config = pat_move_choice_config_create();
  Game *game = config_get_game(config);
  PATWeights *owned = NULL;
  const PATWeights *champion =
      pat_name ? pat_move_choice_chooser_from_spec(config, pat_name, &owned).pat
               : player_get_pat(game_get_player(game, 0));
  if (owned) {
    player_set_pat(game_get_player(game, 0), owned);
    player_set_pat(game_get_player(game, 1), owned);
  }
  MoveList *setup_list = move_list_create(1);
  MoveList *all_list = move_list_create(PAT_MOVE_CHOICE_MOVE_LIST_CAPACITY);
  PATEvalContext *ctx = malloc_or_die(sizeof(PATEvalContext));
  PATEvalContext *post_ctx = malloc_or_die(sizeof(PATEvalContext));
  double *row = malloc_or_die(sizeof(double) * PAT_NUM_FEATURES);
  int num_moves = 0;
  int num_exact = 0;
  double sum_abs_diff = 0.0;
  double max_abs_diff = 0.0;
  double max_bc_diff = 0.0;
  int num_over_half = 0;
  int num_over_two = 0;
  double sum_runtime = 0.0;
  double sum_post = 0.0;
  for (int attempt = 0; attempt < 300; attempt++) {
    const uint64_t seed = 1200000000ULL + (uint64_t)attempt;
    game_reset(game);
    game_seed(game, seed);
    draw_starting_racks(game);
    const int target_bag = 10 + (int)(seed % 31);
    bool ok = true;
    while (bag_get_letters(game_get_bag(game)) > target_bag) {
      play_move(get_top_equity_move(game, setup_list), game, NULL);
      if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
        ok = false;
        break;
      }
    }
    if (!ok || bag_get_letters(game_get_bag(game)) == 0) {
      continue;
    }
    const int mover_index = game_get_player_on_turn_index(game);
    const Player *mover = game_get_player(game, mover_index);
    const int opponent_rack_size = rack_get_total_letters(
        player_get_rack(game_get_player(game, 1 - mover_index)));
    const int csi = board_get_cross_set_index(
        game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG), mover_index);
    pat_eval_context_load(ctx, champion,
                          board_get_readonly_lanes(game_get_board(game), csi),
                          game_get_ld(game), player_get_rack(mover),
                          PAT_CLASS_MASK_ALL, opponent_rack_size);
    const MoveGenArgs args = {
        .game = game,
        .move_list = all_list,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&args);
    move_list_sort_moves(all_list);
    const int count = move_list_get_count(all_list);
    for (int i = 0; i < count && i < 20; i++) {
      const Move *move = move_list_get_move(all_list, i);
      if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
        continue;
      }
      Rack leave;
      get_leave_for_move(move, game, &leave);
      const double runtime =
          equity_to_double(pat_eval_move_penalty(ctx, move, &leave));
      // play_move itself pushes a backup under BACKUP_MODE_SIMULATION, so
      // the mode is on only around this probe, never during the setup
      // plies (the stack holds MAX_SEARCH_DEPTH entries).
      game_set_backup_mode(game, BACKUP_MODE_SIMULATION);
      play_move_without_drawing_tiles(move, game);
      const Square *post_lanes =
          board_get_readonly_lanes(game_get_board(game), csi);
      pat_eval_context_load(post_ctx, champion, post_lanes, game_get_ld(game),
                            &leave, PAT_CLASS_MASK_ALL, opponent_rack_size);
      const double post =
          equity_to_double(pat_eval_non_placement_penalty(post_ctx));
      pat_extract_features_combined(post_lanes, game_get_ld(game), &leave,
                                    champion, opponent_rack_size, row);
      double dot = 0.0;
      for (int f = 0; f < PAT_NUM_FEATURES; f++) {
        dot += equity_to_double(pat_get_weight(champion, f)) * row[f];
      }
      game_unplay_last_move(game);
      game_set_backup_mode(game, BACKUP_MODE_OFF);
      const double diff = fabs(runtime - post);
      num_moves++;
      sum_runtime += runtime;
      sum_post += post;
      sum_abs_diff += diff;
      if (diff < 1e-9) {
        num_exact++;
      }
      if (diff > max_abs_diff) {
        max_abs_diff = diff;
      }
      if (diff > 0.5) {
        num_over_half++;
      }
      if (diff > 2.0) {
        num_over_two++;
      }
      if (fabs(post - dot) > max_bc_diff) {
        max_bc_diff = fabs(post - dot);
      }
    }
  }
  printf("\n[%s] training/runtime parity: %d moves; runtime == post-board "
         "exact on %d (%.1f%%); mean |diff| %.4f, max %.4f; |diff| > 0.5 on "
         "%d (%.1f%%), > 2.0 on %d (%.1f%%); mean runtime %.4f, mean "
         "post-board %.4f\n",
         pat_name ? pat_name : "champion", num_moves, num_exact,
         100.0 * num_exact / num_moves, sum_abs_diff / num_moves, max_abs_diff,
         num_over_half, 100.0 * num_over_half / num_moves, num_over_two,
         100.0 * num_over_two / num_moves, sum_runtime / num_moves,
         sum_post / num_moves);
  printf("  post-board exact vs training row dot: max |diff| %.6f\n",
         max_bc_diff);
  assert(max_bc_diff < 0.01);
  free(row);
  free(ctx);
  free(post_ctx);
  move_list_destroy(setup_list);
  move_list_destroy(all_list);
  if (owned) {
    pat_destroy(owned);
  }
  config_destroy(config);
}

// Prints what the KLV says a full seven-tile rack is worth (the harness
// leaf adds the mover's leave value for the rack held at the horizon,
// which after drawing is full).
void test_pat_leaf_check(void) {
  Config *config = pat_move_choice_config_create();
  Game *game = config_get_game(config);
  const KLV *klv = player_get_klv(game_get_player(game, 0));
  Rack rack;
  rack_set_dist_size(&rack, ld_get_size(game_get_ld(game)));
  const char *racks[] = {"AEINRST", "AEINRS", "QVWXZ??", "EEEIIOU", "S", "?"};
  for (int i = 0; i < 6; i++) {
    rack_reset(&rack);
    rack_set_to_string(game_get_ld(game), &rack, racks[i]);
    printf("leave value %-8s (%d tiles): %.3f\n", racks[i],
           rack_get_total_letters(&rack),
           equity_to_double(klv_get_leave_value(klv, &rack)));
  }
  config_destroy(config);
}

// Through-table audit (Astra): for a multi-tile run the scan adds the
// endpoint statistic of EACH tile at the same span, i.e. it claims
// sum_over_tiles through_count[tile][span]. The actual quantity for a
// floater run is the number of words of the required length that contain
// the whole run at the required end. This enumerates those words for a
// few runs and prints both, so the proxy's error is visible.
static void pat_audit_count_words(const KWG *kwg, uint32_t node_index,
                                  int depth, int target_length,
                                  const MachineLetter *suffix, int suffix_len,
                                  MachineLetter *word, long *count_end,
                                  long *count_start) {
  if (node_index == 0) {
    return;
  }
  for (uint32_t index = node_index;; index++) {
    const uint32_t node = kwg_node(kwg, index);
    word[depth] = (MachineLetter)kwg_node_tile(node);
    const int length = depth + 1;
    if (length == target_length && kwg_node_accepts(node)) {
      bool ends = true;
      bool starts = true;
      for (int k = 0; k < suffix_len; k++) {
        if (word[length - suffix_len + k] != suffix[k]) {
          ends = false;
        }
        if (word[k] != suffix[k]) {
          starts = false;
        }
      }
      if (ends) {
        (*count_end)++;
      }
      if (starts) {
        (*count_start)++;
      }
    }
    if (length < target_length) {
      pat_audit_count_words(kwg, kwg_node_arc_index(node), length,
                            target_length, suffix, suffix_len, word, count_end,
                            count_start);
    }
    if (kwg_node_is_end(node)) {
      break;
    }
  }
}

void test_pat_through_table_audit(void) {
  Config *config = pat_move_choice_config_create();
  Game *game = config_get_game(config);
  const KWG *kwg = player_get_kwg(game_get_player(game, 0));
  const LetterDistribution *ld = game_get_ld(game);
  const PATWeights *pat = player_get_pat(game_get_player(game, 0));
  const char *runs[] = {"E", "S", "AT", "QI", "ING", "TION", "NARCEIN"};
  printf("\nthrough-table audit: table = sum over run tiles of "
         "8*log2(1+words) at span d+1; actual = 8*log2(1+words of length "
         "d+len(run) with the run at that end)\n");
  for (int r = 0; r < 7; r++) {
    MachineLetter run[8];
    const int run_len = (int)strlen(runs[r]);
    for (int k = 0; k < run_len; k++) {
      char one[2] = {runs[r][k], 0};
      run[k] = ld_hl_to_ml(ld, one);
    }
    for (int d = 2; d <= 6; d += 2) {
      const int span = d + 1;
      int table_pooled = 0;
      int table_first = 0;
      int table_last = 0;
      for (int k = 0; k < run_len; k++) {
        table_pooled += pat_get_through_count(pat, run[k], span);
        table_first += pat_get_through_count_end(pat, 0, run[k], span);
        table_last += pat_get_through_count_end(pat, 1, run[k], span);
      }
      long words_end = 0;
      long words_start = 0;
      MachineLetter word[16];
      const int target_length = d + run_len;
      if (target_length < 16) {
        pat_audit_count_words(kwg, kwg_get_dawg_root_node_index(kwg), 0,
                              target_length, run, run_len, word, &words_end,
                              &words_start);
      }
      printf("  run %-7s d=%d: table pooled %3d (first %3d, last %3d) | actual "
             "8*log2(1+words): run at end %5.1f (%ld words), run at start "
             "%5.1f (%ld words), word length %d\n",
             runs[r], d, table_pooled, table_first, table_last,
             8.0 * log2(1.0 + words_end), words_end,
             8.0 * log2(1.0 + words_start), words_start, target_length);
    }
  }
  config_destroy(config);
}
