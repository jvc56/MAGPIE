#include "outplay_check.h"

#include "../def/equity_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/rack_defs.h"
#include "../ent/bag.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/lane_shadow_cache.h"
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
  // Points added to a play that uses the whole rack, so that a best-play
  // generation ranks every such play above every other (as the endgame's
  // first reply does).
  OUTPLAY_CHECK_GOING_OUT_PRIORITY = 100000,
  // The size of the lane shadow cache one choice's best-play generations
  // share (see lane_shadow_cache.h): 2^13 lane entries and 2^14 lane and
  // anchor plays, 1 MB in all.
  OUTPLAY_CHECK_LANE_SHADOW_INDEX_BITS = 13,
  OUTPLAY_CHECK_ANCHOR_PLAY_INDEX_BITS = 14,
  // The model reads whether the mover's out plays lie in 2 lanes or 3 or
  // more, so the lanes are counted up to 3.
  OUTPLAY_CHECK_OUT_LANES_COUNTED = 3,
};

// The draws of the bag, as counts of each letter, and their weights.
typedef struct OutplayDraws {
  int count;
  int dist_size;
  uint8_t counts[OUTPLAY_CHECK_MAX_DRAWS][MAX_ALPHABET_SIZE];
  double weights[OUTPLAY_CHECK_MAX_DRAWS];
  // The first draw with the same tiles as each: itself, but for a sampled
  // draw that repeats an earlier one.
  int first_same[OUTPLAY_CHECK_MAX_DRAWS];
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
      draws->first_same[draw_idx] = draw_idx;
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
    draws->first_same[draw_idx] = draw_idx;
    for (int earlier_idx = 0; earlier_idx < draw_idx; earlier_idx++) {
      if (memcmp(draws->counts[earlier_idx], counts,
                 (size_t)draws->dist_size) == 0) {
        draws->first_same[draw_idx] = earlier_idx;
        break;
      }
    }
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

// The check's best-play generations: by score, a play that uses the whole
// rack ranking above every play that does not, the result in best, and what
// earlier generations found for lanes that recur with the same rack taken
// from lane_cache (a draw's racks are the same after every candidate, and a
// candidate's play changes only a few lanes of the board).
static MoveGenArgs best_play_args(Game *game, MoveList *best,
                                  LaneShadowCache *lane_cache) {
  return (MoveGenArgs){
      .game = game,
      .move_list = best,
      .move_record_type = MOVE_RECORD_BEST_SMALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .best_small_out_bonus = int_to_equity(OUTPLAY_CHECK_GOING_OUT_PRIORITY),
      .best_small_canonical_ties = true,
      .lane_shadow_cache = lane_cache,
  };
}

// The best play of the player on turn in game, going out ranking first: the
// best way to go out when there is one, and otherwise the best scoring play;
// a pass when the player has no play.
static const SmallMove *best_play_going_out_first(Game *game, MoveList *best,
                                                  LaneShadowCache *lane_cache,
                                                  const Rack *pool) {
  const MoveGenArgs args = best_play_args(game, best, lane_cache);
  generate_best_small_move_with_pool(&args, pool);
  return best->small_moves[0];
}

// The features of `after` (the opponent on turn, the mover to play next):
// what the opponent can play, and then the mover. Best-play generations answer
// them all: going out ranks first, so one generation for the opponent says
// whether it can go out and for how much, or else its best score; and one for
// the mover finds its best way to go out and counts the lanes that hold one.
static void find_features(Game *after, int mover_idx, MoveList *best,
                          LaneShadowCache *lane_cache, const Rack *opp_pool,
                          OutplayCheckFeatures *features) {
  const int opp_idx = 1 - mover_idx;
  const int opp_tiles =
      rack_get_total_letters(player_get_rack(game_get_player(after, opp_idx)));
  *features = (OutplayCheckFeatures){0};
  game_set_player_on_turn_index(after, opp_idx);
  const SmallMove *opp_best =
      best_play_going_out_first(after, best, lane_cache, opp_pool);
  if (!small_move_is_pass(opp_best)) {
    const int score = (int)small_move_get_score(opp_best);
    if (small_move_get_tiles_played(opp_best) == opp_tiles) {
      features->opponent_can_go_out = true;
      features->opponent_best_out_score = score;
    } else {
      features->opponent_best_score = score;
    }
  }
  game_set_player_on_turn_index(after, mover_idx);
  const MoveGenArgs own_args = best_play_args(after, best, NULL);
  int own_out_lanes = 0;
  if (generate_best_out_play_and_count_lanes(
          &own_args, OUTPLAY_CHECK_OUT_LANES_COUNTED, &own_out_lanes)) {
    features->own_can_go_out = true;
    features->own_best_out_score =
        (int)small_move_get_score(best->small_moves[0]);
    features->own_out_lanes = own_out_lanes;
  }
  game_set_player_on_turn_index(after, opp_idx);
}

// The scratch one choice needs.
typedef struct OutplayScratch {
  MoveList *candidates;
  MoveList *best;
  LaneShadowCache *lane_cache;
  // The position after each candidate that empties the bag.
  Game *afters[OUTPLAY_CHECK_CANDIDATES];
  OutplayDraws draws;
  // The model's value of each draw after each candidate that empties the bag,
  // for the draws that repeat it.
  double draw_values[OUTPLAY_CHECK_CANDIDATES][OUTPLAY_CHECK_MAX_DRAWS];
} OutplayScratch;

// For each of the num_emptying candidates in emptying, which empty the bag
// and leave leaves: its score plus the mean value, over the draws, of the
// position after it, into values. The draws are the outer loop, so that a
// draw's racks meet every candidate's board in turn while the lane cache
// holds what the earlier candidates found for them; each candidate's sum
// still adds the draws in their order. A sampled draw that repeats an earlier
// one takes that one's value.
static void emptying_candidate_values(OutplayScratch *scratch, const Game *game,
                                      const Move *const *emptying,
                                      const Rack *leaves, int num_emptying,
                                      const int *unseen,
                                      const OutplayCheckWeights *weights,
                                      double *values) {
  const LetterDistribution *ld = game_get_ld(game);
  const int dist_size = ld_get_size(ld);
  const int mover_idx = game_get_player_on_turn_index(game);
  // Whatever play_move draws, the mover then holds its leave and a draw, the
  // opponent the rest of the unseen tiles, and the bag is empty.
  const MachineLetter no_tiles[1] = {0};
  Rack opp_pool;
  rack_set_dist_size_and_reset(&opp_pool, dist_size);
  for (int ml = 0; ml < dist_size; ml++) {
    if (unseen[ml] > 0) {
      rack_add_letters(&opp_pool, (MachineLetter)ml, unseen[ml]);
    }
  }
  for (int empty_idx = 0; empty_idx < num_emptying; empty_idx++) {
    if (scratch->afters[empty_idx] == NULL) {
      scratch->afters[empty_idx] = game_duplicate(game);
    }
    Game *after = scratch->afters[empty_idx];
    game_copy(after, game);
    play_move(emptying[empty_idx], after, NULL);
    bag_set_to_tiles(game_get_bag(after), no_tiles, 0);
    values[empty_idx] = 0.0;
  }
  for (int draw_idx = 0; draw_idx < scratch->draws.count; draw_idx++) {
    const double weight = scratch->draws.weights[draw_idx];
    const int first_same = scratch->draws.first_same[draw_idx];
    if (first_same != draw_idx) {
      for (int empty_idx = 0; empty_idx < num_emptying; empty_idx++) {
        values[empty_idx] +=
            weight * scratch->draw_values[empty_idx][first_same];
      }
      continue;
    }
    const uint8_t *counts = scratch->draws.counts[draw_idx];
    for (int empty_idx = 0; empty_idx < num_emptying; empty_idx++) {
      Game *after = scratch->afters[empty_idx];
      Rack *held = player_get_rack(game_get_player(after, mover_idx));
      Rack *opp_held = player_get_rack(game_get_player(after, 1 - mover_idx));
      rack_copy(held, &leaves[empty_idx]);
      rack_set_dist_size_and_reset(opp_held, dist_size);
      for (int ml = 0; ml < dist_size; ml++) {
        if (counts[ml] > 0) {
          rack_add_letters(held, (MachineLetter)ml, counts[ml]);
        }
        if (unseen[ml] > counts[ml]) {
          rack_add_letters(opp_held, (MachineLetter)ml,
                           unseen[ml] - counts[ml]);
        }
      }
      OutplayCheckFeatures features;
      find_features(after, mover_idx, scratch->best, scratch->lane_cache,
                    &opp_pool, &features);
      const double draw_value =
          outplay_check_weights_value(weights, ld, held, &features);
      scratch->draw_values[empty_idx][draw_idx] = draw_value;
      values[empty_idx] += weight * draw_value;
    }
  }
  for (int empty_idx = 0; empty_idx < num_emptying; empty_idx++) {
    values[empty_idx] += equity_to_double(move_get_score(emptying[empty_idx]));
  }
}

void outplay_check_find_features(Game *game, OutplayCheckFeatures *features) {
  MoveList *best = move_list_create_small(1);
  LaneShadowCache *lane_cache =
      lane_shadow_cache_create(OUTPLAY_CHECK_LANE_SHADOW_INDEX_BITS,
                               OUTPLAY_CHECK_ANCHOR_PLAY_INDEX_BITS);
  const int mover_idx = 1 - game_get_player_on_turn_index(game);
  const Rack *opp_rack = player_get_rack(game_get_player(game, 1 - mover_idx));
  find_features(game, mover_idx, best, lane_cache, opp_rack, features);
  lane_shadow_cache_destroy(lane_cache);
  small_move_list_destroy(best);
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
  // The list keeps the best OUTPLAY_CHECK_CANDIDATES plays by equity, in the
  // order sorting every play would give them.
  scratch->candidates = move_list_create(OUTPLAY_CHECK_CANDIDATES);
  scratch->best = move_list_create_small(1);
  scratch->lane_cache = NULL;
  for (int empty_idx = 0; empty_idx < OUTPLAY_CHECK_CANDIDATES; empty_idx++) {
    scratch->afters[empty_idx] = NULL;
  }
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
  // A candidate's value is its static equity, unless it empties the bag.
  double values[OUTPLAY_CHECK_CANDIDATES];
  const Move *emptying[OUTPLAY_CHECK_CANDIDATES];
  Rack leaves[OUTPLAY_CHECK_CANDIDATES];
  int emptying_cand_idx[OUTPLAY_CHECK_CANDIDATES];
  int num_emptying = 0;
  for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
    const Move *candidate = move_list_get_move(scratch->candidates, cand_idx);
    values[cand_idx] = equity_to_double(move_get_equity(candidate));
    if (move_get_type(candidate) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
        move_get_tiles_played(candidate) >= bag_size) {
      emptying[num_emptying] = candidate;
      leave_of_move(rack, candidate, &leaves[num_emptying]);
      emptying_cand_idx[num_emptying] = cand_idx;
      num_emptying++;
    }
  }
  const bool any_valued = num_emptying > 0;
  if (any_valued) {
    fill_draws(&scratch->draws, unseen, bag_size, seed);
    scratch->lane_cache =
        lane_shadow_cache_create(OUTPLAY_CHECK_LANE_SHADOW_INDEX_BITS,
                                 OUTPLAY_CHECK_ANCHOR_PLAY_INDEX_BITS);
    double emptying_values[OUTPLAY_CHECK_CANDIDATES];
    emptying_candidate_values(scratch, game, emptying, leaves, num_emptying,
                              unseen, weights, emptying_values);
    for (int empty_idx = 0; empty_idx < num_emptying; empty_idx++) {
      values[emptying_cand_idx[empty_idx]] =
          emptying_values[empty_idx] -
          outplay_check_weights_get_center(
              weights, bag_size, rack_get_total_letters(&leaves[empty_idx]));
    }
  }
  int best_idx = 0;
  double best_value = 0.0;
  for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
    const double value = values[cand_idx];
    if (cand_idx == 0 || value > best_value) {
      best_idx = cand_idx;
      best_value = value;
    }
  }
  if (any_valued && best_idx != 0) {
    move_copy(move_list_get_move(move_list, 0),
              move_list_get_move(scratch->candidates, best_idx));
  }
  for (int empty_idx = 0; empty_idx < OUTPLAY_CHECK_CANDIDATES; empty_idx++) {
    game_destroy(scratch->afters[empty_idx]);
  }
  lane_shadow_cache_destroy(scratch->lane_cache);
  small_move_list_destroy(scratch->best);
  move_list_destroy(scratch->candidates);
  free(scratch);
}
