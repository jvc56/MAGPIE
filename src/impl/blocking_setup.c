#include "blocking_setup.h"

#include "../def/board_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/rack_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/xoshiro.h"
#include "../util/io_util.h"
#include "gameplay.h"
#include "move_gen.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

BlockingSetupSamples *blocking_setup_samples_create(int max_racks,
                                                    int pool_capacity) {
  assert(max_racks >= BLOCKING_SETUP_MIN_RACKS &&
         max_racks <= BLOCKING_SETUP_MAX_RACKS);
  assert(pool_capacity > 0);
  BlockingSetupSamples *samples = malloc_or_die(sizeof(BlockingSetupSamples));
  samples->num_racks = 0;
  samples->pool_size = 0;
  samples->rack_capacity = max_racks;
  samples->pool_capacity = pool_capacity;
  samples->opponent_racks = malloc_or_die(sizeof(Rack) * (size_t)max_racks);
  samples->draw_orders = malloc_or_die(
      sizeof(MachineLetter) * (size_t)max_racks * (size_t)pool_capacity);
  samples->draw_sizes = malloc_or_die(sizeof(int) * (size_t)max_racks);
  return samples;
}

void blocking_setup_samples_destroy(BlockingSetupSamples *samples) {
  if (samples == NULL) {
    return;
  }
  free(samples->opponent_racks);
  free(samples->draw_orders);
  free(samples->draw_sizes);
  free(samples);
}

int blocking_setup_unseen_pool(const Game *game, MachineLetter *pool) {
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  int unseen[MAX_ALPHABET_SIZE];
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] = ld_get_dist(ld, ml);
  }
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const MachineLetter letter = board_get_letter(board, row, col);
      if (letter == ALPHABET_EMPTY_SQUARE_MARKER) {
        continue;
      }
      const MachineLetter counted =
          get_is_blanked(letter) ? BLANK_MACHINE_LETTER : letter;
      assert(unseen[counted] > 0);
      unseen[counted]--;
    }
  }
  const Rack *rack = player_get_rack(
      game_get_player(game, game_get_player_on_turn_index(game)));
  int pool_size = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    const int count = unseen[ml] - rack_get_letter(rack, (MachineLetter)ml);
    assert(count >= 0);
    for (int tile_idx = 0; tile_idx < count; tile_idx++) {
      pool[pool_size++] = (MachineLetter)ml;
    }
  }
  return pool_size;
}

static void shuffle_tiles(MachineLetter *tiles, int count, XoshiroPRNG *prng) {
  for (int tile_idx = 0; tile_idx < count - 1; tile_idx++) {
    const int pick = tile_idx + (int)prng_get_random_number(
                                    prng, (uint64_t)(count - tile_idx));
    const MachineLetter tile = tiles[pick];
    tiles[pick] = tiles[tile_idx];
    tiles[tile_idx] = tile;
  }
}

void blocking_setup_samples_deal(BlockingSetupSamples *samples,
                                 const Game *game, int num_racks,
                                 bool partition, bool condition_draws,
                                 uint64_t seed) {
  assert(num_racks >= BLOCKING_SETUP_MIN_RACKS &&
         num_racks <= samples->rack_capacity);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  assert(ld_get_total_tiles(ld) <= samples->pool_capacity);
  MachineLetter *pool =
      malloc_or_die(sizeof(MachineLetter) * (size_t)samples->pool_capacity);
  MachineLetter *shuffled =
      malloc_or_die(sizeof(MachineLetter) * (size_t)samples->pool_capacity);
  const int pool_size = blocking_setup_unseen_pool(game, pool);
  const int rack_size = pool_size < RACK_SIZE ? pool_size : RACK_SIZE;
  XoshiroPRNG *prng = prng_create(seed);
  int rack_idx = 0;
  while (rack_idx < num_racks) {
    for (int tile_idx = 0; tile_idx < pool_size; tile_idx++) {
      shuffled[tile_idx] = pool[tile_idx];
    }
    shuffle_tiles(shuffled, pool_size, prng);
    // With partition, one shuffle deals as many disjoint racks as fit; an
    // independent draw uses a fresh shuffle for every rack.
    for (int start = 0; start + rack_size <= pool_size && rack_idx < num_racks;
         start += rack_size) {
      Rack *rack = &samples->opponent_racks[rack_idx];
      rack_set_dist_size_and_reset(rack, ld_size);
      for (int tile_idx = 0; tile_idx < rack_size; tile_idx++) {
        rack_add_letter(rack, shuffled[start + tile_idx]);
      }
      rack_idx++;
      if (!partition || rack_size == 0) {
        break;
      }
    }
  }
  for (rack_idx = 0; rack_idx < num_racks; rack_idx++) {
    Rack excluded;
    rack_copy(&excluded, &samples->opponent_racks[rack_idx]);
    MachineLetter *order = samples->draw_orders +
                           ((size_t)rack_idx * (size_t)samples->pool_capacity);
    int draw_size = 0;
    for (int tile_idx = 0; tile_idx < pool_size; tile_idx++) {
      const MachineLetter tile = pool[tile_idx];
      if (condition_draws && rack_get_letter(&excluded, tile) > 0) {
        rack_take_letter(&excluded, tile);
      } else {
        order[draw_size++] = tile;
      }
    }
    shuffle_tiles(order, draw_size, prng);
    samples->draw_sizes[rack_idx] = draw_size;
  }
  samples->num_racks = num_racks;
  samples->pool_size = pool_size;
  prng_destroy(prng);
  free(shuffled);
  free(pool);
}

