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
#include "../src/ent/sim_args.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/cgp.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "pat_overlap_pilot_test.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
      "-sinfer false -pat " PAT_MOVE_CHOICE_CHAMPION);
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

double pat_move_choice_reference_value(const Game *game, const Move *move,
                                       int mover_index,
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
void pat_move_choice_reference_values(const Game *game, const Move *move,
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

// Scratch context for the volatility chooser (single-threaded tests).
static PATEvalContext *pat_move_choice_vol_ctx(void) {
  static PATEvalContext *ctx = NULL;
  if (ctx == NULL) {
    ctx = malloc_or_die(sizeof(PATEvalContext));
  }
  return ctx;
}

enum {
  // Candidates further than this below the top keep their equity: the
  // volatility term is a few points at most.
  PAT_VOL_EQUITY_WINDOW = 25,
  // Finite-difference half-width, in points, for the utility curvature;
  // wide enough to smooth the table's integer margin buckets.
  PAT_VOL_KAPPA_STEP = 15,
};

// The default sim utility (win 1.0, spread 0.5, scale 100) of the mover at
// margin with the opponent on turn and unseen tiles left.
static double pat_move_choice_mover_utility(const WinPct *win_pcts,
                                            double margin, int unseen) {
  const int rounded = (int)lround(margin);
  const double win = 1.0 - win_pct_get(win_pcts, -rounded, (unsigned)unseen);
  return sim_utility_blend(win, double_to_equity(margin), 1.0, 0.5, 100.0);
}

// -U''/U' of that utility at margin, by central differences.
static double pat_move_choice_kappa(const WinPct *win_pcts, double margin,
                                    int unseen) {
  const double step = PAT_VOL_KAPPA_STEP;
  const double lower =
      pat_move_choice_mover_utility(win_pcts, margin - step, unseen);
  const double middle = pat_move_choice_mover_utility(win_pcts, margin, unseen);
  const double upper =
      pat_move_choice_mover_utility(win_pcts, margin + step, unseen);
  const double slope = (upper - lower) / (2.0 * step);
  const double curvature = (upper - 2.0 * middle + lower) / (step * step);
  return slope > 0.0 ? -curvature / slope : 0.0;
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
  Player *mover_player = game_get_player(game, mover_index);
  player_set_pat(mover_player, chooser->pat);
  // See PATMoveChooser.zero_leave's comment: swap in the all-zero KLV
  // player_set_rollout_zero_klv already attached (the caller's
  // responsibility), for the duration of this call's own generate_moves
  // only, then restore the real klv immediately after -- the same
  // swap-and-restore pattern get_top_equity_move uses for rollout
  // forward-play, just applied to this one static choice instead.
  const KLV *real_klv = NULL;
  if (chooser->zero_leave) {
    real_klv = player_get_klv(mover_player);
    player_set_klv(mover_player, player_get_rollout_zero_klv(mover_player));
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
      .pat_disabled_classes_mask = chooser->disabled_classes_mask,
  };
  generate_moves(&args);
  if (chooser->zero_leave) {
    player_set_klv(mover_player, real_klv);
  }
  move_list_sort_moves(move_list);
  const int num_moves = move_list_get_count(move_list);
  assert(num_moves > 0);
  const double best_equity =
      pat_move_choice_move_equity(move_list_get_move(move_list, 0));
  int chosen = 0;
  const bool scaled = chooser->pat_scale != 0.0 && chooser->pat_scale != 1.0;
  if (chooser->vol_pat != NULL) {
    const Player *mover = game_get_player(game, mover_index);
    const Player *opponent = game_get_player(game, 1 - mover_index);
    const int csi = board_get_cross_set_index(
        game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG), mover_index);
    PATEvalContext *ctx = pat_move_choice_vol_ctx();
    pat_eval_context_load(
        ctx, chooser->vol_pat,
        board_get_readonly_lanes(game_get_board(game), csi),
        game_get_ld(game), player_get_rack(mover), PAT_CLASS_MASK_ALL,
        rack_get_total_letters(player_get_rack(opponent)));
    pat_eval_context_set_kwg(ctx, player_get_kwg(mover));
    const double margin_before = equity_to_double(player_get_score(mover) -
                                                  player_get_score(opponent));
    const int bag = bag_get_letters(game_get_bag(game));
    double best_adjusted = 0.0;
    for (int i = 0; i < num_moves; i++) {
      const Move *move = move_list_get_move(move_list, i);
      double adjusted = pat_move_choice_move_equity(move);
      if (best_equity - adjusted > PAT_VOL_EQUITY_WINDOW) {
        break;
      }
      if (move_get_type(move) != GAME_EVENT_PASS) {
        Rack leave;
        get_leave_for_move(move, game, &leave);
        const double sigma2 =
            -equity_to_double(pat_eval_move_penalty(ctx, move, &leave));
        const int drawn =
            move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE
                ? move_get_tiles_played(move)
                : 0;
        const int unseen =
            (bag > drawn ? bag - drawn : 0) + RACK_SIZE;
        const double margin =
            margin_before + equity_to_double(move_get_score(move));
        adjusted -= chooser->vol_scale * 0.5 *
                    pat_move_choice_kappa(chooser->win_pcts, margin, unseen) *
                    sigma2;
      }
      if (i == 0 || adjusted > best_adjusted) {
        best_adjusted = adjusted;
        chosen = i;
      }
    }
  } else if (chooser->degrade_margin <= 0.0 &&
             chooser->overlap_correction == 0.0 && !scaled) {
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
    pat_eval_context_set_kwg(ctx, player_get_kwg(mover));
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

// TWS-only PAT: every premium class but PAT_PREMIUM_TWS treated as
// zero-weighted (PAT_CLASS_MASK_TWS_ONLY, see pat.h), the champion's own
// trained TWS weights otherwise unchanged -- not a retrained model, just a
// restricted read of the same file. pat.h's own comment names this "a fast
// mode (e.g. TWS only, for rollouts)"; this measures the static (non-
// rollout) side of that tradeoff: how much of the champion's full static
// advantage over no PAT at all a TWS-only read alone accounts for, and how
// much is left on the table relative to full PAT.
void test_pat_move_choice_tws_only(void) {
  Config *config = pat_move_choice_config_create();
  Game *game = config_get_game(config);
  const PATWeights *champion = player_get_pat(game_get_player(game, 0));
  const PATMoveChooser champion_chooser = {.label = "champion",
                                           .pat = champion,
                                           .degrade_margin = 0.0,
                                           .overlap_correction = 0.0};
  PATWeights *zeroed = pat_create_zeroed("zeroed");
  pat_prepare_hook_flex(zeroed, player_get_kwg(game_get_player(game, 0)),
                        game_get_ld(game));
  const PATMoveChooser zeroed_chooser = {.label = "all PAT weights zero",
                                         .pat = zeroed,
                                         .degrade_margin = 0.0,
                                         .overlap_correction = 0.0};
  const PATMoveChooser tws_only_chooser = {
      .label = "champion, TWS classes only",
      .pat = champion,
      .degrade_margin = 0.0,
      .overlap_correction = 0.0,
      .disabled_classes_mask =
          PAT_CLASS_MASK_ALL & ~(uint32_t)PAT_CLASS_MASK_TWS_ONLY};
  // Same seed_base/position count for all three legs of the triangle
  // (champion, zeroed, TWS-only) so they visit the identical 4000
  // positions: the two per-decision (unconditional) effects below are then
  // exactly additive -- (tws_only-zeroed) + (champion-tws_only) ==
  // (champion-zeroed) -- rather than three independently-sampled numbers
  // that need not agree.
  PATMoveChoiceResult result;
  printf("\nchampion (full PAT) vs no PAT:\n");
  pat_move_choice_compare(config, &zeroed_chooser, &champion_chooser,
                          975000000ULL, 300000, PAT_MOVE_CHOICE_DEFAULT_WORLDS,
                          &result);
  printf("\nTWS-only vs no PAT:\n");
  pat_move_choice_compare(config, &zeroed_chooser, &tws_only_chooser,
                          975000000ULL, 300000, PAT_MOVE_CHOICE_DEFAULT_WORLDS,
                          &result);
  printf("\nchampion (full PAT) vs TWS-only:\n");
  pat_move_choice_compare(config, &champion_chooser, &tws_only_chooser,
                          975000000ULL, 300000, PAT_MOVE_CHOICE_DEFAULT_WORLDS,
                          &result);
  pat_destroy(zeroed);
  config_destroy(config);
}

// Same triangle as test_pat_move_choice_tws_only, but against
// pat_ridgefix_champion_v5 (the full retrain under the variance-scaled
// ridge fix in pat_gen.c -- see notes/pat_champion_recipe.md's provenance
// header in that file) instead of the shipped pat_dls_champion_v2, to see
// whether fixing the regularization closes the gap that motivated this
// file: pat_dls_champion_v2's TWS-only read beat the full champion by
// +0.185/disagreement (95% CI [0.149, 0.221], N=1999990) in the
// production-scale run this triangle's own smaller version predicted.
void test_pat_move_choice_tws_only_ridgefix(void) {
  Config *config = pat_move_choice_config_create();
  Game *game = config_get_game(config);
  ErrorStack *error_stack = error_stack_create();
  PATWeights *champion = pat_create(config_get_data_paths(config),
                                    "pat_ridgefix_champion_v5", error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  pat_prepare_hook_flex(champion, player_get_kwg(game_get_player(game, 0)),
                        game_get_ld(game));
  const PATMoveChooser champion_chooser = {.label = "ridgefix champion",
                                           .pat = champion,
                                           .degrade_margin = 0.0,
                                           .overlap_correction = 0.0};
  PATWeights *zeroed = pat_create_zeroed("zeroed");
  pat_prepare_hook_flex(zeroed, player_get_kwg(game_get_player(game, 0)),
                        game_get_ld(game));
  const PATMoveChooser zeroed_chooser = {.label = "all PAT weights zero",
                                         .pat = zeroed,
                                         .degrade_margin = 0.0,
                                         .overlap_correction = 0.0};
  const PATMoveChooser tws_only_chooser = {
      .label = "ridgefix champion, TWS classes only",
      .pat = champion,
      .degrade_margin = 0.0,
      .overlap_correction = 0.0,
      .disabled_classes_mask =
          PAT_CLASS_MASK_ALL & ~(uint32_t)PAT_CLASS_MASK_TWS_ONLY};
  PATMoveChoiceResult result;
  printf("\nridgefix champion (full PAT) vs no PAT:\n");
  pat_move_choice_compare(config, &zeroed_chooser, &champion_chooser,
                          975000000ULL, 300000, PAT_MOVE_CHOICE_DEFAULT_WORLDS,
                          &result);
  printf("\nridgefix TWS-only vs no PAT:\n");
  pat_move_choice_compare(config, &zeroed_chooser, &tws_only_chooser,
                          975000000ULL, 300000, PAT_MOVE_CHOICE_DEFAULT_WORLDS,
                          &result);
  printf("\nridgefix champion (full PAT) vs ridgefix TWS-only:\n");
  pat_move_choice_compare(config, &champion_chooser, &tws_only_chooser,
                          975000000ULL, 300000, PAT_MOVE_CHOICE_DEFAULT_WORLDS,
                          &result);
  pat_destroy(zeroed);
  pat_destroy(champion);
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

// A chooser from a short spec: "champion"; "twsonly" (the champion's own
// weights, read with every PAT premium class but TWS masked off --
// PAT_CLASS_MASK_TWS_ONLY, see pat.h and test_pat_move_choice_tws_only);
// "overlap<c>" (the champion reranked by equity - c * narrow_overlap, e.g.
// overlap+2, overlap-2); "degrade<m>" (the champion's best move at least m
// equity below the top); anything else is a PAT file name, loaded with its
// lexicon tables prepared. *owned_out receives a PATWeights to destroy, or
// NULL.
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
  if (strcmp(spec, "twsonly") == 0) {
    chooser.disabled_classes_mask =
        PAT_CLASS_MASK_ALL & ~(uint32_t)PAT_CLASS_MASK_TWS_ONLY;
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
    pat_eval_context_set_kwg(ctx, player_get_kwg(mover));
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

// Static (non-rollout) per-move-choice comparison: real leave value vs
// zeroed leave value, same PAT champion for both choosers throughout, so
// only leave toggles. Directly comparable to a PAT-vs-no-PAT
// pat_move_choice_run_spec run -- same disagreement-then-2-ply-reference-
// oracle methodology (pat_move_choice_reference_value(s), reused
// unchanged via pat_move_choice_compare), just a different single axis.
// "seed:num_positions:num_worlds" -- simpler than pat_move_choice_run_spec
// since there is no baseline/candidate PAT file to name; the champion
// config's own PAT stays on both choosers the whole time.
void pat_move_choice_run_leave_spec(const char *spec) {
  char buffer[512];
  snprintf(buffer, sizeof(buffer), "%s", spec);
  char *fields[3] = {NULL, NULL, NULL};
  int num_fields = 0;
  char *save = NULL;
  for (char *tok = strtok_r(buffer, ":", &save); tok && num_fields < 3;
       tok = strtok_r(NULL, ":", &save)) {
    fields[num_fields++] = tok;
  }
  if (num_fields != 3) {
    log_fatal("leavemovechoice spec needs 3 colon-separated fields "
              "(seed:num_positions:num_worlds): %s",
              spec);
  }
  const uint64_t seed_base = strtoull(fields[0], NULL, 10);
  const int num_positions = atoi(fields[1]);
  const int num_worlds = atoi(fields[2]);

  Config *config = pat_move_choice_config_create();
  Player *player0 = game_get_player(config_get_game(config), 0);
  Player *player1 = game_get_player(config_get_game(config), 1);
  const PATWeights *reference_pat = player_get_pat(player0);
  assert(reference_pat);

  // The all-zero KLV both choosers' zero_leave path swaps in when active
  // (see pat_move_choice_choose): built once from the real KLV already
  // loaded (same kwg, same leave count, all values zeroed), attached to
  // both players since either can be on turn at a sampled position.
  const KLV *real_klv0 = player_get_klv(player0);
  assert(real_klv0);
  KLV *zero_klv = klv_create_zeroed_from_kwg(
      (KWG *)klv_get_kwg(real_klv0), (int)klv_get_number_of_leaves(real_klv0),
      "leave_move_choice_zero_klv");
  player_set_rollout_zero_klv(player0, zero_klv);
  player_set_rollout_zero_klv(player1, zero_klv);

  const PATMoveChooser baseline = {.label = "real_leave",
                                   .pat = reference_pat,
                                   .degrade_margin = 0.0,
                                   .overlap_correction = 0.0,
                                   .pat_scale = 0.0,
                                   .zero_leave = false};
  const PATMoveChooser candidate = {.label = "zero_leave",
                                    .pat = reference_pat,
                                    .degrade_margin = 0.0,
                                    .overlap_correction = 0.0,
                                    .pat_scale = 0.0,
                                    .zero_leave = true};
  PATMoveChoiceResult result;
  pat_move_choice_compare(config, &baseline, &candidate, seed_base,
                          num_positions, num_worlds, &result);
  config_destroy(config);
}

// One-off CSW24 diagnostic. Neutral complete-game oracle: after either
// candidate, both sides finish under production no-PAT static play. The
// opponent rack is resampled with paired seeds in each world.
static double pat_debug_complete_value(const Game *position, const Move *move,
                                       int mover_index, uint64_t world_seed,
                                       MoveList *reply_list) {
  Game *continuation = game_duplicate(position);
  game_seed(continuation, world_seed);
  player_set_pat(game_get_player(continuation, 0), NULL);
  player_set_pat(game_get_player(continuation, 1), NULL);
  // Randomize the hidden rack while it can exchange with the full bag.
  // Doing this after a candidate empties the bag freezes the source-game
  // opponent rack and falsely makes the continuation deterministic.
  set_random_rack(continuation, 1 - mover_index, NULL);
  play_move(move, continuation, NULL);
  while (game_get_game_end_reason(continuation) == GAME_END_REASON_NONE) {
    const Move *reply = get_top_equity_move(continuation, reply_list);
    play_move(reply, continuation, NULL);
  }
  const double spread = equity_to_double(
      player_get_score(game_get_player(continuation, mover_index)) -
      player_get_score(game_get_player(continuation, 1 - mover_index)));
  game_destroy(continuation);
  return spread;
}


enum { PAT_DEBUG_FULL_GAME_BAG = 21, PAT_DEBUG_HORIZON_PLIES = 4 };

typedef struct {
  double spread;
  double horizon_leave;
  double win_pct;
  double utility;
  int horizon_unseen;
  int plies;
  bool ended;
} PATDebugOracleValue;

static PATDebugOracleValue pat_debug_measure_horizon(
    const Game *continuation, int mover_index, const WinPct *win_pcts,
    const Rack last_leave[2], const bool has_leave[2], int plies) {
  PATDebugOracleValue value = {0};
  const Player *mover = game_get_player(continuation, mover_index);
  const Player *opponent = game_get_player(continuation, 1 - mover_index);
  value.spread = equity_to_double(player_get_score(mover) -
                                  player_get_score(opponent));
  value.plies = plies;
  value.ended = game_get_game_end_reason(continuation) != GAME_END_REASON_NONE;
  if (value.ended) {
    value.win_pct = value.spread > 0 ? 1.0 :
                    value.spread < 0 ? 0.0 : 0.5;
  } else {
    if (has_leave[mover_index]) {
      value.horizon_leave += equity_to_double(klv_get_leave_value(
          player_get_klv(mover), &last_leave[mover_index]));
    }
    if (has_leave[1 - mover_index]) {
      value.horizon_leave -= equity_to_double(klv_get_leave_value(
          player_get_klv(opponent), &last_leave[1 - mover_index]));
    }
    value.horizon_unseen = bag_get_letters(game_get_bag(continuation)) +
        rack_get_total_letters(player_get_rack(opponent));
    assert(value.horizon_unseen > 0);
    const double adjusted_spread = value.spread + value.horizon_leave;
    int rounded = (int)(adjusted_spread + 0.5 - (adjusted_spread < 0.0));
    const bool opponent_on_turn =
        game_get_player_on_turn_index(continuation) != mover_index;
    if (opponent_on_turn) {
      rounded = -rounded;
    }
    value.win_pct = win_pct_get(win_pcts, rounded, value.horizon_unseen);
    if (opponent_on_turn) {
      value.win_pct = 1.0 - value.win_pct;
    }
  }
  value.utility = sim_utility_blend(
      value.win_pct, double_to_equity(value.spread), 1.0, 0.5, 100.0);
  return value;
}

static PATDebugOracleValue pat_debug_hybrid_value(
    const Game *position, const Move *candidate, int mover_index,
    uint64_t world_seed, MoveList *reply_list, const WinPct *win_pcts) {
  Game *continuation = game_duplicate(position);
  game_seed(continuation, world_seed);
  player_set_pat(game_get_player(continuation, 0), NULL);
  player_set_pat(game_get_player(continuation, 1), NULL);
  set_random_rack(continuation, 1 - mover_index, NULL);
  Rack last_leave[2];
  bool has_leave[2] = {false, false};
  for (int player_index = 0; player_index < 2; player_index++) {
    rack_set_dist_size(&last_leave[player_index],
                       ld_get_size(game_get_ld(continuation)));
    rack_reset(&last_leave[player_index]);
  }
  const bool full_game =
      bag_get_letters(game_get_bag(position)) <= PAT_DEBUG_FULL_GAME_BAG;
  int ply = 0;
  while (game_get_game_end_reason(continuation) == GAME_END_REASON_NONE &&
         (full_game || ply < PAT_DEBUG_HORIZON_PLIES)) {
    const int actor = game_get_player_on_turn_index(continuation);
    const Move *selected = ply == 0 ? candidate :
                           get_top_equity_move(continuation, reply_list);
    play_move(selected, continuation, &last_leave[actor]);
    has_leave[actor] = true;
    ply++;
  }
  const PATDebugOracleValue value = pat_debug_measure_horizon(
      continuation, mover_index, win_pcts, last_leave, has_leave, ply);
  game_destroy(continuation);
  return value;
}

static int pat_debug_find_rank(const MoveList *move_list, const Move *target) {
  const int count = move_list_get_count(move_list);
  for (int index = 0; index < count; index++) {
    if (compare_moves_without_equity(move_list_get_move(move_list, index),
                                     target, true) == -1) {
      return index + 1;
    }
  }
  return 0;
}

static char *pat_debug_rankings(const Game *game, const MoveList *move_list) {
  StringBuilder *sb = string_builder_create();
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const int count = move_list_get_count(move_list);
  int emitted = 0;
  for (int index = 0; index < count && index < 10; index++) {
    const Move *move = move_list_get_move(move_list, index);
    if (move_get_equity(move) == EQUITY_PASS_VALUE) continue;
    StringBuilder *move_sb = string_builder_create();
    string_builder_add_move(move_sb, board, move, ld, true);
    char *move_text = string_builder_dump(move_sb, NULL);
    string_builder_destroy(move_sb);
    if (emitted++ > 0) {
      string_builder_add_string(sb, ";;");
    }
    string_builder_add_formatted_string(
        sb, "%d|%s|%.3f", index + 1, move_text,
        equity_to_double(move_get_equity(move)));
    free(move_text);
  }
  char *result = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return result;
}

static double pat_debug_elapsed(const struct timespec *start) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (double)(now.tv_sec - start->tv_sec) +
         (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

void pat_move_choice_debug_csw24(void) {
  const int duration_seconds = 1800;
  const int worlds = 32;
  const uint64_t seed_base = 8600000000ULL;
  Config *config = config_create_or_die(
      "set -lex CSW24 -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -sinfer false -wmp true -rit true -ritmmap true "
      "-wit true -pat hookscore_x");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  const PATWeights *champion = player_get_pat(game_get_player(game, 0));
  assert(champion);
  const PATMoveChooser prod = {.label = "prod_no_pat", .pat = NULL};
  const PATMoveChooser x = {.label = "hookscore_x", .pat = champion};
  MoveList *setup_list = move_list_create(1);
  MoveList *choice_list = move_list_create(PAT_MOVE_CHOICE_MOVE_LIST_CAPACITY);
  MoveList *reply_list = move_list_create(1);
  FILE *out = fopen("/tmp/pat_csw24_oracle.tsv", "w");
  assert(out);
  fprintf(out, "seed\tbag\tmover\tcgp\tprod_move\tprod_static_equity\tx_"
               "move\tx_static_equity\tprod_top10\tx_top10\tprod_rank_of_x\tx_"
               "rank_of_prod\tprod_equity_of_x\tx_equity_of_prod\toracle_x_"
               "minus_prod\toracle_se\tworlds\n");
  fflush(out);
  int games = 0;
  int positions = 0;
  int disagreements = 0;
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  double last_progress = 0.0;
  for (uint64_t attempt = 0; pat_debug_elapsed(&start) < duration_seconds;
       attempt++) {
    const uint64_t seed = seed_base + attempt;
    game_reset(game);
    game_seed(game, seed);
    draw_starting_racks(game);
    player_set_pat(game_get_player(game, 0), NULL);
    player_set_pat(game_get_player(game, 1), NULL);
    const int target_bag = 10 + (int)(seed % 60ULL);
    while (game_get_game_end_reason(game) == GAME_END_REASON_NONE &&
           bag_get_letters(game_get_bag(game)) > target_bag) {
      const Move *setup = get_top_equity_move(game, setup_list);
      play_move(setup, game, NULL);
    }
    if (game_get_game_end_reason(game) == GAME_END_REASON_NONE &&
        bag_get_letters(game_get_bag(game)) > 0 &&
        board_get_cross_sets_valid(game_get_board(game)) &&
        !board_get_transposed(game_get_board(game))) {
      positions++;
      const int mover_index = game_get_player_on_turn_index(game);
      Move move_prod;
      Move move_x;
      pat_move_choice_choose(game, mover_index, &prod, choice_list, &move_prod);
      char *prod_top10 = pat_debug_rankings(game, choice_list);
      pat_move_choice_choose(game, mover_index, &x, choice_list, &move_x);
      char *x_top10 = pat_debug_rankings(game, choice_list);
      const int x_rank_of_prod = pat_debug_find_rank(choice_list, &move_prod);
      double x_equity_of_prod = 0.0;
      if (x_rank_of_prod > 0) {
        x_equity_of_prod = equity_to_double(move_get_equity(
            move_list_get_move(choice_list, x_rank_of_prod - 1)));
      }
      Move ignored_prod;
      pat_move_choice_choose(game, mover_index, &prod, choice_list,
                             &ignored_prod);
      const int prod_rank_of_x = pat_debug_find_rank(choice_list, &move_x);
      double prod_equity_of_x = 0.0;
      if (prod_rank_of_x > 0) {
        prod_equity_of_x = equity_to_double(move_get_equity(
            move_list_get_move(choice_list, prod_rank_of_x - 1)));
      }
      player_set_pat(game_get_player(game, mover_index), NULL);
      if (move_get_type(&move_prod) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
          move_get_type(&move_x) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
          compare_moves_without_equity(&move_prod, &move_x, true) != -1) {
        disagreements++;
        char *cgp = game_get_cgp(game, true);
        const Board *board = game_get_board(game);
        const LetterDistribution *ld = game_get_ld(game);
        StringBuilder *sb = string_builder_create();
        string_builder_add_move(sb, board, &move_prod, ld, true);
        char *prod_text = string_builder_dump(sb, NULL);
        string_builder_destroy(sb);
        sb = string_builder_create();
        string_builder_add_move(sb, board, &move_x, ld, true);
        char *x_text = string_builder_dump(sb, NULL);
        string_builder_destroy(sb);
        double sum = 0.0;
        double sum_sq = 0.0;
        for (int world = 0; world < worlds; world++) {
          const uint64_t world_seed =
              seed + 500000000ULL + (uint64_t)world * 1000003ULL;
          const double prod_value = pat_debug_complete_value(
              game, &move_prod, mover_index, world_seed, reply_list);
          const double x_value = pat_debug_complete_value(
              game, &move_x, mover_index, world_seed, reply_list);
          const double difference = x_value - prod_value;
          sum += difference;
          sum_sq += difference * difference;
        }
        const double mean = sum / worlds;
        const double variance =
            fmax(0.0, (sum_sq - sum * sum / worlds) / (worlds - 1));
        fprintf(out,
                "%llu\t%d\t%d\t%s\t%s\t%.3f\t%s\t%.3f\t%s\t%s\t%d\t%d\t%.3f\t%."
                "3f\t%.6f\t%.6f\t%d\n",
                (unsigned long long)seed, bag_get_letters(game_get_bag(game)),
                mover_index, cgp, prod_text,
                equity_to_double(move_get_equity(&move_prod)), x_text,
                equity_to_double(move_get_equity(&move_x)), prod_top10, x_top10,
                prod_rank_of_x, x_rank_of_prod, prod_equity_of_x,
                x_equity_of_prod, mean, sqrt(variance / worlds), worlds);
        fflush(out);
        free(cgp);
        free(prod_text);
        free(x_text);
      }
      free(prod_top10);
      free(x_top10);
    }
    // Finish each sampled game under the production static policy.
    player_set_pat(game_get_player(game, 0), NULL);
    player_set_pat(game_get_player(game, 1), NULL);
    while (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
      const Move *reply = get_top_equity_move(game, setup_list);
      play_move(reply, game, NULL);
    }
    games++;
    const double elapsed = pat_debug_elapsed(&start);
    if (elapsed - last_progress >= 60.0) {
      last_progress = elapsed;
      fprintf(stderr,
              "[pat-debug-csw24] elapsed=%.0fs games=%d positions=%d "
              "disagreements=%d\n",
              elapsed, games, positions, disagreements);
      fflush(stderr);
    }
  }
  fprintf(stderr,
          "[pat-debug-csw24] FINAL games=%d positions=%d disagreements=%d\n",
          games, positions, disagreements);
  fclose(out);
  move_list_destroy(reply_list);
  move_list_destroy(choice_list);
  move_list_destroy(setup_list);
  config_destroy(config);
}


// Detailed four-ply or complete-game outcomes for positions on the local
// CSW24 debug page. This uses the same worlds and continuation as confirmation.
enum { PAT_DEBUG_AUDIT_MAX_PLIES = 100 };

typedef struct {
  int reached[PAT_DEBUG_AUDIT_MAX_PLIES];
  int bingos[PAT_DEBUG_AUDIT_MAX_PLIES];
  double scores[PAT_DEBUG_AUDIT_MAX_PLIES];
  double adjustments[PAT_DEBUG_AUDIT_MAX_PLIES];
  double bag_before[PAT_DEBUG_AUDIT_MAX_PLIES];
  int wins;
  int draws;
  int losses;
  double spread_sum;
  double spread_sq_sum;
  double utility_sum;
  double win_pct_sum;
  double horizon_leave_sum;
  double horizon_unseen_sum;
  double terminal_adjustment_sum;
  int completed_worlds;
  int total_plies;
} PATDebugAudit;

static void pat_debug_audit_world(const Game *position, const Move *candidate,
                                  int mover_index, uint64_t world_seed,
                                  MoveList *reply_list, const WinPct *win_pcts,
                                  PATDebugAudit *audit, const char *mode,
                                  FILE *trace) {
  const PATDebugOracleValue value = pat_debug_hybrid_value(
      position, candidate, mover_index, world_seed, reply_list, win_pcts);
  Game *continuation = game_duplicate(position);
  game_seed(continuation, world_seed);
  player_set_pat(game_get_player(continuation, 0), NULL);
  player_set_pat(game_get_player(continuation, 1), NULL);
  set_random_rack(continuation, 1 - mover_index, NULL);
  const bool full_game =
      bag_get_letters(game_get_bag(position)) <= PAT_DEBUG_FULL_GAME_BAG;
  int ply = 0;
  while (game_get_game_end_reason(continuation) == GAME_END_REASON_NONE &&
         (full_game || ply < PAT_DEBUG_HORIZON_PLIES)) {
    assert(ply < PAT_DEBUG_AUDIT_MAX_PLIES);
    const int actor = game_get_player_on_turn_index(continuation);
    const Move *selected = ply == 0 ? candidate
                                    : get_top_equity_move(continuation, reply_list);
    const double score = equity_to_double(move_get_score(selected));
    const Equity raw_equity = move_get_equity(selected);
  const double adjustment = raw_equity == EQUITY_PASS_VALUE
      ? 0.0 : equity_to_double(raw_equity) - score;
    const int bag_before = bag_get_letters(game_get_bag(continuation));
    const double mover_before = equity_to_double(
        player_get_score(game_get_player(continuation, mover_index)));
    const double opponent_before = equity_to_double(
        player_get_score(game_get_player(continuation, 1 - mover_index)));
    char *move_text = NULL;
    if (trace != NULL) {
      StringBuilder *sb = string_builder_create();
      string_builder_add_move(sb, game_get_board(continuation), selected,
                              game_get_ld(continuation), true);
      move_text = string_builder_dump(sb, NULL);
      string_builder_destroy(sb);
    }
    audit->reached[ply]++;
    audit->scores[ply] += score;
    audit->adjustments[ply] += adjustment;
    audit->bag_before[ply] += bag_before;
    if (move_get_type(selected) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
        move_get_tiles_played(selected) == RACK_SIZE) {
      audit->bingos[ply]++;
    }
    play_move(selected, continuation, NULL);
    const double mover_after = equity_to_double(
        player_get_score(game_get_player(continuation, mover_index)));
    const double opponent_after = equity_to_double(
        player_get_score(game_get_player(continuation, 1 - mover_index)));
    const double signed_score = actor == mover_index ? score : -score;
    const double extra_spread =
        (mover_after - opponent_after) - (mover_before - opponent_before) -
        signed_score;
    if (game_get_game_end_reason(continuation) != GAME_END_REASON_NONE) {
      audit->terminal_adjustment_sum += extra_spread;
    }
    if (trace != NULL) {
      fprintf(trace, "%s\t%d\t%s\t%s\t%.3f\t%.3f\t%d\t%d\t%.3f\t%.3f\t%.3f\n",
              mode, ply, actor == mover_index ? "mover" : "opponent",
              move_text, score, adjustment, bag_before,
              bag_get_letters(game_get_bag(continuation)), mover_after,
              opponent_after, extra_spread);
      free(move_text);
    }
    ply++;
  }
  assert(ply == value.plies);
  audit->spread_sum += value.spread;
  audit->spread_sq_sum += value.spread * value.spread;
  audit->utility_sum += value.utility;
  audit->win_pct_sum += value.win_pct;
  audit->horizon_leave_sum += value.horizon_leave;
  audit->horizon_unseen_sum += value.horizon_unseen;
  audit->total_plies += value.plies;
  if (value.ended) {
    audit->completed_worlds++;
    if (value.spread > 0) {
      audit->wins++;
    } else if (value.spread < 0) {
      audit->losses++;
    } else {
      audit->draws++;
    }
  }
  game_destroy(continuation);
}

static void pat_debug_audit_rank_row(FILE *ranks, uint64_t seed,
                                      const char *mode, int rank,
                                      const Game *game, const Move *move,
                                      double prod_equity, double x_equity) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_move(sb, game_get_board(game), move, game_get_ld(game),
                          true);
  char *move_text = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  const double score = equity_to_double(move_get_score(move));
  fprintf(ranks, "%llu\t%s\t%d\t%s\t%.3f\t%.3f\t%.3f\t%.3f\n",
          (unsigned long long)seed, mode, rank, move_text, score,
          prod_equity - score, x_equity - prod_equity,
          strcmp(mode, "x") == 0 ? x_equity : prod_equity);
  free(move_text);
}

static void pat_debug_draw_exact_opening(Game *game, uint64_t seed) {
  static FILE *racks = NULL;
  if (!racks) {
    racks = fopen("/tmp/pat_csw24_all_opening_racks.txt", "r");
    assert(racks);
  }
  assert(seed >= 8800000000ULL && seed < 8803199724ULL);
  const uint64_t index = seed - 8800000000ULL;
  assert(fseek(racks, (long)(index * 8ULL), SEEK_SET) == 0);
  char rack_text[64];
  assert(fscanf(racks, "%63s", rack_text) == 1);
  MachineLetter letters[RACK_SIZE];
  assert(ld_str_to_mls(game_get_ld(game), rack_text, false, letters,
                       RACK_SIZE) == RACK_SIZE);
  Rack *rack = player_get_rack(game_get_player(game, 0));
  for (int i = 0; i < RACK_SIZE; i++) {
    assert(bag_draw_letter(game_get_bag(game), letters[i], 0));
    rack_add_letter(rack, letters[i]);
  }
  draw_to_full_rack(game, 1);
}

void pat_move_choice_debug_audit_csw24(void) {
  const int worlds = 1024;
  Config *config = config_create_or_die(
      "set -lex CSW24 -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -sinfer false -wmp true -rit true -ritmmap true "
      "-wit true -winpct winpct -pat hookscore_x");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  const PATWeights *champion = player_get_pat(game_get_player(game, 0));
  const WinPct *win_pcts = config_get_win_pcts(config);
  assert(champion && win_pcts);
  assert(config_get_utility_w_winpct(config) == 1.0);
  assert(config_get_utility_w_spread(config) == 0.5);
  assert(config_get_utility_spread_scale(config) == 100.0);
  const PATMoveChooser prod = {.label = "prod_no_pat", .pat = NULL};
  const PATMoveChooser x = {.label = "hookscore_x", .pat = champion};
  MoveList *setup_list = move_list_create(1);
  MoveList *choice_list = move_list_create(PAT_MOVE_CHOICE_MOVE_LIST_CAPACITY);
  MoveList *reply_list = move_list_create(1);
  FILE *input = fopen("/tmp/pat_csw24_audit_seeds.txt", "r");
  FILE *out = fopen("/tmp/pat_csw24_audit.tsv", "w");
  FILE *plies = fopen("/tmp/pat_csw24_audit_plies.tsv", "w");
  FILE *ranks = fopen("/tmp/pat_csw24_audit_ranks.tsv", "w");
  FILE *trace = fopen("/tmp/pat_csw24_8600071200_trace.tsv", "w");
  assert(input && out && plies && ranks && trace);
  fprintf(out, "seed\tmode\twins\tdraws\tlosses\tmean_spread\tspread_se\tmean_utility\tmean_win_pct\tmean_horizon_leave\tmean_horizon_unseen\tmean_plies\tmean_terminal_adjustment\tcompleted_worlds\tmethod\tworlds\n");
  fprintf(plies, "seed\tmode\tply\treached\tmean_score\tbingo_rate\tmean_equity_minus_score\tmean_bag_before\n");
  fprintf(ranks, "seed\tmode\trank\tmove\tscore\tleave\tpositional_adjustment\tequity\n");
  fprintf(trace, "mode\tply\tactor\tmove\tscore\tequity_minus_score\tbag_before\tbag_after\tmover_score_after\topponent_score_after\tterminal_spread_adjustment\n");
  unsigned long long seed_input;
  int completed = 0;
  while (fscanf(input, "%llu", &seed_input) == 1) {
    const uint64_t seed = (uint64_t)seed_input;
    const int target_bag = 10 + (int)(seed % 60ULL);
    game_reset(game);
    game_seed(game, seed);
    if (getenv("PAT_DEBUG_OPENING") && seed >= 8800000000ULL) {
      pat_debug_draw_exact_opening(game, seed);
    } else {
      draw_starting_racks(game);
    }
    player_set_pat(game_get_player(game, 0), NULL);
    player_set_pat(game_get_player(game, 1), NULL);
    const bool opening = getenv("PAT_DEBUG_OPENING") != NULL;
    while (!opening && game_get_game_end_reason(game) == GAME_END_REASON_NONE &&
           bag_get_letters(game_get_bag(game)) > target_bag) {
      play_move(get_top_equity_move(game, setup_list), game, NULL);
    }
    assert(game_get_game_end_reason(game) == GAME_END_REASON_NONE);
    const int mover_index = game_get_player_on_turn_index(game);
    Move move_prod;
    Move move_x;
    pat_move_choice_choose(game, mover_index, &prod, choice_list, &move_prod);
    pat_move_choice_choose(game, mover_index, &x, choice_list, &move_x);
    const int x_count = move_list_get_count(choice_list) < 10
                            ? move_list_get_count(choice_list) : 10;
    Move x_top[10];
    for (int rank_index = 0; rank_index < x_count; rank_index++) {
      x_top[rank_index] = *move_list_get_move(choice_list, rank_index);
    }
    const int x_rank_of_prod = pat_debug_find_rank(choice_list, &move_prod);
    const double x_equity_of_prod = x_rank_of_prod > 0
        ? equity_to_double(move_get_equity(
              move_list_get_move(choice_list, x_rank_of_prod - 1))) : 0.0;
    Move ignored_prod;
    pat_move_choice_choose(game, mover_index, &prod, choice_list,
                           &ignored_prod);
    const int prod_count = move_list_get_count(choice_list) < 10
                               ? move_list_get_count(choice_list) : 10;
    for (int rank_index = 0; rank_index < prod_count; rank_index++) {
      const Move *ranked = move_list_get_move(choice_list, rank_index);
      const double equity = equity_to_double(move_get_equity(ranked));
      pat_debug_audit_rank_row(ranks, seed, "prod", rank_index + 1,
                               game, ranked, equity, equity);
    }
    const int prod_rank_of_x = pat_debug_find_rank(choice_list, &move_x);
    if (prod_rank_of_x > 10) {
      const Move *ranked = move_list_get_move(choice_list, prod_rank_of_x - 1);
      const double equity = equity_to_double(move_get_equity(ranked));
      pat_debug_audit_rank_row(ranks, seed, "prod", prod_rank_of_x,
                               game, ranked, equity, equity);
    }
    for (int rank_index = 0; rank_index < x_count; rank_index++) {
      const Move *ranked = &x_top[rank_index];
      const int prod_rank = pat_debug_find_rank(choice_list, ranked);
      assert(prod_rank > 0);
      const double prod_equity = equity_to_double(move_get_equity(
          move_list_get_move(choice_list, prod_rank - 1)));
      pat_debug_audit_rank_row(ranks, seed, "x", rank_index + 1,
                               game, ranked, prod_equity,
                               equity_to_double(move_get_equity(ranked)));
    }
    if (x_rank_of_prod > 10) {
      pat_debug_audit_rank_row(ranks, seed, "x", x_rank_of_prod,
                               game, &move_prod,
                               equity_to_double(move_get_equity(&move_prod)),
                               x_equity_of_prod);
    }
    player_set_pat(game_get_player(game, mover_index), NULL);
    PATDebugAudit prod_audit = {0};
    PATDebugAudit x_audit = {0};
    for (int world = 0; world < worlds; world++) {
      const uint64_t world_seed =
          seed + 700000000ULL + (uint64_t)world * 1000003ULL;
      FILE *world_trace = seed == 8600071200ULL && world == 0 ? trace : NULL;
      pat_debug_audit_world(game, &move_prod, mover_index, world_seed,
                            reply_list, win_pcts, &prod_audit, "prod", world_trace);
      pat_debug_audit_world(game, &move_x, mover_index, world_seed,
                            reply_list, win_pcts, &x_audit, "x", world_trace);
    }
    const PATDebugAudit *audits[2] = {&prod_audit, &x_audit};
    const char *modes[2] = {"prod", "x"};
    for (int mode_index = 0; mode_index < 2; mode_index++) {
      const PATDebugAudit *audit = audits[mode_index];
      const double mean = audit->spread_sum / worlds;
      const double variance = fmax(0.0, (audit->spread_sq_sum -
          audit->spread_sum * audit->spread_sum / worlds) / (worlds - 1));
      fprintf(out, "%llu\t%s\t%d\t%d\t%d\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%d\t%s\t%d\n",
              seed_input, modes[mode_index], audit->wins, audit->draws,
              audit->losses, mean, sqrt(variance / worlds),
              audit->utility_sum / worlds, audit->win_pct_sum / worlds,
              audit->horizon_leave_sum / worlds,
              audit->horizon_unseen_sum / worlds,
              (double)audit->total_plies / worlds,
              audit->terminal_adjustment_sum / worlds,
              audit->completed_worlds,
              bag_get_letters(game_get_bag(game)) <= PAT_DEBUG_FULL_GAME_BAG
                  ? "complete_game" : "four_ply", worlds);
      for (int ply = 0; ply < PAT_DEBUG_AUDIT_MAX_PLIES; ply++) {
        const int reached = audit->reached[ply];
        if (reached == 0) {
          break;
        }
        fprintf(plies, "%llu\t%s\t%d\t%d\t%.6f\t%.6f\t%.6f\t%.6f\n",
                seed_input, modes[mode_index], ply, reached,
                audit->scores[ply] / reached,
                (double)audit->bingos[ply] / reached,
                audit->adjustments[ply] / reached,
                audit->bag_before[ply] / reached);
      }
    }
    fflush(out);
    fflush(plies);
    completed++;
    fprintf(stderr, "[pat-debug-audit] completed=%d seed=%llu\n",
            completed, seed_input);
    fflush(stderr);
  }
  fclose(trace);
  fclose(ranks);
  fclose(plies);
  fclose(out);
  fclose(input);
  move_list_destroy(reply_list);
  move_list_destroy(choice_list);
  move_list_destroy(setup_list);
  config_destroy(config);
}


// Empty-board CSW24 move-choice disagreements, including bingo placements.
void pat_move_choice_debug_openings_csw24(void) {
  const char *attempt_text = getenv("PAT_OPENING_ATTEMPTS");
  const int attempts = attempt_text ? atoi(attempt_text) : 100000;
  const char *rack_file = getenv("PAT_OPENING_RACK_FILE");
  const char *offset_text = getenv("PAT_OPENING_INDEX_OFFSET");
  const char *stride_text = getenv("PAT_OPENING_INDEX_STRIDE");
  const uint64_t offset = offset_text ? strtoull(offset_text, NULL, 10) : 0;
  const uint64_t stride = stride_text ? strtoull(stride_text, NULL, 10) : 1;
  assert(stride > 0);
  FILE *rack_input = rack_file ? fopen(rack_file, "r") : NULL;
  assert(!rack_file || rack_input);
  assert(rack_input || attempts > 0);
  Config *config = config_create_or_die(
      "set -lex CSW24 -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -sinfer false -wmp true -rit true -ritmmap true "
      "-wit true -pat hookscore_x");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  const PATWeights *champion = player_get_pat(game_get_player(game, 0));
  assert(champion);
  const PATMoveChooser prod = {.label = "prod_no_pat", .pat = NULL};
  const PATMoveChooser x = {.label = "hookscore_x", .pat = champion};
  MoveList *choice_list = move_list_create(PAT_MOVE_CHOICE_MOVE_LIST_CAPACITY);
  const char *output_path = getenv("PAT_OPENING_OUTPUT");
  FILE *out = fopen(output_path ? output_path : "/tmp/pat_csw24_openings.tsv", "w");
  assert(out);
  fprintf(out, "seed\tbag\tmover\tcategory\tcgp\tprod_move\tprod_static_equity\tx_move\tx_static_equity\tprod_top10\tx_top10\tprod_rank_of_x\tx_rank_of_prod\tprod_equity_of_x\tx_equity_of_prod\n");
  int disagreements = 0;
  int both_bingo = 0;
  int one_bingo = 0;
  int nonbingo = 0;
  char rack_text[64];
  for (int attempt = 0; rack_input ? fscanf(rack_input, "%63s", rack_text) == 1
                                    : attempt < attempts; attempt++) {
    const uint64_t seed = rack_input
        ? 8800000000ULL + offset + (uint64_t)attempt * stride
        : 8700000000ULL + (uint64_t)attempt;
    game_reset(game);
    game_seed(game, seed);
    if (rack_input) {
      MachineLetter letters[RACK_SIZE];
      assert(ld_str_to_mls(game_get_ld(game), rack_text, false, letters,
                           RACK_SIZE) == RACK_SIZE);
      Rack *rack = player_get_rack(game_get_player(game, 0));
      for (int i = 0; i < RACK_SIZE; i++) {
        assert(bag_draw_letter(game_get_bag(game), letters[i], 0));
        rack_add_letter(rack, letters[i]);
      }
      draw_to_full_rack(game, 1);
    } else {
      draw_starting_racks(game);
    }
    player_set_pat(game_get_player(game, 0), NULL);
    player_set_pat(game_get_player(game, 1), NULL);
    const int mover_index = game_get_player_on_turn_index(game);
    Move move_prod;
    Move move_x;
    pat_move_choice_choose(game, mover_index, &prod, choice_list, &move_prod);
    if (move_get_type(&move_prod) != GAME_EVENT_TILE_PLACEMENT_MOVE) continue;
    char *prod_top10 = pat_debug_rankings(game, choice_list);
    pat_move_choice_choose(game, mover_index, &x, choice_list, &move_x);
    if (move_get_type(&move_x) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      free(prod_top10);
      continue;
    }
    char *x_top10 = pat_debug_rankings(game, choice_list);
    const int x_rank_of_prod = pat_debug_find_rank(choice_list, &move_prod);
    const double x_equity_of_prod = x_rank_of_prod > 0
        ? equity_to_double(move_get_equity(
              move_list_get_move(choice_list, x_rank_of_prod - 1))) : 0.0;
    Move ignored_prod;
    pat_move_choice_choose(game, mover_index, &prod, choice_list,
                           &ignored_prod);
    const int prod_rank_of_x = pat_debug_find_rank(choice_list, &move_x);
    const double prod_equity_of_x = prod_rank_of_x > 0
        ? equity_to_double(move_get_equity(
              move_list_get_move(choice_list, prod_rank_of_x - 1))) : 0.0;
    if (move_get_type(&move_prod) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
        move_get_type(&move_x) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
        compare_moves_without_equity(&move_prod, &move_x, true) != -1) {
      const bool prod_bingo = move_get_tiles_played(&move_prod) == RACK_SIZE;
      const bool x_bingo = move_get_tiles_played(&move_x) == RACK_SIZE;
      const char *category = prod_bingo && x_bingo ? "both_bingo" :
                             prod_bingo || x_bingo ? "one_bingo" : "nonbingo";
      if (prod_bingo && x_bingo) {
        both_bingo++;
      } else if (prod_bingo || x_bingo) {
        one_bingo++;
      } else {
        nonbingo++;
      }
      char *cgp = game_get_cgp(game, true);
      StringBuilder *sb = string_builder_create();
      string_builder_add_move(sb, game_get_board(game), &move_prod,
                              game_get_ld(game), true);
      char *prod_text = string_builder_dump(sb, NULL);
      string_builder_destroy(sb);
      sb = string_builder_create();
      string_builder_add_move(sb, game_get_board(game), &move_x,
                              game_get_ld(game), true);
      char *x_text = string_builder_dump(sb, NULL);
      string_builder_destroy(sb);
      fprintf(out, "%llu\t%d\t%d\t%s\t%s\t%s\t%.3f\t%s\t%.3f\t%s\t%s\t%d\t%d\t%.3f\t%.3f\n",
              (unsigned long long)seed, bag_get_letters(game_get_bag(game)),
              mover_index, category, cgp, prod_text,
              equity_to_double(move_get_equity(&move_prod)), x_text,
              equity_to_double(move_get_equity(&move_x)), prod_top10,
              x_top10, prod_rank_of_x, x_rank_of_prod, prod_equity_of_x,
              x_equity_of_prod);
      disagreements++;
      free(cgp);
      free(prod_text);
      free(x_text);
    }
    free(prod_top10);
    free(x_top10);
    if ((attempt + 1) % 10000 == 0) {
      fflush(out);
      fprintf(stderr, "[pat-opening] attempts=%d disagreements=%d both_bingo=%d one_bingo=%d nonbingo=%d\n",
              attempt + 1, disagreements, both_bingo, one_bingo, nonbingo);
      fflush(stderr);
    }
  }
  fclose(out);
  if (rack_input) fclose(rack_input);
  move_list_destroy(choice_list);
  config_destroy(config);
}

// Re-score the discovery run's leading cases on fresh hidden-rack worlds.
// Input is one source-game seed per line, selected outside this harness;
// this avoids the discovery worlds' winner's-curse noise in the final ranks.
void pat_move_choice_debug_confirm_csw24(void) {
  const char *worlds_text = getenv("PAT_DEBUG_WORLDS");
  const int worlds = worlds_text ? atoi(worlds_text) : 1024;
  assert(worlds > 1);
  Config *config = config_create_or_die(
      "set -lex CSW24 -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -sinfer false -wmp true -rit true -ritmmap true "
      "-wit true -winpct winpct -pat hookscore_x");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  const PATWeights *champion = player_get_pat(game_get_player(game, 0));
  const WinPct *win_pcts = config_get_win_pcts(config);
  assert(champion && win_pcts);
  const PATMoveChooser prod = {.label = "prod_no_pat", .pat = NULL};
  const PATMoveChooser x = {.label = "hookscore_x", .pat = champion};
  MoveList *setup_list = move_list_create(1);
  MoveList *choice_list = move_list_create(PAT_MOVE_CHOICE_MOVE_LIST_CAPACITY);
  MoveList *reply_list = move_list_create(1);
  const char *input_path = getenv("PAT_DEBUG_INPUT");
  const char *output_path = getenv("PAT_DEBUG_OUTPUT");
  FILE *input = fopen(input_path ? input_path :
                      "/tmp/pat_csw24_confirm_seeds.txt", "r");
  FILE *out = fopen(output_path ? output_path :
                    "/tmp/pat_csw24_confirm.tsv", "w");
  assert(input && out);
  fprintf(out, "seed\toracle_x_minus_prod\toracle_se\tworlds\tprod_win_pct\tx_win_pct\tprod_mean_spread\tx_mean_spread\tprod_utility\tx_utility\tutility_x_minus_prod\tutility_se\tmethod\tprod_horizon_leave\tx_horizon_leave\tprod_horizon_unseen\tx_horizon_unseen\n");
  fflush(out);
  int completed = 0;
  unsigned long long seed_input;
  while (fscanf(input, "%llu", &seed_input) == 1) {
    const uint64_t seed = (uint64_t)seed_input;
    const int target_bag = 10 + (int)(seed % 60ULL);
    game_reset(game);
    game_seed(game, seed);
    if (getenv("PAT_DEBUG_OPENING") && seed >= 8800000000ULL) {
      pat_debug_draw_exact_opening(game, seed);
    } else {
      draw_starting_racks(game);
    }
    player_set_pat(game_get_player(game, 0), NULL);
    player_set_pat(game_get_player(game, 1), NULL);
    const bool opening = getenv("PAT_DEBUG_OPENING") != NULL;
    while (!opening && game_get_game_end_reason(game) == GAME_END_REASON_NONE &&
           bag_get_letters(game_get_bag(game)) > target_bag) {
      const Move *setup = get_top_equity_move(game, setup_list);
      play_move(setup, game, NULL);
    }
    assert(game_get_game_end_reason(game) == GAME_END_REASON_NONE);
    const int mover_index = game_get_player_on_turn_index(game);
    Move move_prod;
    Move move_x;
    pat_move_choice_choose(game, mover_index, &prod, choice_list, &move_prod);
    pat_move_choice_choose(game, mover_index, &x, choice_list, &move_x);
    player_set_pat(game_get_player(game, mover_index), NULL);
    assert(compare_moves_without_equity(&move_prod, &move_x, true) != -1);
    double sum = 0.0;
    double sum_sq = 0.0;
    double prod_win_sum = 0.0;
    double x_win_sum = 0.0;
    double prod_spread_sum = 0.0;
    double x_spread_sum = 0.0;
    double prod_utility_sum = 0.0;
    double x_utility_sum = 0.0;
    double utility_diff_sum = 0.0;
    double utility_diff_sq_sum = 0.0;
    double prod_leave_sum = 0.0;
    double x_leave_sum = 0.0;
    double prod_unseen_sum = 0.0;
    double x_unseen_sum = 0.0;
    for (int world = 0; world < worlds; world++) {
      const uint64_t world_seed =
          seed + 700000000ULL + (uint64_t)world * 1000003ULL;
      const PATDebugOracleValue prod_value = pat_debug_hybrid_value(
          game, &move_prod, mover_index, world_seed, reply_list, win_pcts);
      const PATDebugOracleValue x_value = pat_debug_hybrid_value(
          game, &move_x, mover_index, world_seed, reply_list, win_pcts);
      const double difference = x_value.spread - prod_value.spread;
      sum += difference;
      sum_sq += difference * difference;
      const double prod_win = prod_value.win_pct;
      const double x_win = x_value.win_pct;
      const double prod_utility = prod_value.utility;
      const double x_utility = x_value.utility;
      const double utility_diff = x_utility - prod_utility;
      prod_leave_sum += prod_value.horizon_leave;
      x_leave_sum += x_value.horizon_leave;
      prod_unseen_sum += prod_value.horizon_unseen;
      x_unseen_sum += x_value.horizon_unseen;
      prod_win_sum += prod_win;
      x_win_sum += x_win;
      prod_spread_sum += prod_value.spread;
      x_spread_sum += x_value.spread;
      prod_utility_sum += prod_utility;
      x_utility_sum += x_utility;
      utility_diff_sum += utility_diff;
      utility_diff_sq_sum += utility_diff * utility_diff;
    }
    const double mean = sum / worlds;
    const double variance =
        fmax(0.0, (sum_sq - sum * sum / worlds) / (worlds - 1));
    const double utility_variance = fmax(0.0, (
        utility_diff_sq_sum - utility_diff_sum * utility_diff_sum / worlds) /
        (worlds - 1));
    fprintf(out,
            "%llu\t%.6f\t%.6f\t%d\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%s\t%.6f\t%.6f\t%.6f\t%.6f\n",
            seed_input, mean, sqrt(variance / worlds), worlds,
            prod_win_sum / worlds, x_win_sum / worlds,
            prod_spread_sum / worlds, x_spread_sum / worlds,
            prod_utility_sum / worlds, x_utility_sum / worlds,
            utility_diff_sum / worlds, sqrt(utility_variance / worlds),
            bag_get_letters(game_get_bag(game)) <= PAT_DEBUG_FULL_GAME_BAG
                ? "complete_game" : "four_ply",
            prod_leave_sum / worlds, x_leave_sum / worlds,
            prod_unseen_sum / worlds, x_unseen_sum / worlds);
    fflush(out);
    completed++;
    fprintf(stderr,
            "[pat-debug-confirm] completed=%d seed=%llu mean=%.3f se=%.3f\n",
            completed, seed_input, mean, sqrt(variance / worlds));
    fflush(stderr);
  }
  fclose(out);
  fclose(input);
  move_list_destroy(reply_list);
  move_list_destroy(choice_list);
  move_list_destroy(setup_list);
  config_destroy(config);
}

// Decision diagnostic between two PAT models on any lexicon:
//   patdecide:<lex>:<pat_a>:<pat_b>:<seed>:<positions>:<worlds>
//       [:<shard>:<num_shards>]
// Positions come from pat_a self-play stopped at a bag size drawn from
// [1, PAT_DECIDE_MAX_BAG] (the top value is the opening). Each model picks
// its move independently from the same exhaustive list; on disagreement both
// moves are scored by pat_debug_hybrid_value on identical paired worlds
// (opponent rack resampled before either move, production no-PAT static
// continuation, four plies plus leave and winpct above
// PAT_DEBUG_FULL_GAME_BAG bag tiles, game end at or below it). One CASE line
// per disagreement (b minus a) and one POS line per shard, for pooling.
// "none" loads no PAT for that side.
enum { PAT_DECIDE_MAX_BAG = 86 };

static PATWeights *pat_decide_load(Config *config, const char *name) {
  if (strings_iequal(name, "none")) {
    return NULL;
  }
  Game *game = config_get_game(config);
  ErrorStack *error_stack = error_stack_create();
  PATWeights *pat =
      pat_create(config_get_data_paths(config), name, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("could not load PAT file '%s'", name);
  }
  error_stack_destroy(error_stack);
  pat_prepare_hook_flex(pat, player_get_kwg(game_get_player(game, 0)),
                        game_get_ld(game));
  return pat;
}

static bool pat_decide_file_exists(const char *path) {
  FILE *file = fopen(path, "r");
  if (!file) {
    return false;
  }
  fclose(file);
  return true;
}

void pat_move_choice_run_decision_spec(const char *spec) {
  char buffer[512];
  snprintf(buffer, sizeof(buffer), "%s", spec);
  char *fields[8] = {NULL};
  int num_fields = 0;
  char *save = NULL;
  for (char *tok = strtok_r(buffer, ":", &save); tok && num_fields < 8;
       tok = strtok_r(NULL, ":", &save)) {
    fields[num_fields++] = tok;
  }
  if (num_fields != 6 && num_fields != 8) {
    log_fatal("patdecide spec needs 6 or 8 colon-separated fields: %s", spec);
  }
  const char *lexicon = fields[0];
  const uint64_t seed_base = strtoull(fields[3], NULL, 10);
  const int num_positions = atoi(fields[4]);
  const int worlds = atoi(fields[5]);
  const int shard = (num_fields == 8) ? atoi(fields[6]) : 0;
  const int num_shards = (num_fields == 8) ? atoi(fields[7]) : 1;
  assert(worlds > 0 && num_shards >= 1 && shard >= 0 && shard < num_shards);

  char *rit_path = get_formatted_string("data/lexica/%s.rit", lexicon);
  char *wit_path = get_formatted_string("data/lexica/%s.wit", lexicon);
  char *set_cmd = get_formatted_string(
      "set -lex %s -s1 equity -s2 equity -r1 all -r2 all -numplays 1 "
      "-sinfer false -wmp true %s %s -winpct winpct",
      lexicon,
      pat_decide_file_exists(rit_path) ? "-rit true -ritmmap true" : "",
      pat_decide_file_exists(wit_path) ? "-wit true" : "");
  free(rit_path);
  free(wit_path);
  Config *config = config_create_or_die(set_cmd);
  free(set_cmd);
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  const WinPct *win_pcts = config_get_win_pcts(config);
  assert(win_pcts);
  // Side B may be "<pat>@<volatility pat>@<scale>" (see
  // PATMoveChooser.vol_pat).
  char *vol_name = strchr(fields[2], '@');
  double vol_scale = 0.0;
  if (vol_name) {
    *vol_name++ = '\0';
    char *scale_text = strchr(vol_name, '@');
    if (!scale_text) {
      log_fatal("patdecide volatility side needs <pat>@<vol>@<scale>");
    }
    *scale_text++ = '\0';
    vol_scale = strtod(scale_text, NULL);
  }
  PATWeights *pat_a = pat_decide_load(config, fields[1]);
  PATWeights *pat_b = pat_decide_load(config, fields[2]);
  PATWeights *vol_pat = vol_name ? pat_decide_load(config, vol_name) : NULL;
  const PATMoveChooser chooser_a = {.label = fields[1], .pat = pat_a};
  const PATMoveChooser chooser_b = {.label = fields[2],
                                    .pat = pat_b,
                                    .vol_pat = vol_pat,
                                    .vol_scale = vol_scale,
                                    .win_pcts = win_pcts};
  Game *game = config_get_game(config);
  MoveList *setup_list = move_list_create(1);
  MoveList *choice_list =
      move_list_create(PAT_MOVE_CHOICE_MOVE_LIST_CAPACITY);
  MoveList *reply_list = move_list_create(1);

  long positions = 0;
  long disagreements = 0;
  for (int attempt = 0; attempt < num_positions; attempt++) {
    if (attempt % num_shards != shard) {
      continue;
    }
    const uint64_t seed = seed_base + (uint64_t)attempt;
    game_reset(game);
    game_seed(game, seed);
    draw_starting_racks(game);
    player_set_pat(game_get_player(game, 0), pat_a);
    player_set_pat(game_get_player(game, 1), pat_a);
    const int target_bag = 1 + (int)(seed % (uint64_t)PAT_DECIDE_MAX_BAG);
    while (game_get_game_end_reason(game) == GAME_END_REASON_NONE &&
           bag_get_letters(game_get_bag(game)) > target_bag) {
      play_move(get_top_equity_move(game, setup_list), game, NULL);
    }
    const Board *board = game_get_board(game);
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE ||
        bag_get_letters(game_get_bag(game)) == 0 ||
        !board_get_cross_sets_valid(board) || board_get_transposed(board)) {
      continue;
    }
    positions++;
    const int mover_index = game_get_player_on_turn_index(game);
    Move move_a;
    Move move_b;
    pat_move_choice_choose(game, mover_index, &chooser_a, choice_list,
                           &move_a);
    pat_move_choice_choose(game, mover_index, &chooser_b, choice_list,
                           &move_b);
    player_set_pat(game_get_player(game, mover_index), pat_a);
    if (compare_moves_without_equity(&move_a, &move_b, true) == -1) {
      continue;
    }
    disagreements++;
    double sum_utility = 0.0;
    double sum_utility_sq = 0.0;
    double sum_spread = 0.0;
    double sum_win = 0.0;
    for (int world = 0; world < worlds; world++) {
      const uint64_t world_seed =
          seed + 500000000ULL + (uint64_t)world * 1000003ULL;
      const PATDebugOracleValue value_a = pat_debug_hybrid_value(
          game, &move_a, mover_index, world_seed, reply_list, win_pcts);
      const PATDebugOracleValue value_b = pat_debug_hybrid_value(
          game, &move_b, mover_index, world_seed, reply_list, win_pcts);
      const double utility_diff = value_b.utility - value_a.utility;
      sum_utility += utility_diff;
      sum_utility_sq += utility_diff * utility_diff;
      sum_spread += value_b.spread - value_a.spread;
      sum_win += value_b.win_pct - value_a.win_pct;
    }
    const int bag = bag_get_letters(game_get_bag(game));
    const double margin_before = equity_to_double(
        player_get_score(game_get_player(game, mover_index)) -
        player_get_score(game_get_player(game, 1 - mover_index)));
    const bool bingo_a = move_get_type(&move_a) ==
                             GAME_EVENT_TILE_PLACEMENT_MOVE &&
                         move_get_tiles_played(&move_a) == RACK_SIZE;
    const bool bingo_b = move_get_type(&move_b) ==
                             GAME_EVENT_TILE_PLACEMENT_MOVE &&
                         move_get_tiles_played(&move_b) == RACK_SIZE;
    const char *bingo_class = (bingo_a && bingo_b)     ? "bb"
                              : (!bingo_a && !bingo_b) ? "nn"
                                                       : "mixed";
    const double utility_mean = sum_utility / worlds;
    const double utility_var =
        worlds > 1 ? fmax(0.0, (sum_utility_sq - sum_utility * sum_utility /
                                                      worlds) /
                                   (worlds - 1))
                   : 0.0;
    printf("CASE\t%llu\t%d\t%s\t%.6f\t%.6f\t%.6f\t%.6f\t%.0f\n",
           (unsigned long long)seed, bag, bingo_class, utility_mean,
           sum_win / worlds, sum_spread / worlds, utility_var, margin_before);
  }
  printf("POS\t%ld\t%ld\n", positions, disagreements);
  move_list_destroy(reply_list);
  move_list_destroy(choice_list);
  move_list_destroy(setup_list);
  player_set_pat(game_get_player(game, 0), NULL);
  player_set_pat(game_get_player(game, 1), NULL);
  if (pat_a) {
    pat_destroy(pat_a);
  }
  if (pat_b) {
    pat_destroy(pat_b);
  }
  if (vol_pat) {
    pat_destroy(vol_pat);
  }
  config_destroy(config);
}

// Prints the mover's win% and utility curvature kappa on a margin grid.
void pat_move_choice_print_kappa(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -winpct winpct");
  const WinPct *win_pcts = config_get_win_pcts(config);
  const int unseen_values[] = {10, 20, 40, 60, 90};
  for (int unseen_idx = 0; unseen_idx < 5; unseen_idx++) {
    const int unseen = unseen_values[unseen_idx];
    for (int margin = -150; margin <= 150; margin += 25) {
      const double win =
          1.0 - win_pct_get(win_pcts, -margin, (unsigned)unseen);
      printf("KAPPA\t%d\t%d\t%.4f\t%.5f\n", unseen, margin, win,
             pat_move_choice_kappa(win_pcts, margin, unseen));
    }
  }
  config_destroy(config);
}
