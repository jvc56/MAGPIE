#include "path_move_lists_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/move_undo.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/move_gen_base.h"
#include "../src/impl/path_move_lists.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Compare the complete lists in generation order, excluding the trailing pass
// that PathMoveLists deliberately leaves for its caller to append.
static void assert_matches_scratch(const SmallMove *derived, int count,
                                   const MoveGenArgs *args) {
  generate_moves(args);
  const MoveList *move_list = args->move_list;
  assert(count == move_list->count - 1);
  assert(small_move_is_pass(move_list->small_moves[count]));
  for (int move_idx = 0; move_idx < count; move_idx++) {
    const SmallMove *scratch = move_list->small_moves[move_idx];
    assert(derived[move_idx].tiny_move == scratch->tiny_move);
    assert(derived[move_idx].metadata.score == scratch->metadata.score);
    assert(derived[move_idx].metadata.play_length ==
           scratch->metadata.play_length);
    assert(derived[move_idx].metadata.tiles_played ==
           scratch->metadata.tiles_played);
  }
}

// On an empty board AA has plays and VV does not. Swap the racks to exercise
// both root selections, then follow passes far enough to derive from a slot.
// Empty cached lists are valid even when they have never allocated storage.
static void test_pass_path(int empty_side) {
  Config *config =
      config_create_or_die("set -lex NWL20 -wmp false -rit false -wit false");
  load_and_exec_config_or_die(config, "new");
  Game *game = config_get_game(config);
  MoveList *move_list = move_list_create_small(16);
  PathMoveLists *lists = path_move_lists_create();
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL_SMALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .target_equity = EQUITY_MAX_VALUE,
  };
  for (int side = 0; side < 2; side++) {
    Rack *rack = player_get_rack(game_get_player(game, side));
    const int tiles = rack_set_to_string(game_get_ld(game), rack,
                                         side == empty_side ? "VV" : "AA");
    assert(tiles == 2);
  }
  for (int side = 0; side < 2; side++) {
    game_set_player_on_turn_index(game, side);
    generate_moves(&args);
    assert((move_list->count == 1) == (side == empty_side));
    path_move_lists_set_root(lists, side, move_list);
  }

  game_set_player_on_turn_index(game, 0);
  Move pass;
  move_set_as_pass(&pass);
  for (int length = 1; length <= 4; length++) {
    path_move_lists_push(lists, game_get_board(game), &pass);
    play_move(&pass, game, NULL);
    // The no-regeneration path must work with an empty scratch list too.
    small_move_list_reset(move_list);
    const SmallMove *derived = NULL;
    const int count = path_move_lists_moves_at(lists, &args, &derived);
    assert((count == 0) == (length % 2 == empty_side));
    assert_matches_scratch(derived, count, &args);
  }
  path_move_lists_destroy(lists);
  small_move_list_destroy(move_list);
  config_destroy(config);
}

// Keep each parent list live while descending, then check it again after
// unplay. Replaying another sequence reuses the same slots for sibling paths.
static void assert_move_sequence(Game *game, PathMoveLists *lists,
                                 const MoveGenArgs *args,
                                 const char *const *moves) {
  if (*moves == NULL) {
    return;
  }
  ValidatedMoves *validated = validated_moves_create_and_assert_status(
      game, game_get_player_on_turn_index(game), *moves, false, false,
      ERROR_STATUS_SUCCESS);
  const Move *move = validated_moves_get_move(validated, 0);
  path_move_lists_push(lists, game_get_board(game), move);
  MoveUndo undo;
  play_move_incremental(move, game, &undo);
  if (undo.move_tiles_length > 0) {
    update_cross_set_for_move_from_undo(&undo, game);
    board_set_cross_sets_valid(game_get_board(game), true);
  }
  const SmallMove *derived = NULL;
  const int count = path_move_lists_moves_at(lists, args, &derived);
  assert_matches_scratch(derived, count, args);
  assert_move_sequence(game, lists, args, moves + 1);
  assert_matches_scratch(derived, count, args);
  unplay_move_incremental(game, &undo);
  path_move_lists_pop(lists);
  validated_moves_destroy(validated);
}

static void test_tile_placement_paths(void) {
  const char *positions[] = {
      "15/15/15/15/15/15/15/7AT6/15/15/15/15/15/15/15 CE?/RS? 0/0 0",
      "15/15/15/15/15/15/15/7A7/7T7/15/15/15/15/15/15 CE?/RS? 0/0 0",
  };
  const char *sequences[][2][7] = {
      {{"8H ATE", "8G RATE", "8F CRATE", "8F CRATES", "pass", "pass", NULL},
       {"8H ATE", "8G RATE", "8F CRATE", "8F CRATEs", "pass", "pass", NULL}},
      {{"H8 ATE", "H7 RATE", "H6 CRATE", "H6 CRATES", "pass", "pass", NULL},
       {"H8 ATE", "H7 RATE", "H6 CRATE", "H6 CRATEs", "pass", "pass", NULL}},
  };
  Config *config =
      config_create_or_die("set -lex NWL20 -wmp false -rit false -wit false");
  Game *game = config_game_create(config);
  MoveList *move_list = move_list_create_small(100000);
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL_SMALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .target_equity = EQUITY_MAX_VALUE,
  };
  PathMoveLists *lists = path_move_lists_create();
  for (int position_idx = 0; position_idx < 2; position_idx++) {
    load_cgp_or_die(game, positions[position_idx]);
    bag_set_to_tiles(game_get_bag(game), NULL, 0);
    Game *snapshot = game_duplicate(game);
    path_move_lists_reset(lists);
    for (int side = 0; side < 2; side++) {
      game_set_player_on_turn_index(game, side);
      generate_moves(&args);
      path_move_lists_set_root(lists, side, move_list);
    }
    game_set_player_on_turn_index(game, 0);
    // Using S versus a blank changes both the cross scores and the remaining
    // rack filter. The passes also exercise play/pass and pass/pass parents.
    for (int sequence_idx = 0; sequence_idx < 2; sequence_idx++) {
      assert_move_sequence(game, lists, &args,
                           sequences[position_idx][sequence_idx]);
      assert(lists->length == 0);
      assert_games_are_equal(game, snapshot, true);
    }
    game_destroy(snapshot);
  }
  path_move_lists_destroy(lists);
  small_move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

