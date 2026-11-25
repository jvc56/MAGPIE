#include "outplay_check.h"

#include "../def/board_defs.h"
#include "../def/equity_defs.h"
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
#include "../ent/outplay_check_weights.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "gameplay.h"
#include "move_gen.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  OUTPLAY_CHECK_CANDIDATES = 8,
  OUTPLAY_CHECK_MAX_BAG = 6,
  OUTPLAY_CHECK_MAX_DRAWS = 128,
  // Every play of a rack on an endgame board fits: the move generator stops
  // recording past a list's capacity.
  OUTPLAY_CHECK_MOVE_LIST_CAPACITY = 20000,
};

// The draws of the bag, as counts of each letter, and their weights.
typedef struct OutplayDraws {
  int count;
  int dist_size;
  uint8_t counts[OUTPLAY_CHECK_MAX_DRAWS][MAX_ALPHABET_SIZE];
  double weights[OUTPLAY_CHECK_MAX_DRAWS];
} OutplayDraws;

static double choose(int total, int taken) {
  double ways = 1.0;
  for (int taken_idx = 0; taken_idx < taken; taken_idx++) {
    ways = ways * (double)(total - taken_idx) / (double)(taken_idx + 1);
  }
  return ways;
}

// Every distinct draw of `remaining` more tiles from letter ml on, each with
// its number of ways. Counts past OUTPLAY_CHECK_MAX_DRAWS without keeping
// them, and stops once past it.
static void enumerate_draws(const int *unseen, int ml, int remaining,
                            uint8_t *current, double ways,
                            OutplayDraws *draws) {
  if (draws->count > OUTPLAY_CHECK_MAX_DRAWS) {
    return;
  }
  if (remaining == 0) {
    if (draws->count < OUTPLAY_CHECK_MAX_DRAWS) {
      memcpy(draws->counts[draws->count], current, (size_t)draws->dist_size);
      draws->weights[draws->count] = ways;
    }
    draws->count++;
    return;
  }
  if (ml == draws->dist_size) {
    return;
  }
  const int most = unseen[ml] < remaining ? unseen[ml] : remaining;
  for (int taken = 0; taken <= most; taken++) {
    current[ml] = (uint8_t)taken;
    enumerate_draws(unseen, ml + 1, remaining - taken, current,
                    ways * choose(unseen[ml], taken), draws);
  }
  current[ml] = 0;
}