void blocking_setup_candidate_leave(const Game *game, const Move *move,
                                    Rack *leave) {
  rack_copy(leave, player_get_rack(game_get_player(
                       game, game_get_player_on_turn_index(game))));
  const bool is_exchange = move_get_type(move) == GAME_EVENT_EXCHANGE;
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    const MachineLetter tile = move_get_tile(move, tile_idx);
    // An exchanged blank is BLANK_MACHINE_LETTER, which shares its value
    // with PLAYED_THROUGH_MARKER; only placements play through tiles.
    if (!is_exchange && tile == PLAYED_THROUGH_MARKER) {
      continue;
    }
    rack_take_letter(leave, get_is_blanked(tile) ? BLANK_MACHINE_LETTER : tile);
  }
}

struct BlockingSetupChecker {
  const LetterDistribution *ld;
  const BlockingSetupSamples *samples;
  int followup_draws;
  int on_turn_index;
  int bag_tiles;
  Game *base;
  Game *after;
  // One board per rack: our pass and that rack's best reply to it.
  Game **pass_replied;
  int pass_replied_capacity;
  double *pass_reply_scores;
  MoveList *reply_list;
  Move *pass_move;
};

BlockingSetupChecker *blocking_setup_checker_create(void) {
  BlockingSetupChecker *checker = malloc_or_die(sizeof(BlockingSetupChecker));
  checker->ld = NULL;
  checker->samples = NULL;
  checker->followup_draws = 0;
  checker->on_turn_index = 0;
  checker->bag_tiles = 0;
  checker->base = NULL;
  checker->after = NULL;
  checker->pass_replied = NULL;
  checker->pass_replied_capacity = 0;
  checker->pass_reply_scores = NULL;
  checker->reply_list = move_list_create(1);
  checker->pass_move = move_create();
  move_set_as_pass(checker->pass_move);
  return checker;
}

static void checker_destroy_games(BlockingSetupChecker *checker) {
  game_destroy(checker->base);
  game_destroy(checker->after);
  for (int rack_idx = 0; rack_idx < checker->pass_replied_capacity;
       rack_idx++) {
    game_destroy(checker->pass_replied[rack_idx]);
  }
  free(checker->pass_replied);
  free(checker->pass_reply_scores);
  checker->base = NULL;
  checker->after = NULL;
  checker->pass_replied = NULL;
  checker->pass_reply_scores = NULL;
  checker->pass_replied_capacity = 0;
}

void blocking_setup_checker_destroy(BlockingSetupChecker *checker) {
  if (checker == NULL) {
    return;
  }
  checker_destroy_games(checker);
  move_list_destroy(checker->reply_list);
  move_destroy(checker->pass_move);
  free(checker);
}

// Copies src into *dst, creating it when missing. Copies share the letter
// distribution, so a checker reloaded with a different one recreates its
// games first (see blocking_setup_checker_load).
static void checker_copy_game(Game **dst, const Game *src) {
  if (*dst == NULL) {
    *dst = game_duplicate(src);
  } else {
    game_copy(*dst, src);
  }
}

// The opponent's (or our) best tile placement by score on game for the
// player on turn's current rack, or NULL when there is none.
static const Move *checker_best_placement(BlockingSetupChecker *checker,
                                          const Game *game) {
  move_list_reset(checker->reply_list);
  const MoveGenArgs args = {
      .game = game,
      .move_list = checker->reply_list,
      .move_record_type = MOVE_RECORD_BEST,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .disable_pat = true,
  };
  generate_moves(&args);
  if (move_list_get_count(checker->reply_list) == 0) {
    return NULL;
  }
  const Move *best = move_list_get_move(checker->reply_list, 0);
  if (move_get_type(best) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return NULL;
  }
  return best;
}

// Replaces the opponent's rack with a sampled one, then has the opponent
// play its best placement (or pass). Returns the reply's score in points.
static double checker_play_reply(BlockingSetupChecker *checker, Game *game,
                                 const Rack *opponent_rack) {
  rack_copy(player_get_rack(game_get_player(game, 1 - checker->on_turn_index)),
            opponent_rack);
  const Move *reply = checker_best_placement(checker, game);
  const double score =
      reply == NULL ? 0.0 : equity_to_double(move_get_score(reply));
  play_move(reply == NULL ? checker->pass_move : reply, game, NULL);
  return score;
}