// After AA is placed in this synthetic empty-bag position, UU still has no
// plays. The solver must append a pass to an empty regenerated list. Disable
// the TT so exact greedy-leaf entries cannot bypass that interior node.
static void test_empty_derived_endgame(void) {
  Config *config =
      config_create_or_die("set -lex NWL20 -wmp false -rit false -wit false");
  load_and_exec_config_or_die(config, "new");
  Game *game = config_get_game(config);
  bag_set_to_tiles(game_get_bag(game), NULL, 0);
  game_set_player_on_turn_index(game, 0);
  for (int side = 0; side < 2; side++) {
    const int tiles = rack_set_to_string(
        game_get_ld(game), player_get_rack(game_get_player(game, side)),
        side == 0 ? "AAA" : "UU");
    assert(tiles == (side == 0 ? 3 : 2));
  }
  int32_t scores[2];
  uint64_t nodes[2];
  for (int incremental = 0; incremental < 2; incremental++) {
    const EndgameArgs args = {
        .game = game,
        .thread_control = config_get_thread_control(config),
        .plies = 2,
        .tt_fraction_of_mem = 0,
        .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
        .num_threads = 1,
        .num_top_moves = 1,
        .use_heuristics = true,
        .forced_pass_bypass = true,
        .incremental_movegen = incremental != 0,
        .seed = 42,
    };
    EndgameCtx *ctx = NULL;
    EndgameResults *results = config_get_endgame_results(config);
    ErrorStack *error_stack = error_stack_create();
    endgame_solve(&ctx, &args, results, error_stack);
    assert(error_stack_is_empty(error_stack));
    scores[incremental] =
        endgame_results_get_pvline(results, ENDGAME_RESULT_BEST)->score;
    nodes[incremental] = endgame_ctx_get_nodes_searched(ctx);
    endgame_ctx_destroy(ctx);
    error_stack_destroy(error_stack);
  }
  assert(scores[1] == scores[0]);
  assert(nodes[0] > 0);
  assert(nodes[1] == nodes[0]);
  config_destroy(config);
}

// ---------------------------------------------------------------------------
// path_move_lists_best_play: random-path comparison against the materialized
// list and against scratch generations.

enum {
  BEST_PLAY_NUM_POSITIONS = 3,
  BEST_PLAY_PATHS_PER_POSITION = 60,
  BEST_PLAY_MAX_PATH_LENGTH = 12,
  BEST_PLAY_BONUS_VARIANTS = 4,
  BEST_PLAY_PASS_ONE_IN = 7,
  BEST_PLAY_MIN_CHECKS = 500,
  BEST_PLAY_MIN_DEEPEST = 4,
  BEST_PLAY_SIBLINGS = 3,
  // How many lanes' whole-rack plays and other plays the replying side's
  // per-lane bests are compared over, at least.
  REPLY_LANES_MIN_WHOLE_RACK = 1000,
  REPLY_LANES_MIN_PARTIAL = 1500,
  // test_tiles_played_in_lanes_random_paths: its positions, and how often at
  // least a move's changed squares had to reveal a tile type over its checks.
  TILES_PLAYED_NUM_POSITIONS = 3,
  TILES_PLAYED_PATHS_PER_POSITION = 400,
  TILES_PLAYED_MIN_REVEALED = 20,
  // How many picks path_move_lists_penalized_best_play is compared over, at
  // least, in each of the two tests that check it.
  PENALIZED_MIN_PICKS = 1000,
  PENALIZED_SCALES = 3,
  // How often a tile type's only plays had to be standing ones, at least.
  TILES_PLAYED_MIN_STANDING = 250,
  BEST_PLAY_MOVE_LIST_CAPACITY = 100000,
};

static uint64_t next_random(uint64_t *state) {
  uint64_t value = *state;
  value ^= value << 13;
  value ^= value >> 7;
  value ^= value << 17;
  *state = value;
  return value;
}

// Whether `candidate` outranks the running best under the shared tie-break:
// higher adjusted score, then larger tiny_move.
static bool outranks(int adjusted, const SmallMove *candidate, bool have_best,
                     int best_adjusted, const SmallMove *best) {
  return !have_best || adjusted > best_adjusted ||
         (adjusted == best_adjusted && candidate->tiny_move > best->tiny_move);
}

// The argmax of `count` plays (a trailing or interior pass never competes).
// Returns whether a play exists.
static bool argmax_of_plays(const SmallMove *const *plays, int count,
                            int go_out_bonus, int side_to_move_tiles,
                            SmallMove *out_best, int *out_adjusted) {
  bool found = false;
  int best_adjusted = 0;
  for (int move_idx = 0; move_idx < count; move_idx++) {
    const SmallMove *candidate = plays[move_idx];
    if (small_move_is_pass(candidate)) {
      continue;
    }
    int adjusted = small_move_get_score(candidate);
    if (small_move_get_tiles_played(candidate) == side_to_move_tiles) {
      adjusted += go_out_bonus;
    }
    if (outranks(adjusted, candidate, found, best_adjusted, out_best)) {
      found = true;
      best_adjusted = adjusted;
      *out_best = *candidate;
    }
  }
  *out_adjusted = best_adjusted;
  return found;
}

