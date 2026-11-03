#include "pat_inference.h"

#include "../def/board_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/pat.h"
#include "../ent/pat_eval.h"
#include "../ent/pat_features.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/static_eval.h"
#include "../util/io_util.h"
#include "census.h"
#include "census_play_scores.h"
#include "move_gen.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

enum { PAT_INFERENCE_CARRY = 4 };
struct PATInference {
  Game *game;
  int target;
  PATEvalContext pat;
  uint8_t board_unseen[MAX_ALPHABET_SIZE];
  Equity word_penalties[2 * BOARD_DIM];
  Equity letter_penalties[2 * BOARD_DIM];
  CensusLattice *lattice;
  Equity *scores;
  MoveList *moves;
  Move carry[PAT_INFERENCE_CARRY];
  int carry_count;
  int next_carry;
  bool observed_initialized;
  Move observed;
  PATInferenceStats stats;
};

PATInference *pat_inference_create(Game *game, bool use_census,
                                   ErrorStack *errors) {
  PATInference *ctx = calloc_or_die(1, sizeof(PATInference));
  ctx->game = game;
  ctx->target = game_get_player_on_turn_index(game);
  const Player *player = game_get_player(game, ctx->target);
  const Player *opponent = game_get_player(game, 1 - ctx->target);
  const LetterDistribution *ld = game_get_ld(game);
  const Board *board = game_get_board(game);
  assert(player_get_pat(player) != NULL);
  const uint32_t disabled_classes =
      player_get_pat_disabled_classes_mask(player);
  const int cross_index = board_get_cross_set_index(
      game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG), ctx->target);
  pat_compute_unseen_counts(board_get_readonly_lanes(board, cross_index), ld,
                            NULL, ctx->board_unseen);
  pat_eval_context_load(&ctx->pat, player_get_pat(player),
                        board_get_readonly_lanes(board, cross_index), ld,
                        player_get_rack(player),
                        PAT_CLASS_MASK_ALL & ~disabled_classes,
                        rack_get_total_letters(player_get_rack(opponent)));
  pat_eval_context_set_kwg(&ctx->pat, player_get_kwg(player),
                           game_get_variant(game));
  pat_eval_context_set_utility(
      &ctx->pat,
      equity_to_int(player_get_score(player) - player_get_score(opponent)),
      bag_get_letters(game_get_bag(game)));
  if (bag_get_letters(game_get_bag(game)) == 0) {
    pat_eval_context_disable(&ctx->pat);
  }
  board_copy_opening_penalties(board, ctx->word_penalties,
                               ctx->letter_penalties);
  ctx->moves = move_list_create(1);
  // Opening census folds in legacy placement penalties, whereas PAT may
  // replace them. Do not use those scores as PAT upper bounds. Unsupported
  // variants and bag-empty positions take the exact path as well.
  if (use_census && board_get_tiles_played(board) > 0 &&
      bag_get_letters(game_get_bag(game)) > 0 &&
      game_get_variant(game) == GAME_VARIANT_CLASSIC &&
      game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG) &&
      ld_get_size(ld) <= CENSUS_MAX_LETTERS) {
    ctx->lattice = census_lattice_create(ld_get_size(ld), RACK_SIZE, errors);
    if (ctx->lattice == NULL) {
      pat_inference_destroy(ctx);
      return NULL;
    }
    const uint64_t size = census_lattice_get_size(ctx->lattice);
    ctx->scores = malloc_or_die(sizeof(Equity) * size);
    for (uint64_t index = 0; index < size; index++) {
      ctx->scores[index] = CENSUS_UNPLAYABLE;
    }
    uint8_t pool[MAX_ALPHABET_SIZE] = {0};
    for (int ml = 0; ml < ld_get_size(ld); ml++) {
      const int held = rack_get_letter(player_get_rack(opponent), ml);
      pool[ml] =
          ctx->board_unseen[ml] > held ? ctx->board_unseen[ml] - held : 0;
    }
    census_play_scores_build(game, ctx->lattice, pool, ctx->scores);
  }
  return ctx;
}

void pat_inference_destroy(PATInference *ctx) {
  if (ctx == NULL) {
    return;
  }
  census_lattice_destroy(ctx->lattice);
  free(ctx->scores);
  move_list_destroy(ctx->moves);
  free(ctx);
}