// SplitMix64, for sampling draws reproducibly.
static uint64_t next_random(uint64_t *state) {
  *state += UINT64_C(0x9E3779B97F4A7C15);
  uint64_t mixed = *state;
  mixed = (mixed ^ (mixed >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
  mixed = (mixed ^ (mixed >> 27)) * UINT64_C(0x94D049BB133111EB);
  return mixed ^ (mixed >> 31);
}

// The draws of bag_size tiles from the unseen tiles: every distinct one with
// its probability when there are at most OUTPLAY_CHECK_MAX_DRAWS, and
// otherwise that many sampled ones, equally weighted, from a generator seeded
// with seed.
static void fill_draws(OutplayDraws *draws, const int *unseen, int bag_size,
                       uint64_t seed) {
  uint8_t current[MAX_ALPHABET_SIZE] = {0};
  draws->count = 0;
  int total = 0;
  for (int ml = 0; ml < draws->dist_size; ml++) {
    total += unseen[ml];
  }
  enumerate_draws(unseen, 0, bag_size, current, 1.0, draws);
  if (draws->count <= OUTPLAY_CHECK_MAX_DRAWS) {
    const double all_ways = choose(total, bag_size);
    for (int draw_idx = 0; draw_idx < draws->count; draw_idx++) {
      draws->weights[draw_idx] /= all_ways;
    }
    return;
  }
  MachineLetter tiles[MAX_ALPHABET_SIZE * RACK_SIZE * 4];
  int num_tiles = 0;
  for (int ml = 0; ml < draws->dist_size; ml++) {
    for (int copy_idx = 0; copy_idx < unseen[ml]; copy_idx++) {
      tiles[num_tiles++] = (MachineLetter)ml;
    }
  }
  uint64_t state = seed;
  for (int draw_idx = 0; draw_idx < OUTPLAY_CHECK_MAX_DRAWS; draw_idx++) {
    uint8_t *counts = draws->counts[draw_idx];
    memset(counts, 0, (size_t)draws->dist_size);
    for (int pick_idx = 0; pick_idx < bag_size && pick_idx < num_tiles;
         pick_idx++) {
      const int swap_idx = pick_idx + (int)(next_random(&state) %
                                            (uint64_t)(num_tiles - pick_idx));
      const MachineLetter picked = tiles[swap_idx];
      tiles[swap_idx] = tiles[pick_idx];
      tiles[pick_idx] = picked;
      counts[picked]++;
    }
    draws->weights[draw_idx] = 1.0 / (double)OUTPLAY_CHECK_MAX_DRAWS;
  }
  draws->count = OUTPLAY_CHECK_MAX_DRAWS;
}

// The rack left after move.
static void leave_of_move(const Rack *rack, const Move *move, Rack *leave) {
  rack_copy(leave, rack);
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    const MachineLetter ml = move_get_tile(move, tile_idx);
    if (ml != PLAYED_THROUGH_MARKER) {
      rack_take_letter(leave, get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
    }
  }
}

// Every play of the player on turn in game, by score, into plays.
static void generate_all_plays(Game *game, MoveList *plays) {
  const MoveGenArgs args = {
      .game = game,
      .move_list = plays,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&args);
}

// The features of `after` (the opponent on turn, the mover holding held):
// what the opponent can play, and then the mover.
static void find_features(Game *after, int mover_idx, const Rack *held,
                          MoveList *plays, OutplayCheckFeatures *features) {
  const int opp_idx = 1 - mover_idx;
  const int opp_tiles =
      rack_get_total_letters(player_get_rack(game_get_player(after, opp_idx)));
  *features = (OutplayCheckFeatures){0};
  game_set_player_on_turn_index(after, opp_idx);
  generate_all_plays(after, plays);
  for (int play_idx = 0; play_idx < move_list_get_count(plays); play_idx++) {
    const Move *play = move_list_get_move(plays, play_idx);
    if (move_get_type(play) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    const int score = equity_to_int(move_get_score(play));
    if (score > features->opponent_best_score) {
      features->opponent_best_score = score;
    }
    if (move_get_tiles_played(play) == opp_tiles &&
        (!features->opponent_can_go_out ||
         score > features->opponent_best_out_score)) {
      features->opponent_best_out_score = score;
      features->opponent_can_go_out = true;
    }
  }
  const int held_tiles = rack_get_total_letters(held);
  game_set_player_on_turn_index(after, mover_idx);
  generate_all_plays(after, plays);
  game_set_player_on_turn_index(after, opp_idx);
  uint64_t lanes = 0;
  for (int play_idx = 0; play_idx < move_list_get_count(plays); play_idx++) {
    const Move *play = move_list_get_move(plays, play_idx);
    if (move_get_type(play) != GAME_EVENT_TILE_PLACEMENT_MOVE ||
        move_get_tiles_played(play) != held_tiles) {
      continue;
    }
    const int score = equity_to_int(move_get_score(play));
    if (features->own_out_plays == 0 || score > features->own_best_out_score) {
      features->own_best_out_score = score;
    }
    features->own_out_plays++;
    const bool vertical = board_is_dir_vertical(move_get_dir(play));
    lanes |= UINT64_C(1) << (vertical ? BOARD_DIM + move_get_col_start(play)
                                      : move_get_row_start(play));
  }
  features->own_out_lanes = __builtin_popcountll(lanes);
}

// The scratch one choice needs.
typedef struct OutplayScratch {
  MoveList *candidates;
  MoveList *plays;
  Game *after;
  OutplayDraws draws;
} OutplayScratch;

// The mean value, over the draws, of the position after candidate, which
// empties the bag.
static double candidate_model_value(OutplayScratch *scratch, const Game *game,
                                    const Move *candidate, const Rack *leave,
                                    const int *unseen,
                                    const OutplayCheckWeights *weights) {
  const LetterDistribution *ld = game_get_ld(game);
  const int dist_size = ld_get_size(ld);
  const int mover_idx = game_get_player_on_turn_index(game);
  Game *after = scratch->after;
  game_copy(after, game);
  play_move(candidate, after, NULL);
  // Whatever play_move drew, the mover now holds its leave and a draw, the
  // opponent the rest of the unseen tiles, and the bag is empty.
  const MachineLetter no_tiles[1] = {0};
  bag_set_to_tiles(game_get_bag(after), no_tiles, 0);
  Rack *held = player_get_rack(game_get_player(after, mover_idx));
  Rack *opp_held = player_get_rack(game_get_player(after, 1 - mover_idx));
  double model = 0.0;
  for (int draw_idx = 0; draw_idx < scratch->draws.count; draw_idx++) {
    const uint8_t *counts = scratch->draws.counts[draw_idx];
    rack_copy(held, leave);
    rack_set_dist_size_and_reset(opp_held, dist_size);
    for (int ml = 0; ml < dist_size; ml++) {
      if (counts[ml] > 0) {
        rack_add_letters(held, (MachineLetter)ml, counts[ml]);
      }
      if (unseen[ml] > counts[ml]) {
        rack_add_letters(opp_held, (MachineLetter)ml, unseen[ml] - counts[ml]);
      }
    }
    OutplayCheckFeatures features;
    find_features(after, mover_idx, held, scratch->plays, &features);
    model += scratch->draws.weights[draw_idx] *
             outplay_check_weights_value(weights, ld, held, &features);
  }
  return model + equity_to_double(move_get_score(candidate));
}

void outplay_check_choose(Game *game, const MoveList *move_list,
                          const OutplayCheckWeights *weights) {
  const LetterDistribution *ld = game_get_ld(game);
  if (move_list_get_count(move_list) == 0 ||
      !strings_equal(outplay_check_weights_get_ld_name(weights),
                     ld_get_name(ld))) {
    return;
  }
  const int bag_size = game_get_real_bag_size(game);
  if (bag_size < 1 || bag_size > OUTPLAY_CHECK_MAX_BAG) {
    return;
  }
  const int mover_idx = game_get_player_on_turn_index(game);
  const Rack *rack = player_get_rack(game_get_player(game, mover_idx));
  const Rack *opp_rack = player_get_rack(game_get_player(game, 1 - mover_idx));
  if (rack_get_total_letters(rack) != RACK_SIZE) {
    return;
  }
  // The unseen tiles: the bag's and the opponent's, known or not.
  const Bag *bag = game_get_bag(game);
  const int dist_size = ld_get_size(ld);
  int unseen[MAX_ALPHABET_SIZE] = {0};
  int total_unseen = 0;
  uint64_t seed = (uint64_t)bag_size;
  for (int ml = 0; ml < dist_size; ml++) {
    unseen[ml] = bag_get_letter(bag, (MachineLetter)ml) +
                 rack_get_letter(opp_rack, (MachineLetter)ml);
    total_unseen += unseen[ml];
    seed = seed * UINT64_C(1099511628211) + (uint64_t)unseen[ml] * 31 +
           (uint64_t)rack_get_letter(rack, (MachineLetter)ml);
  }
  if (total_unseen != bag_size + RACK_SIZE) {
    return;
  }
  OutplayScratch *scratch = malloc_or_die(sizeof(OutplayScratch));
  scratch->candidates = move_list_create(OUTPLAY_CHECK_MOVE_LIST_CAPACITY);
  scratch->plays = move_list_create(OUTPLAY_CHECK_MOVE_LIST_CAPACITY);
  scratch->after = game_duplicate(game);
  scratch->draws.count = 0;
  scratch->draws.dist_size = dist_size;
  const MoveGenArgs candidate_args = {
      .game = game,
      .move_list = scratch->candidates,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&candidate_args);
  move_list_sort_moves(scratch->candidates);
  int num_candidates = move_list_get_count(scratch->candidates);
  if (num_candidates > OUTPLAY_CHECK_CANDIDATES) {
    num_candidates = OUTPLAY_CHECK_CANDIDATES;
  }
  bool draws_ready = false;
  int best_idx = 0;
  double best_value = 0.0;
  bool any_valued = false;
  for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
    const Move *candidate = move_list_get_move(scratch->candidates, cand_idx);
    double value = equity_to_double(move_get_equity(candidate));
    if (move_get_type(candidate) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
        move_get_tiles_played(candidate) >= bag_size) {
      if (!draws_ready) {
        fill_draws(&scratch->draws, unseen, bag_size, seed);
        draws_ready = true;
      }
      Rack leave;
      leave_of_move(rack, candidate, &leave);
      value = candidate_model_value(scratch, game, candidate, &leave, unseen,
                                    weights) -
              outplay_check_weights_get_center(weights, bag_size,
                                               rack_get_total_letters(&leave));
      any_valued = true;
    }
    if (cand_idx == 0 || value > best_value) {
      best_idx = cand_idx;
      best_value = value;
    }
  }
  if (any_valued && best_idx != 0) {
    move_copy(move_list_get_move(move_list, 0),
              move_list_get_move(scratch->candidates, best_idx));
  }
  game_destroy(scratch->after);
  move_list_destroy(scratch->plays);
  move_list_destroy(scratch->candidates);
  free(scratch);
}