// The MOVE_RECORD_BEST_SMALL generation for the player on turn, with
// `go_out_bonus` points for a whole-rack play and the canonical tie-break
// path_move_lists_best_play reproduces.
static MoveGenArgs best_small_args_for(const Game *game, MoveList *move_list,
                                       int go_out_bonus) {
  const MoveGenArgs best_args = {
      .best_small_out_bonus = int_to_equity(go_out_bonus),
      .best_small_canonical_ties = true,
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_BEST_SMALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .target_equity = EQUITY_MAX_VALUE,
  };
  return best_args;
}

// Every best_play and best_play_alone answer at the current path position
// must agree with the argmax of the materialized list, of a scratch ALL_SMALL
// generation, and of a scratch BEST_SMALL generation. Counts in
// *num_split_lanes the lanes the materialized list both regenerated anchors
// of and carried anchors of.
static void assert_best_play_matches(PathMoveLists *lists,
                                     const MoveGenArgs *args,
                                     MoveList *best_move_list, uint64_t *rng,
                                     int *num_checks, int *num_split_lanes) {
  const Game *game = args->game;
  const int side_to_move_tiles = rack_get_total_letters(player_get_rack(
      game_get_player(game, game_get_player_on_turn_index(game))));
  const int bonuses[BEST_PLAY_BONUS_VARIANTS] = {
      0, 1, (int)(next_random(rng) % 40), (int)(next_random(rng) % 120)};

  // best_play and best_play_alone first: neither may need (or disturb) the
  // current slot. They take turns going first, since they share the lists'
  // scratch.
  SmallMove best_plays[BEST_PLAY_BONUS_VARIANTS];
  bool found[BEST_PLAY_BONUS_VARIANTS];
  SmallMove alone_plays[BEST_PLAY_BONUS_VARIANTS];
  bool alone_found[BEST_PLAY_BONUS_VARIANTS];
  for (int variant = 0; variant < BEST_PLAY_BONUS_VARIANTS; variant++) {
    const MoveGenArgs best_args =
        best_small_args_for(game, best_move_list, bonuses[variant]);
    if (variant % 2 == 0) {
      alone_found[variant] = path_move_lists_best_play_alone(
          lists, &best_args, &alone_plays[variant]);
    }
    found[variant] =
        path_move_lists_best_play(lists, &best_args, &best_plays[variant]);
    if (variant % 2 != 0) {
      alone_found[variant] = path_move_lists_best_play_alone(
          lists, &best_args, &alone_plays[variant]);
    }
  }

  const SmallMove *derived = NULL;
  const int count = path_move_lists_moves_at(lists, args, &derived);
  assert_matches_scratch(derived, count, args);
  const uint64_t pending = path_move_lists_pending_lanes(lists);
  for (int lane = 0; lane < PATH_MOVE_LISTS_NUM_LANES; lane++) {
    if (((pending >> lane) & 1) != 0 && lists->lane_generated[lane] != 0 &&
        lists->lane_generated[lane] != lists->lane_anchors[lane]) {
      (*num_split_lanes)++;
    }
  }
  const MoveList *scratch = args->move_list;
  const SmallMove **derived_pointers =
      malloc_or_die(((size_t)count + 1) * sizeof(const SmallMove *));
  for (int move_idx = 0; move_idx < count; move_idx++) {
    derived_pointers[move_idx] = &derived[move_idx];
  }

  for (int variant = 0; variant < BEST_PLAY_BONUS_VARIANTS; variant++) {
    SmallMove expected;
    small_move_set_as_pass(&expected);
    int expected_adjusted = 0;
    const bool expected_found = argmax_of_plays(
        (const SmallMove *const *)scratch->small_moves, scratch->count,
        bonuses[variant], side_to_move_tiles, &expected, &expected_adjusted);
    SmallMove from_derived;
    small_move_set_as_pass(&from_derived);
    int derived_adjusted = 0;
    const bool derived_found =
        argmax_of_plays(derived_pointers, count, bonuses[variant],
                        side_to_move_tiles, &from_derived, &derived_adjusted);
    assert(found[variant] == expected_found);
    assert(alone_found[variant] == expected_found);
    assert(derived_found == expected_found);
    if (expected_found) {
      assert(best_plays[variant].tiny_move == expected.tiny_move);
      assert(alone_plays[variant].tiny_move == expected.tiny_move);
      assert(small_move_get_score(&alone_plays[variant]) ==
             small_move_get_score(&expected));
      assert(from_derived.tiny_move == expected.tiny_move);
      assert(derived_adjusted == expected_adjusted);
    }

    // BEST_SMALL generation must pick the same play (a pass when none).
    const MoveGenArgs best_args =
        best_small_args_for(game, best_move_list, bonuses[variant]);
    generate_moves(&best_args);
    assert(best_move_list->count == 1);
    assert(best_move_list->small_moves[0]->tiny_move ==
           (expected_found ? expected.tiny_move : 0));
    (*num_checks)++;
  }
  free(derived_pointers);
}

// Weights for path_move_lists_penalized_best_play, different for most tile
// types.
static void set_tile_weights(int *tile_weights) {
  for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
    tile_weights[ml] = 2 + (ml * 5) % 7;
  }
}