static Equity pat_inference_move_equity(const PATInference *ctx,
                                        const Move *move, const Rack *leave) {
  const Player *player = game_get_player(ctx->game, ctx->target);
  return static_eval_get_move_equity(
      game_get_ld(ctx->game), player_get_klv(player), move, leave,
      player_get_rack(game_get_player(ctx->game, 1 - ctx->target)),
      ctx->word_penalties, ctx->letter_penalties,
      pat_eval_context_get_active_classes(&ctx->pat), &ctx->pat,
      board_get_tiles_played(game_get_board(ctx->game)),
      bag_get_letters(game_get_bag(ctx->game)));
}

// Every placement's defense term is bounded by its lane's bound. The
// maximum over lanes plus the score/tiles-specific utility bound remains
// conservative even when the census's highest-score placement is not PAT-best.
static Equity pat_inference_census_bound(const PATInference *ctx,
                                         Equity *exchange_lower) {
  const Game *game = ctx->game;
  const Player *player = game_get_player(game, ctx->target);
  const Rack *rack = player_get_rack(player);
  const Rack *opponent =
      player_get_rack(game_get_player(game, 1 - ctx->target));
  const LetterDistribution *ld = game_get_ld(game);
  const int bag = bag_get_letters(game_get_bag(game));
  Equity defense_bound = EQUITY_MIN_VALUE;
  for (int dir = 0; dir < 2; dir++) {
    for (int lane = 0; lane < BOARD_DIM; lane++) {
      const Equity bound = pat_eval_lane_defense_bound(&ctx->pat, dir, lane);
      if (bound > defense_bound) {
        defense_bound = bound;
      }
    }
  }
  MachineLetter tiles[RACK_SIZE];
  int size = 0;
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    for (int count = 0; count < rack_get_letter(rack, ml); count++) {
      assert(size < RACK_SIZE);
      tiles[size++] = (MachineLetter)ml;
    }
  }
  Equity upper = EQUITY_PASS_VALUE;
  *exchange_lower = EQUITY_PASS_VALUE;
  for (unsigned int subset = 1; subset < (1U << size); subset++) {
    uint8_t played[MAX_ALPHABET_SIZE] = {0};
    Rack kept = *rack;
    int used = 0;
    for (int tile_idx = 0; tile_idx < size; tile_idx++) {
      if ((subset & (1U << tile_idx)) != 0) {
        played[tiles[tile_idx]]++;
        rack_take_letter(&kept, tiles[tile_idx]);
        used++;
      }
    }
    const Equity leave = klv_get_leave_value(player_get_klv(player), &kept);
    const Equity score = ctx->scores[census_lattice_rank(ctx->lattice, played)];
    if (score != CENSUS_UNPLAYABLE) {
      const Equity bound =
          static_eval_get_nonopening_move_equity(ld, &kept, opponent, used, bag,
                                                 score, leave) +
          defense_bound +
          pat_eval_utility_bound_for_play(&ctx->pat, used, score);
      if (bound > upper) {
        upper = bound;
      }
    }
    if (bag + rack_get_total_letters(opponent) >= 2 * RACK_SIZE) {
      const Equity exchange = static_eval_get_nonopening_move_equity(
                                  ld, &kept, opponent, used, bag, 0, leave) +
                              pat_eval_non_placement_penalty(&ctx->pat);
      if (exchange > *exchange_lower) {
        *exchange_lower = exchange;
      }
      if (exchange > upper) {
        upper = exchange;
      }
    }
  }
  return upper;
}

// The observed move is already priced exactly for every candidate rack.
// Remove it from its census cell so its loose upper bound cannot force an
// otherwise unnecessary search. ALL_SMALL grows dynamically: no truncated
// shortlist may be used to compute this upper bound.
static void pat_inference_exclude_observed(PATInference *ctx,
                                           const Move *observed) {
  if (ctx->observed_initialized) {
    assert(compare_moves_without_equity(&ctx->observed, observed, true) == -1);
    return;
  }
  move_copy(&ctx->observed, observed);
  ctx->observed_initialized = true;
  if (ctx->scores == NULL) {
    return;
  }
  Rack *rack = player_get_rack(game_get_player(ctx->game, ctx->target));
  const Rack saved = *rack;
  rack_reset(rack);
  uint8_t used[MAX_ALPHABET_SIZE] = {0};
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(observed);
       tile_idx++) {
    const MachineLetter tile = move_get_tile(observed, tile_idx);
    if (tile != PLAYED_THROUGH_MARKER) {
      const MachineLetter ml =
          get_is_blanked(tile) ? BLANK_MACHINE_LETTER : tile;
      rack_add_letter(rack, ml);
      used[ml]++;
    }
  }
  MoveList *alternatives = move_list_create_small(128);
  const MoveGenArgs args = {.game = ctx->game,
                            .move_list = alternatives,
                            .move_record_type = MOVE_RECORD_ALL_SMALL,
                            .move_sort_type = MOVE_SORT_SCORE,
                            .target_equity = EQUITY_MAX_VALUE,
                            .disable_pat = true};
  generate_moves(&args);
  Equity best = CENSUS_UNPLAYABLE;
  for (int move_idx = 0; move_idx < move_list_get_count(alternatives);
       move_idx++) {
    const SmallMove *small = alternatives->small_moves[move_idx];
    if (small_move_get_tiles_played(small) == move_get_tiles_played(observed)) {
      Move move;
      small_move_to_move(&move, small, game_get_board(ctx->game));
      if (compare_moves_without_equity(&move, observed, true) != -1 &&
          move_get_score(&move) > best) {
        best = move_get_score(&move);
      }
    }
  }
  ctx->scores[census_lattice_rank(ctx->lattice, used)] = best;
  small_move_list_destroy(alternatives);
  rack_copy(rack, &saved);
}