void blocking_setup_checker_load(BlockingSetupChecker *checker,
                                 const Game *game,
                                 const BlockingSetupSamples *samples,
                                 int followup_draws) {
  assert(!game_over(game));
  assert(bag_get_letters(game_get_bag(game)) > 0);
  assert(samples->num_racks >= BLOCKING_SETUP_MIN_RACKS);
  assert(followup_draws >= 1 &&
         followup_draws <= BLOCKING_SETUP_MAX_FOLLOWUP_DRAWS);
  if (checker->ld != game_get_ld(game) ||
      checker->pass_replied_capacity < samples->num_racks) {
    checker_destroy_games(checker);
    checker->ld = game_get_ld(game);
    checker->pass_replied_capacity = samples->num_racks;
    checker->pass_replied =
        malloc_or_die(sizeof(Game *) * (size_t)samples->num_racks);
    checker->pass_reply_scores =
        malloc_or_die(sizeof(double) * (size_t)samples->num_racks);
    for (int rack_idx = 0; rack_idx < samples->num_racks; rack_idx++) {
      checker->pass_replied[rack_idx] = NULL;
    }
  }
  checker->samples = samples;
  checker->followup_draws = followup_draws;
  checker->on_turn_index = game_get_player_on_turn_index(game);
  // The tiles left to draw once the opponent holds a full rack. This is the
  // bag when the game knows the opponent's rack, and stays right when it
  // does not (the bag then also holds the opponent's tiles).
  checker->bag_tiles =
      samples->pool_size -
      (samples->pool_size < RACK_SIZE ? samples->pool_size : RACK_SIZE);
  checker_copy_game(&checker->base, game);
  game_set_backup_mode(checker->base, BACKUP_MODE_OFF);
  for (int rack_idx = 0; rack_idx < samples->num_racks; rack_idx++) {
    checker_copy_game(&checker->pass_replied[rack_idx], checker->base);
    Game *passed = checker->pass_replied[rack_idx];
    play_move(checker->pass_move, passed, NULL);
    checker->pass_reply_scores[rack_idx] =
        checker_play_reply(checker, passed, &samples->opponent_racks[rack_idx]);
  }
}

// Our best follow-up placement score in points with the given rack, or 0
// when the game is over.
static double checker_followup(BlockingSetupChecker *checker, const Game *game,
                               const Rack *rack) {
  if (game_over(game)) {
    return 0.0;
  }
  rack_copy(player_get_rack(game_get_player(game, checker->on_turn_index)),
            rack);
  const Move *best = checker_best_placement(checker, game);
  return best == NULL ? 0.0 : equity_to_double(move_get_score(best));
}

void blocking_setup_checker_measure(BlockingSetupChecker *checker,
                                    const Move *candidate,
                                    BlockingSetupResult *result) {
  const BlockingSetupSamples *samples = checker->samples;
  assert(samples != NULL);
  assert(move_get_type(candidate) == GAME_EVENT_TILE_PLACEMENT_MOVE ||
         move_get_type(candidate) == GAME_EVENT_EXCHANGE);
  Rack leave;
  blocking_setup_candidate_leave(checker->base, candidate, &leave);
  int missing = RACK_SIZE - rack_get_total_letters(&leave);
  if (missing > checker->bag_tiles) {
    missing = checker->bag_tiles;
  }
  double pass_reply_sum = 0.0;
  double candidate_reply_sum = 0.0;
  double pass_followup_sum = 0.0;
  double candidate_followup_sum = 0.0;
  int terminal_replies = 0;
  for (int rack_idx = 0; rack_idx < samples->num_racks; rack_idx++) {
    checker_copy_game(&checker->after, checker->base);
    Game *after = checker->after;
    play_move(candidate, after, NULL);
    pass_reply_sum += checker->pass_reply_scores[rack_idx];
    if (!game_over(after)) {
      candidate_reply_sum += checker_play_reply(
          checker, after, &samples->opponent_racks[rack_idx]);
      terminal_replies += game_over(after);
    }
    const MachineLetter *order =
        samples->draw_orders +
        ((size_t)rack_idx * (size_t)samples->pool_capacity);
    const int draw_size = samples->draw_sizes[rack_idx];
    const int draw_count = missing < draw_size ? missing : draw_size;
    for (int draw_idx = 0; draw_idx < checker->followup_draws; draw_idx++) {
      Rack followup;
      rack_copy(&followup, &leave);
      for (int tile_idx = 0; tile_idx < draw_count; tile_idx++) {
        rack_add_letter(&followup,
                        order[(draw_idx * draw_count + tile_idx) % draw_size]);
      }
      candidate_followup_sum += checker_followup(checker, after, &followup);
      pass_followup_sum +=
          checker_followup(checker, checker->pass_replied[rack_idx], &followup);
    }
  }
  const double num_racks = (double)samples->num_racks;
  const double num_followups = num_racks * checker->followup_draws;
  result->pass_reply_mean = pass_reply_sum / num_racks;
  result->candidate_reply_mean = candidate_reply_sum / num_racks;
  result->blocking_delta = (pass_reply_sum - candidate_reply_sum) / num_racks;
  result->pass_followup_mean = pass_followup_sum / num_followups;
  result->candidate_followup_mean = candidate_followup_sum / num_followups;
  result->setup_delta =
      (candidate_followup_sum - pass_followup_sum) / num_followups;
  result->terminal_replies = terminal_replies;
}