// At the current path position, path_move_lists_penalized_best_play must
// pick the first play of a scratch generation with the highest score less its
// tiles' weights times the scale, for the scales 0 and 1 and one that
// `variety` selects. Adds the picks compared to *num_picks.
static void assert_penalized_best_play_matches(PathMoveLists *lists,
                                               const MoveGenArgs *args,
                                               const int *tile_weights,
                                               int variety, int *num_picks) {
  const float scales[PENALIZED_SCALES] = {0.0F, 1.0F,
                                          (float)(variety % 16) / 8.0F};
  for (int scale_idx = 0; scale_idx < PENALIZED_SCALES; scale_idx++) {
    const float scale = scales[scale_idx];
    SmallMove picked;
    int picked_adjusted = 0;
    const bool found = path_move_lists_penalized_best_play(
        lists, args, tile_weights, scale, &picked, &picked_adjusted);
    generate_moves(args);
    const MoveList *scratch = args->move_list;
    const SmallMove *expected = NULL;
    int expected_adjusted = 0;
    for (int move_idx = 0; move_idx < scratch->count; move_idx++) {
      const SmallMove *candidate = scratch->small_moves[move_idx];
      if (small_move_is_pass(candidate)) {
        continue;
      }
      int weight = 0;
      const int tiles_played = small_move_get_tiles_played(candidate);
      for (int tile_idx = 0; tile_idx < tiles_played; tile_idx++) {
        weight += tile_weights[small_move_tile_is_blank(candidate, tile_idx)
                                   ? BLANK_MACHINE_LETTER
                                   : small_move_get_tile(candidate, tile_idx)];
      }
      const int adjusted =
          (int)small_move_get_score(candidate) - (int)((float)weight * scale);
      if (expected == NULL || adjusted > expected_adjusted) {
        expected = candidate;
        expected_adjusted = adjusted;
      }
    }
    assert(found == (expected != NULL));
    if (expected != NULL) {
      assert(picked.tiny_move == expected->tiny_move);
      assert(small_move_get_score(&picked) == small_move_get_score(expected));
      assert(picked_adjusted == expected_adjusted);
      (*num_picks)++;
    }
  }
}

// Makes `chosen` along the path: pushes it on the lists, plays it, and brings
// the cross-sets up to date for the next generation.
static void make_path_move(Game *game, PathMoveLists *lists,
                           MoveList *move_list, const SmallMove *chosen,
                           MoveUndo *undo) {
  small_move_to_move(move_list->spare_move, chosen, game_get_board(game));
  path_move_lists_push(lists, game_get_board(game), move_list->spare_move);
  play_move_incremental(move_list->spare_move, game, undo);
  if (undo->move_tiles_length > 0) {
    update_cross_set_for_move_from_undo(undo, game);
  }
  board_set_cross_sets_valid(game_get_board(game), true);
}

// The lane of a play as PathMoveLists numbers them: its row, or past the rows
// its column when it is vertical.
static int lane_of_play(const SmallMove *small_move) {
  return small_move_is_vertical(small_move)
             ? BOARD_DIM + small_move_get_col_start(small_move)
             : small_move_get_row_start(small_move);
}

// Each lane's best whole-rack play and best other play among the plays of
// `move_list`, for a side holding `side_to_move_tiles` tiles: the highest
// score, then the larger tiny_move.
static void lane_bests_of_scratch(const MoveList *move_list,
                                  int side_to_move_tiles,
                                  LaneBestPlays *lanes) {
  memset(lanes, 0, PATH_MOVE_LISTS_NUM_LANES * sizeof(LaneBestPlays));
  for (int move_idx = 0; move_idx < move_list->count; move_idx++) {
    const SmallMove *candidate = move_list->small_moves[move_idx];
    if (small_move_is_pass(candidate)) {
      continue;
    }
    LaneBestPlays *best = &lanes[lane_of_play(candidate)];
    const int score = small_move_get_score(candidate);
    if (small_move_get_tiles_played(candidate) == side_to_move_tiles) {
      if (outranks(score, candidate, best->has_whole_rack,
                   small_move_get_score(&best->whole_rack),
                   &best->whole_rack)) {
        best->whole_rack = *candidate;
        best->has_whole_rack = true;
      }
    } else if (outranks(score, candidate, best->has_partial,
                        small_move_get_score(&best->partial), &best->partial)) {
      best->partial = *candidate;
      best->has_partial = true;
    }
  }
}

static void assert_lane_bests_equal(const LaneBestPlays *actual,
                                    const LaneBestPlays *expected) {
  for (int lane = 0; lane < PATH_MOVE_LISTS_NUM_LANES; lane++) {
    assert(actual[lane].has_whole_rack == expected[lane].has_whole_rack);
    if (expected[lane].has_whole_rack) {
      assert(actual[lane].whole_rack.tiny_move ==
             expected[lane].whole_rack.tiny_move);
      assert(small_move_get_score(&actual[lane].whole_rack) ==
             small_move_get_score(&expected[lane].whole_rack));
    }
    assert(actual[lane].has_partial == expected[lane].has_partial);
    if (expected[lane].has_partial) {
      assert(actual[lane].partial.tiny_move ==
             expected[lane].partial.tiny_move);
      assert(small_move_get_score(&actual[lane].partial) ==
             small_move_get_score(&expected[lane].partial));
    }
  }
}

