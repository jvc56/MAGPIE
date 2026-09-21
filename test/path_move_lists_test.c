#include "path_move_lists_test.h"

#include "../src/def/equity_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/endgame_results.h"
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
#include "../src/impl/path_move_lists.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
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
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
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

void test_path_move_lists(void) {
  test_pass_path(1);
  test_pass_path(0);
  test_tile_placement_paths();
  test_empty_derived_endgame();
}