PATInferenceEvaluation pat_inference_evaluate(PATInference *ctx,
                                              const Move *observed,
                                              const Rack *leave,
                                              Equity margin) {
  assert(move_get_type(observed) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(margin >= 0);
  const Player *player = game_get_player(ctx->game, ctx->target);
  const Rack *rack = player_get_rack(player);
  pat_inference_exclude_observed(ctx, observed);
  pat_eval_context_reload_rack(&ctx->pat, rack, ctx->board_unseen);
  PATInferenceEvaluation result = {
      .observed = pat_inference_move_equity(ctx, observed, leave),
      .resolution = PAT_INFERENCE_EXACT};
  const int64_t cutoff = (int64_t)result.observed + margin;
  ctx->stats.racks++;
  if (ctx->scores != NULL) {
    Equity lower;
    const Equity upper = pat_inference_census_bound(ctx, &lower);
    if (lower > cutoff) {
      result.best = lower;
      result.resolution = PAT_INFERENCE_REJECTED;
    } else if (upper <= result.observed) {
      result.best = result.observed;
      result.resolution = PAT_INFERENCE_ZERO_REGRET;
    }
  }
  // Previous exact searches supply legal lower bounds on this same board.
  // Recompute both legality and PAT equity for the new candidate rack.
  for (int move_idx = 0;
       result.resolution == PAT_INFERENCE_EXACT && move_idx < ctx->carry_count;
       move_idx++) {
    const Move *candidate = &ctx->carry[move_idx];
    Rack kept = *rack;
    bool legal = true;
    for (int tile_idx = 0; tile_idx < move_get_tiles_length(candidate);
         tile_idx++) {
      const MachineLetter tile = move_get_tile(candidate, tile_idx);
      if (tile == PLAYED_THROUGH_MARKER) {
        continue;
      }
      const MachineLetter ml =
          get_is_blanked(tile) ? BLANK_MACHINE_LETTER : tile;
      if (rack_get_letter(&kept, ml) == 0) {
        legal = false;
        break;
      }
      rack_take_letter(&kept, ml);
    }
    if (legal) {
      const Equity lower = pat_inference_move_equity(ctx, candidate, &kept);
      if (lower > cutoff) {
        result.best = lower;
        result.resolution = PAT_INFERENCE_REJECTED;
      }
    }
  }
  if (result.resolution == PAT_INFERENCE_EXACT) {
    const MoveGenArgs args = {.game = ctx->game,
                              .move_record_type = MOVE_RECORD_BEST,
                              .move_sort_type = MOVE_SORT_EQUITY,
                              .target_equity = EQUITY_MAX_VALUE,
                              .move_list = ctx->moves};
    generate_moves_with_pat_context(&args, &ctx->pat);
    const Move *best = move_list_get_move(ctx->moves, 0);
    result.best = move_get_equity(best);
    if (move_get_type(best) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      move_copy(&ctx->carry[ctx->next_carry], best);
      ctx->next_carry = (ctx->next_carry + 1) % PAT_INFERENCE_CARRY;
      if (ctx->carry_count < PAT_INFERENCE_CARRY) {
        ctx->carry_count++;
      }
    }
    ctx->stats.exact_searches++;
  } else if (result.resolution == PAT_INFERENCE_ZERO_REGRET) {
    ctx->stats.zero_regrets++;
  } else {
    ctx->stats.rejected++;
  }
  return result;
}

PATInferenceStats pat_inference_get_stats(const PATInference *ctx) {
  return ctx->stats;
}