// The per-lane best plays of the side not on turn, on the current board:
// path_move_lists_reply_lane_bests must find, and
// path_move_lists_lane_bests_of_list pick out of a scratch generation, the
// plays this file picks out of that generation. Adds the lanes compared with
// a whole-rack play and with another play to the two counts.
static void assert_reply_lane_bests_match(PathMoveLists *lists, Game *game,
                                          const MoveGenArgs *args,
                                          MoveList *best_move_list,
                                          int *num_whole_rack,
                                          int *num_partial) {
  const int on_turn = game_get_player_on_turn_index(game);
  const int replying_tiles = rack_get_total_letters(
      player_get_rack(game_get_player(game, 1 - on_turn)));
  game_set_player_on_turn_index(game, 1 - on_turn);
  const MoveGenArgs reply_args = best_small_args_for(game, best_move_list, 0);
  const LaneBestPlays *from_lists =
      path_move_lists_reply_lane_bests(lists, &reply_args);
  assert(from_lists != NULL);
  generate_moves(args);
  game_set_player_on_turn_index(game, on_turn);
  LaneBestPlays expected[PATH_MOVE_LISTS_NUM_LANES];
  lane_bests_of_scratch(args->move_list, replying_tiles, expected);
  LaneBestPlays from_list[PATH_MOVE_LISTS_NUM_LANES];
  path_move_lists_lane_bests_of_list(args->move_list, replying_tiles,
                                     from_list);
  assert_lane_bests_equal(from_list, expected);
  assert_lane_bests_equal(from_lists, expected);
  for (int lane = 0; lane < PATH_MOVE_LISTS_NUM_LANES; lane++) {
    *num_whole_rack += expected[lane].has_whole_rack ? 1 : 0;
    *num_partial += expected[lane].has_partial ? 1 : 0;
  }
}

static void test_best_play_random_paths(void) {
  const char *positions[BEST_PLAY_NUM_POSITIONS] = {
      "15/15/15/15/15/15/15/6CAT6/15/15/15/15/15/15/15 AEINRST/DEILNOR 0/0 0",
      ("15/15/15/15/15/15/15/5JOGS6/15/6HE7/15/15/15/15/15 ?AEIRST/BDEOUY? "
       "0/0 0"),
      "15/15/15/15/15/15/15/7AT6/15/15/15/15/15/15/15 CE?/RS? 0/0 0",
  };
  Config *config =
      config_create_or_die("set -lex NWL20 -wmp false -rit false -wit false");
  Game *game = config_game_create(config);
  MoveList *move_list = move_list_create_small(100000);
  MoveList *best_move_list =
      move_list_create_small(BEST_PLAY_MOVE_LIST_CAPACITY);
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL_SMALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .target_equity = EQUITY_MAX_VALUE,
  };
  PathMoveLists *lists = path_move_lists_create();
  uint64_t rng = 0x9E3779B97F4A7C15ULL;
  int num_checks = 0;
  int num_split_lanes = 0;
  int num_reply_whole_rack = 0;
  int num_reply_partial = 0;
  int num_penalized = 0;
  int tile_weights[MAX_ALPHABET_SIZE];
  set_tile_weights(tile_weights);
  int deepest = 0;
  bool saw_filter_case = false;
  for (int position_idx = 0; position_idx < BEST_PLAY_NUM_POSITIONS;
       position_idx++) {
    load_cgp_or_die(game, positions[position_idx]);
    bag_set_to_tiles(game_get_bag(game), NULL, 0);
    Game *snapshot = game_duplicate(game);
    for (int path_idx = 0; path_idx < BEST_PLAY_PATHS_PER_POSITION;
         path_idx++) {
      path_move_lists_reset(lists);
      for (int side = 0; side < 2; side++) {
        game_set_player_on_turn_index(game, side);
        generate_moves(&args);
        path_move_lists_set_root(lists, side, move_list);
      }
      game_set_player_on_turn_index(game, 0);
      MoveUndo undos[BEST_PLAY_MAX_PATH_LENGTH];
      int length = 0;
      while (length < BEST_PLAY_MAX_PATH_LENGTH &&
             game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
        // At every other position the replying side's per-lane bests are
        // found before any move is made, which the positions below then
        // share; at the others those positions find their own.
        if ((path_idx + length) % 2 == 1) {
          assert_reply_lane_bests_match(lists, game, &args, best_move_list,
                                        &num_reply_whole_rack,
                                        &num_reply_partial);
        }
        generate_moves(&args);
        const int play_count = move_list->count - 1;
        // Mostly tile plays, occasionally the trailing pass. The first pick
        // continues the path; the others are its siblings.
        SmallMove picks[BEST_PLAY_SIBLINGS + 1];
        for (int pick_idx = 0; pick_idx <= BEST_PLAY_SIBLINGS; pick_idx++) {
          int pick = play_count;
          if (play_count > 0 &&
              next_random(&rng) % BEST_PLAY_PASS_ONE_IN != 0) {
            pick = (int)(next_random(&rng) % (uint64_t)play_count);
          }
          picks[pick_idx] = *move_list->small_moves[pick];
        }
        // Siblings share the parent, so they reuse each other's per-lane best
        // plays in best_play.
        for (int sibling_idx = 1; sibling_idx <= BEST_PLAY_SIBLINGS;
             sibling_idx++) {
          make_path_move(game, lists, move_list, &picks[sibling_idx],
                         &undos[length]);
          if (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
            assert_best_play_matches(lists, &args, best_move_list, &rng,
                                     &num_checks, &num_split_lanes);
          }
          unplay_move_incremental(game, &undos[length]);
          path_move_lists_pop(lists);
        }
        make_path_move(game, lists, move_list, &picks[0], &undos[length]);
        length++;
        if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
          break;
        }
        if (length >= 2 && lists->played[length - 2]) {
          saw_filter_case = true;
        }
        assert_penalized_best_play_matches(
            lists, &args, tile_weights, path_idx + 3 * length, &num_penalized);
        assert_best_play_matches(lists, &args, best_move_list, &rng,
                                 &num_checks, &num_split_lanes);
        if (length > deepest) {
          deepest = length;
        }
      }
      while (length > 0) {
        length--;
        unplay_move_incremental(game, &undos[length]);
        path_move_lists_pop(lists);
      }
      assert(lists->length == 0);
      assert_games_are_equal(game, snapshot, true);
    }
    game_destroy(snapshot);
  }
  assert(saw_filter_case);
  assert(deepest >= BEST_PLAY_MIN_DEEPEST);
  assert(num_checks > BEST_PLAY_MIN_CHECKS);
  // Touched lanes that carried some anchors' plays and regenerated others,
  // merging the two.
  assert(num_split_lanes > 0);
  assert(num_reply_whole_rack >= REPLY_LANES_MIN_WHOLE_RACK);
  assert(num_reply_partial >= REPLY_LANES_MIN_PARTIAL);
  assert(num_penalized >= PENALIZED_MIN_PICKS);
  path_move_lists_destroy(lists);
  small_move_list_destroy(best_move_list);
  small_move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

// The rack tile types side `side` plays on the current board, by a
// MOVE_RECORD_TILES_PLAYED generation over the whole board, or with
// `lane_hits` non-NULL over the anchors of `lanes` reaching those squares.
static uint64_t side_tiles_played(Game *game, MoveList *move_list, int side,
                                  uint64_t lanes, const uint32_t *lane_hits) {
  const int on_turn = game_get_player_on_turn_index(game);
  game_set_player_on_turn_index(game, side);
  uint64_t tiles_played = 0;
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_TILES_PLAYED,
      .move_sort_type = MOVE_SORT_SCORE,
      .target_equity = EQUITY_MAX_VALUE,
      .tiles_played_bv = &tiles_played,
  };
  if (lane_hits == NULL) {
    generate_moves(&args);
  } else {
    generate_tiles_played_in_lanes(&args, lanes, lane_hits);
  }
  game_set_player_on_turn_index(game, on_turn);
  return tiles_played;
}

// The rack tile types side `side` can play as a single tile (a designated
// blank counts as the blank).
static uint64_t side_single_tile_types(Game *game, MoveList *move_list,
                                       int side) {
  const int on_turn = game_get_player_on_turn_index(game);
  game_set_player_on_turn_index(game, side);
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL_SMALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&args);
  uint64_t tile_types = 0;
  for (int move_idx = 0; move_idx < move_list->count; move_idx++) {
    const SmallMove *small_move = move_list->small_moves[move_idx];
    if (small_move_is_pass(small_move) ||
        small_move_get_tiles_played(small_move) != 1) {
      continue;
    }
    tile_types |= (uint64_t)1 << (small_move_tile_is_blank(small_move, 0)
                                      ? BLANK_MACHINE_LETTER
                                      : small_move_get_tile(small_move, 0));
  }
  game_set_player_on_turn_index(game, on_turn);
  return tile_types;
}

// The tile types side `side` plays on the current board, `playable`, are
// those its standing plays place (path_move_lists_standing_tile_types), those
// of the plays from the anchors that reach a square changed since
// (generate_tiles_played_in_lanes over the lanes and squares it reports), and
// those of its single-tile plays, `single`. Counts in *num_standing the
// positions where only standing plays placed some tile type.
static void assert_standing_tile_types(const PathMoveLists *lists, Game *game,
                                       MoveList *move_list, int side,
                                       uint64_t playable, uint64_t single,
                                       int *num_standing) {
  const Rack *rack = player_get_rack(game_get_player(game, side));
  uint64_t rack_types = 0;
  for (int ml = 0; ml < rack_get_dist_size(rack); ml++) {
    if (rack_get_letter(rack, ml) > 0) {
      rack_types |= (uint64_t)1 << ml;
    }
  }
  const bool side_is_on_turn = side == game_get_player_on_turn_index(game);
  uint64_t touched = 0;
  uint32_t hits[PATH_MOVE_LISTS_NUM_LANES];
  const uint64_t standing = path_move_lists_standing_tile_types(
      lists, side_is_on_turn, rack, rack_types, &touched, hits);
  assert((standing & ~playable) == 0);
  const uint64_t in_lanes =
      touched == 0 ? 0
                   : side_tiles_played(game, move_list, side, touched, hits);
  assert((standing | in_lanes | single) == playable);
  if ((standing & ~(in_lanes | single)) != 0) {
    (*num_standing)++;
  }
  // Asked about fewer tile types, it answers the same about those.
  const uint64_t wanted = rack_types & ~single;
  uint64_t wanted_touched = 0;
  uint32_t wanted_hits[PATH_MOVE_LISTS_NUM_LANES];
  assert(path_move_lists_standing_tile_types(
             lists, side_is_on_turn, rack, wanted, &wanted_touched,
             wanted_hits) == (standing & wanted));
  assert(wanted_touched == touched);
  assert(memcmp(wanted_hits, hits, sizeof(hits)) == 0);
}

// generate_tiles_played_in_lanes along random paths. Over every lane with
// every square changed it is the whole-board generation. Over the last move's
// changed squares (path_move_lists_last_move_hits), for each side: every tile
// type it reports is playable, and a tile type that had no play before the
// move but has one after has a single-tile play or one it reports -- what the
// endgame's stuck-tile check relies on. So is what
// path_move_lists_standing_tile_types reports, with the lists built at some
// positions and not at others (see assert_standing_tile_types).
static void test_tiles_played_in_lanes_random_paths(void) {
  // Endgames where the opponent holds tiles with no play.
  const char *positions[TILES_PLAYED_NUM_POSITIONS] = {
      ("P1QAT2E7/OPA3FUTZ5/GUT4OI6/OR1C3IN6/1I1O2A1G6/1R1T1KA1L6/1I1I1EH1E1W4/"
       "AS1JIBERS1H4/N1DA1AD3I4/N1R2B4R4/EGOTISM3L4/A1WO5NYM3/l1N2ELUDE5/"
       "E1DEOXY2VULN2/d5EVOE5 AEIRST/CF 390/443 0"),
      ("D6N5F1/I4J1O5ON/V1ENDARCH4XI/AGLY1PIE6N/G3WEBBY5E/AD4WO6T/TO4o7e/"
       "ER4RIZ2UVAE/6TOEIER2N/1COUGHS1POA4/2INIA1US6/MULIE5ATOKE/I3DEAREST4/"
       "L14/F14 AMRST/LQ 306/435 0"),
      ("STEREOED3PAUA/5FlOATIER2/2G3LOX6/2L4C3NU2/2ID1TEENAGER2/2BOOH3Z1TIS1/"
       "3W1AJI1Y1TAP1/FAIN1WOO1G2LI1/L2I5Y3V1/U2E11/R2SH10/R2TO10/I3B10/"
       "E1DEsKMEN6/S4IURE6 AADMNV/CNQ 435/440 0"),
  };
  Config *config =
      config_create_or_die("set -lex CSW21 -wmp false -rit false -wit false");
  Game *game = config_game_create(config);
  MoveList *move_list = move_list_create_small(100000);
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL_SMALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .target_equity = EQUITY_MAX_VALUE,
  };
  uint32_t every_square[PATH_MOVE_LISTS_NUM_LANES];
  for (int lane = 0; lane < PATH_MOVE_LISTS_NUM_LANES; lane++) {
    every_square[lane] = ~(uint32_t)0;
  }
  const uint64_t every_lane =
      PATH_MOVE_LISTS_NUM_LANES >= 64
          ? ~(uint64_t)0
          : ((uint64_t)1 << PATH_MOVE_LISTS_NUM_LANES) - 1;
  PathMoveLists *lists = path_move_lists_create();
  uint64_t rng = 0x2545F4914F6CDD1DULL;
  int num_revealed = 0;
  int num_standing = 0;
  int num_penalized = 0;
  int tile_weights[MAX_ALPHABET_SIZE];
  set_tile_weights(tile_weights);
  for (int position_idx = 0; position_idx < TILES_PLAYED_NUM_POSITIONS;
       position_idx++) {
    load_cgp_or_die(game, positions[position_idx]);
    bag_set_to_tiles(game_get_bag(game), NULL, 0);
    Game *snapshot = game_duplicate(game);
    for (int path_idx = 0; path_idx < TILES_PLAYED_PATHS_PER_POSITION;
         path_idx++) {
      path_move_lists_reset(lists);
      for (int side = 0; side < 2; side++) {
        game_set_player_on_turn_index(game, side);
        generate_moves(&args);
        path_move_lists_set_root(lists, side, move_list);
      }
      game_set_player_on_turn_index(game, 0);
      MoveUndo undos[BEST_PLAY_MAX_PATH_LENGTH];
      int length = 0;
      while (length < BEST_PLAY_MAX_PATH_LENGTH &&
             game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
        uint64_t before[2];
        for (int side = 0; side < 2; side++) {
          before[side] = side_tiles_played(game, move_list, side, 0, NULL);
        }
        generate_moves(&args);
        const int play_count = move_list->count - 1;
        int pick = play_count;
        if (play_count > 0 && next_random(&rng) % BEST_PLAY_PASS_ONE_IN != 0) {
          pick = (int)(next_random(&rng) % (uint64_t)play_count);
        }
        const SmallMove chosen = *move_list->small_moves[pick];
        make_path_move(game, lists, move_list, &chosen, &undos[length]);
        length++;
        // Three positions in four get their list, where the lists up the path
        // allow it, so that the standing plays and the penalized pick are read
        // from a list at every distance up the path, the root lists included.
        if (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
          assert_penalized_best_play_matches(lists, &args, tile_weights,
                                             path_idx + 3 * length,
                                             &num_penalized);
          if ((path_idx + length) % 4 != 0 &&
              (length <= 2 ||
               path_move_lists_slot_is_current(lists, length - 2))) {
            const SmallMove *derived = NULL;
            path_move_lists_moves_at(lists, &args, &derived);
          }
        }
        for (int side = 0; side < 2; side++) {
          const uint64_t after =
              side_tiles_played(game, move_list, side, 0, NULL);
          assert(side_tiles_played(game, move_list, side, every_lane,
                                   every_square) == after);
          const uint64_t in_lanes = side_tiles_played(
              game, move_list, side, path_move_lists_last_move_lanes(lists),
              path_move_lists_last_move_hits(lists));
          assert((in_lanes & ~after) == 0);
          const uint64_t revealed = after & ~before[side];
          const uint64_t single = side_single_tile_types(game, move_list, side);
          assert((revealed & ~(in_lanes | single)) == 0);
          if ((revealed & ~single) != 0) {
            num_revealed++;
          }
          assert_standing_tile_types(lists, game, move_list, side, after,
                                     single, &num_standing);
        }
      }
      while (length > 0) {
        length--;
        unplay_move_incremental(game, &undos[length]);
        path_move_lists_pop(lists);
      }
      assert_games_are_equal(game, snapshot, true);
    }
    game_destroy(snapshot);
  }
  assert(num_revealed >= TILES_PLAYED_MIN_REVEALED);
  assert(num_standing >= TILES_PLAYED_MIN_STANDING);
  assert(num_penalized >= PENALIZED_MIN_PICKS);
  path_move_lists_destroy(lists);
  small_move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

// A pass-only path never regenerates; best_play and best_play_alone must
// report no play when the side has none, and agree with moves_at when it does.
static void test_best_play_pass_path(int empty_side) {
  Config *config =
      config_create_or_die("set -lex NWL20 -wmp false -rit false -wit false");
  load_and_exec_config_or_die(config, "new");
  Game *game = config_get_game(config);
  MoveList *move_list = move_list_create_small(16);
  MoveList *best_move_list =
      move_list_create_small(BEST_PLAY_MOVE_LIST_CAPACITY);
  PathMoveLists *lists = path_move_lists_create();
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL_SMALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .target_equity = EQUITY_MAX_VALUE,
  };
  for (int side = 0; side < 2; side++) {
    Rack *rack = player_get_rack(game_get_player(game, side));
    const int tiles = rack_set_to_string(game_get_ld(game), rack,
                                         side == empty_side ? "VV" : "AA");
    assert(tiles == 2);
  }
  for (int side = 0; side < 2; side++) {
    game_set_player_on_turn_index(game, side);
    generate_moves(&args);
    path_move_lists_set_root(lists, side, move_list);
  }
  game_set_player_on_turn_index(game, 0);
  Move pass;
  move_set_as_pass(&pass);
  for (int length = 1; length <= 4; length++) {
    path_move_lists_push(lists, game_get_board(game), &pass);
    play_move(&pass, game, NULL);
    assert(path_move_lists_pending_lanes(lists) == 0);
    small_move_list_reset(move_list);
    const MoveGenArgs best_args = best_small_args_for(game, best_move_list, 10);
    SmallMove best_play;
    const bool found = path_move_lists_best_play(lists, &best_args, &best_play);
    SmallMove alone_play;
    const bool alone_found =
        path_move_lists_best_play_alone(lists, &best_args, &alone_play);
    assert(alone_found == found);
    assert(!found || alone_play.tiny_move == best_play.tiny_move);
    const SmallMove *derived = NULL;
    const int count = path_move_lists_moves_at(lists, &args, &derived);
    assert(found == (count > 0));
    assert(found == (length % 2 != empty_side));
    assert_matches_scratch(derived, count, &args);
  }
  path_move_lists_destroy(lists);
  small_move_list_destroy(best_move_list);
  small_move_list_destroy(move_list);
  config_destroy(config);
}

// anchor_span_reach from the lane's `empty` bits, a rack tile at a time.
static uint32_t anchor_span_reach_by_steps(uint32_t empty, uint32_t blocked,
                                           int col, int limit, int spare) {
  const uint32_t right_of_col = ~(((uint32_t)2 << col) - 1);
  const uint32_t left_of_col = ((uint32_t)1 << col) - 1;
  uint32_t right_empties = empty & right_of_col;
  uint32_t left_empties = empty & left_of_col;
  for (int tile_idx = 0; tile_idx < spare; tile_idx++) {
    right_empties &= right_empties - 1;
    if (left_empties != 0) {
      left_empties &= ~((uint32_t)1 << highest_set_bit_index(left_empties));
    }
  }
  right_empties |= blocked & right_of_col;
  left_empties |= blocked & left_of_col;
  const int right = right_empties != 0 ? lowest_set_bit_index(right_empties) - 1
                                       : BOARD_DIM - 1;
  int left = left_empties != 0 ? highest_set_bit_index(left_empties) + 1 : 0;
  if (left <= limit) {
    left = limit + 1;
  }
  return (((uint32_t)2 << right) - 1) & ~(((uint32_t)1 << left) - 1);
}

enum {
  SPAN_REACH_NUM_LANES = 5000,
  SPAN_REACH_DENSE_BLOCKS_ONE_IN = 4,
};

// anchor_span_reach reads where a play must stop from the list of the lane's
// empty columns. Over random lanes, and every anchor column, leftward limit
// and rack size, that is where taking the rack's tiles off the lane's empty
// squares one at a time stops.
static void test_anchor_span_reach(void) {
  uint64_t rng = 0xD1B54A32D192ED03ULL;
  const uint32_t lane_squares = ((uint32_t)1 << BOARD_DIM) - 1;
  for (int lane_idx = 0; lane_idx < SPAN_REACH_NUM_LANES; lane_idx++) {
    const uint32_t empty = (uint32_t)next_random(&rng) & lane_squares;
    // Usually a quarter of the empty squares take no rack tile, sometimes
    // half.
    uint32_t blocked = empty & (uint32_t)next_random(&rng);
    if (lane_idx % SPAN_REACH_DENSE_BLOCKS_ONE_IN != 0) {
      blocked &= (uint32_t)next_random(&rng);
    }
    int8_t empty_cols[LANE_SPAN_PAD + BOARD_DIM + LANE_SPAN_PAD];
    int empties_left_of[BOARD_DIM];
    memset(empty_cols, -1, LANE_SPAN_PAD);
    int lane_empties = 0;
    for (int col = 0; col < BOARD_DIM; col++) {
      empties_left_of[col] = lane_empties;
      if (((empty >> col) & 1) != 0) {
        empty_cols[LANE_SPAN_PAD + lane_empties] = (int8_t)col;
        lane_empties++;
      }
    }
    memset(&empty_cols[LANE_SPAN_PAD + lane_empties], BOARD_DIM, LANE_SPAN_PAD);
    for (int col = 0; col < BOARD_DIM; col++) {
      const int anchor_is_empty = (int)((empty >> col) & 1);
      for (int limit = -1; limit <= col; limit++) {
        for (int rack_tiles = 1; rack_tiles <= RACK_SIZE; rack_tiles++) {
          const int spare = rack_tiles - anchor_is_empty;
          assert(anchor_span_reach(empty_cols, empties_left_of[col],
                                   anchor_is_empty, blocked, col, limit,
                                   spare) ==
                 anchor_span_reach_by_steps(empty, blocked, col, limit, spare));
        }
      }
    }
  }
}

void test_path_move_lists(void) {
  test_pass_path(1);
  test_pass_path(0);
  test_tile_placement_paths();
  test_empty_derived_endgame();
  test_best_play_pass_path(1);
  test_best_play_pass_path(0);
  test_best_play_random_paths();
  test_tiles_played_in_lanes_random_paths();
  test_anchor_span_reach();
}
