#include "pat_eval_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/pat_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/pat.h"
#include "../src/ent/pat_eval.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "pat_test_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

// A lone Q on A7, directly above the A8 triple word square: A8 hooks a row
// play with exactly the unseen Is (CSW21's only Q? word is QI).
#define PAT_EVAL_Q_CGP                                                         \
  "cgp 15/15/15/15/15/15/Q14/15/15/15/15/15/15/15/15 / 0/0 0"

enum {
  // One point per unseen tile that fits a triple word hook one tile away.
  PAT_EVAL_TEST_HOOK_WEIGHT = -1000,
  PAT_EVAL_TEST_MOVE_CAPACITY = 3000,
  PAT_EVAL_TEST_A8_ROW = 7,
};

static MoveList *pat_eval_test_generate(const Game *game) {
  MoveList *move_list = move_list_create(PAT_EVAL_TEST_MOVE_CAPACITY);
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
  return move_list;
}

// Whether the move puts a fresh tile on (row, col) or beside it, and
// whether any fresh tile lies within reach of a triple word square other
// than A8.
static void pat_eval_test_move_squares(const Move *move, int row, int col,
                                       bool *covers, bool *beside,
                                       bool *near_other) {
  *covers = false;
  *beside = false;
  *near_other = false;
  const bool vertical = board_is_dir_vertical(move_get_dir(move));
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    if (move_get_tile(move, tile_idx) == PLAYED_THROUGH_MARKER) {
      continue;
    }
    const int tile_row = move_get_row_start(move) + (vertical ? tile_idx : 0);
    const int tile_col = move_get_col_start(move) + (vertical ? 0 : tile_idx);
    const int row_distance = abs(tile_row - row);
    const int col_distance = abs(tile_col - col);
    if (row_distance + col_distance == 0) {
      *covers = true;
    } else if (row_distance + col_distance == 1) {
      *beside = true;
    }
    if (tile_row <= 1 || tile_row >= BOARD_DIM - 2 || tile_col >= 6) {
      *near_other = true;
    }
  }
}

// Worked by hand on the Q board with only hook_d1 weighted: the rack holds
// two of the nine Is, so A8's hook is worth 7 unseen tiles and the position
// baseline is -7000. A move covering A8 removes the only charged unit (0);
// a move that places nothing on or beside A8 leaves its cross set as it
// was (-7000), while one beside it creates a column hook there too. Every
// move's bound, and its lane's bound, is at least its exact term.
static void test_pat_eval_hand_position(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, PAT_EVAL_Q_CGP);
  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  Player *player = game_get_player(game, 0);
  rack_set_to_string(ld, player_get_rack(player), "IIAEORS");
  PATWeights *pat = pat_test_create_prepared("eval_hand", game);
  pat_set_weight(pat, PAT_FEATURE_HOOK_START, PAT_EVAL_TEST_HOOK_WEIGHT);

  PATEvalContext *pat_eval_ctx = malloc_or_die(sizeof(PATEvalContext));
  pat_eval_context_load(pat_eval_ctx, pat,
                        board_get_readonly_lanes(game_get_board(game), 0), ld,
                        player_get_rack(player), PAT_CLASS_MASK_ALL, RACK_SIZE);
  pat_eval_context_set_kwg(pat_eval_ctx, player_get_kwg(player));
  assert(pat_eval_ctx->pre_penalty == 7 * PAT_EVAL_TEST_HOOK_WEIGHT);
  assert(pat_eval_non_placement_penalty(pat_eval_ctx) ==
         7 * PAT_EVAL_TEST_HOOK_WEIGHT);
  assert(pat_eval_utility_bound(pat_eval_ctx) == 0);
  assert(pat_eval_context_get_active_classes(pat_eval_ctx) ==
         PAT_CLASS_MASK_TWS_ONLY);

  MoveList *move_list = pat_eval_test_generate(game);
  const int num_moves = move_list_get_count(move_list);
  int covering = 0;
  int leaving = 0;
  for (int move_idx = 0; move_idx < num_moves; move_idx++) {
    const Move *move = move_list_get_move(move_list, move_idx);
    Rack leave;
    get_leave_for_move(move, game, &leave);
    const Equity exact = pat_eval_move_penalty(pat_eval_ctx, move, &leave);
    assert(exact <= 0);
    assert(exact <= pat_eval_move_penalty_bound(pat_eval_ctx, move, &leave));
    assert(exact <= pat_eval_move_penalty_bound(pat_eval_ctx, move, NULL));
    if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      assert(exact == 7 * PAT_EVAL_TEST_HOOK_WEIGHT);
      continue;
    }
    const int dir = move_get_dir(move);
    const int lane = board_is_dir_vertical(dir) ? move_get_col_start(move)
                                                : move_get_row_start(move);
    assert(exact <= pat_eval_lane_penalty_bound(pat_eval_ctx, dir, lane));
    bool covers_a8;
    bool beside_a8;
    bool near_other;
    pat_eval_test_move_squares(move, PAT_EVAL_TEST_A8_ROW, 0, &covers_a8,
                               &beside_a8, &near_other);
    if (near_other) {
      continue;
    }
    if (covers_a8) {
      assert(exact == 0);
      covering++;
    } else if (beside_a8) {
      assert(exact < 7 * PAT_EVAL_TEST_HOOK_WEIGHT);
    } else {
      assert(exact == 7 * PAT_EVAL_TEST_HOOK_WEIGHT);
      leaving++;
    }
  }
  assert(covering > 0);
  assert(leaving > 0);
  move_list_destroy(move_list);
  free(pat_eval_ctx);
  pat_destroy(pat);
  config_destroy(config);
}

// The runtime class mask: a class it excludes is treated like one the file
// leaves unweighted, and the context reports only the classes it applies.
static void test_pat_eval_class_masks(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, PAT_EVAL_Q_CGP);
  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, "IIAEORS");
  PATWeights *pat = pat_test_create_prepared("eval_masks", game);
  pat_set_weight(pat, PAT_FEATURE_HOOK_START, PAT_EVAL_TEST_HOOK_WEIGHT);
  // No double word square here is a hook, so this weight charges nothing
  // but still marks the class as weighted.
  pat_set_weight(pat, PAT_FEATURE_DWS_HOOK_START, PAT_EVAL_TEST_HOOK_WEIGHT);
  const uint32_t dws_bit = 1U << PAT_PREMIUM_DWS;
  const struct {
    uint32_t enabled;
    uint32_t active;
    Equity baseline;
  } cases[] = {
      {PAT_CLASS_MASK_ALL, PAT_CLASS_MASK_TWS_ONLY | dws_bit,
       7 * PAT_EVAL_TEST_HOOK_WEIGHT},
      {PAT_CLASS_MASK_TWS_ONLY, PAT_CLASS_MASK_TWS_ONLY,
       7 * PAT_EVAL_TEST_HOOK_WEIGHT},
      {PAT_CLASS_MASK_ALL & ~PAT_CLASS_MASK_TWS_ONLY, dws_bit, 0},
      {0, 0, 0},
  };
  PATEvalContext *pat_eval_ctx = malloc_or_die(sizeof(PATEvalContext));
  for (size_t case_idx = 0; case_idx < sizeof(cases) / sizeof(cases[0]);
       case_idx++) {
    pat_eval_context_load(pat_eval_ctx, pat,
                          board_get_readonly_lanes(game_get_board(game), 0), ld,
                          rack, cases[case_idx].enabled, RACK_SIZE);
    assert(pat_eval_context_get_active_classes(pat_eval_ctx) ==
           cases[case_idx].active);
    assert(pat_eval_non_placement_penalty(pat_eval_ctx) ==
           cases[case_idx].baseline);
  }
  // A disabled context applies nothing.
  pat_eval_context_disable(pat_eval_ctx);
  assert(pat_eval_non_placement_penalty(pat_eval_ctx) == 0);
  assert(pat_eval_context_get_active_classes(pat_eval_ctx) == 0);
  assert(pat_eval_lane_penalty_bound(pat_eval_ctx, 0, 0) == 0);
  assert(pat_eval_non_placement_penalty(NULL) == 0);
  free(pat_eval_ctx);
  rack_destroy(rack);
  pat_destroy(pat);
  config_destroy(config);
}

static void assert_pat_move_lists_equal(const MoveList *first,
                                        const MoveList *second) {
  const int num_moves = move_list_get_count(first);
  assert(num_moves == move_list_get_count(second));
  for (int move_idx = 0; move_idx < num_moves; move_idx++) {
    const Move *first_move = move_list_get_move(first, move_idx);
    const Move *second_move = move_list_get_move(second, move_idx);
    assert(compare_moves_without_equity(first_move, second_move, true) == -1);
    assert(move_get_equity(first_move) == move_get_equity(second_move));
  }
}

// Movegen applies PAT only while the bag has tiles: with the Q board's hook
// weighted, some move's equity changes while tiles remain, and none does
// once the bag is empty.
static void test_pat_eval_empty_bag(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, PAT_EVAL_Q_CGP);
  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  PlayersData *players_data = config_get_players_data(config);
  Player *player = game_get_player(game, 0);
  rack_set_to_string(ld, player_get_rack(player), "IIAEORS");
  PATWeights *pat = pat_test_create_prepared("eval_empty_bag", game);
  pat_set_weight(pat, PAT_FEATURE_HOOK_START, PAT_EVAL_TEST_HOOK_WEIGHT);

  MoveList *lists[2][2];
  for (int empty_bag = 0; empty_bag < 2; empty_bag++) {
    if (empty_bag) {
      Bag *bag = game_get_bag(game);
      while (bag_get_letters(bag) > 0) {
        bag_draw_random_letter(bag, 1);
      }
    }
    for (int with_pat = 0; with_pat < 2; with_pat++) {
      players_data_set_data(players_data, PLAYERS_DATA_TYPE_PAT, 0,
                            with_pat ? pat : NULL);
      player_update(players_data, player);
      lists[empty_bag][with_pat] = pat_eval_test_generate(game);
    }
  }
  const int num_moves = move_list_get_count(lists[0][0]);
  assert(num_moves == move_list_get_count(lists[0][1]));
  int changed = 0;
  for (int move_idx = 0; move_idx < num_moves; move_idx++) {
    const Move *without = move_list_get_move(lists[0][0], move_idx);
    for (int other_idx = 0; other_idx < num_moves; other_idx++) {
      const Move *with = move_list_get_move(lists[0][1], other_idx);
      if (compare_moves_without_equity(without, with, true) == -1) {
        changed += move_get_equity(without) != move_get_equity(with);
        break;
      }
    }
  }
  assert(changed > 0);
  assert_pat_move_lists_equal(lists[1][0], lists[1][1]);
  players_data_set_data(players_data, PLAYERS_DATA_TYPE_PAT, 0, NULL);
  player_update(players_data, player);
  for (int empty_bag = 0; empty_bag < 2; empty_bag++) {
    for (int with_pat = 0; with_pat < 2; with_pat++) {
      move_list_destroy(lists[empty_bag][with_pat]);
    }
  }
  pat_destroy(pat);
  config_destroy(config);
}

// Class lists parse to masks and print back in canonical order.
static void test_pat_eval_class_names(void) {
  ErrorStack *error_stack = error_stack_create();
  const struct {
    const char *input;
    const char *canonical;
  } cases[] = {
      {"all", "all"},
      {"none", "none"},
      {"tws", "tws"},
      {"qls,tws", "tws,qls"},
      {"windows,dls,tws", "tws,dls,windows"},
      {"tws,dws,tls,dls,qws,qls,windows", "all"},
  };
  for (size_t case_idx = 0; case_idx < sizeof(cases) / sizeof(cases[0]);
       case_idx++) {
    const uint32_t mask =
        pat_parse_classes_mask(cases[case_idx].input, error_stack);
    assert(error_stack_is_empty(error_stack));
    char *printed = pat_classes_mask_to_string(mask);
    assert_strings_equal(printed, cases[case_idx].canonical);
    free(printed);
  }
  assert(pat_parse_classes_mask("tws,bogus", error_stack) == 0);
  assert(error_stack_top(error_stack) == ERROR_STATUS_PAT_INVALID_CLASSES_ARG);
  error_stack_reset(error_stack);
  error_stack_destroy(error_stack);
}

// The PAT usage options: the defaults, and the class-list options reading
// and rejecting class names.
static void test_pat_eval_usage_options(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -patrolloutclasses none -patrolloutclasses2 tws");
  const PlayersData *players_data = config_get_players_data(config);
  assert(!players_data_get_pat_candidates_disabled(players_data, 0));
  assert(!players_data_get_pat_rollout_disabled(players_data, 0));
  assert(players_data_get_pat_disabled_classes_mask(players_data, 0) == 0);
  assert(players_data_get_pat_rollout_disabled_classes_mask(players_data, 0) ==
         PAT_CLASS_MASK_ALL);
  assert(players_data_get_pat_rollout_disabled_classes_mask(players_data, 1) ==
         (PAT_CLASS_MASK_ALL & ~PAT_CLASS_MASK_TWS_ONLY));
  ErrorStack *error_stack = error_stack_create();
  config_load_command(config, "set -patclasses tws,bogus", error_stack);
  assert(error_stack_top(error_stack) == ERROR_STATUS_PAT_INVALID_CLASSES_ARG);
  error_stack_reset(error_stack);
  config_load_command(config, "set -patrolloutclasses bogus", error_stack);
  assert(error_stack_top(error_stack) == ERROR_STATUS_PAT_INVALID_CLASSES_ARG);
  error_stack_reset(error_stack);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_pat_rollout_default_classes(void) {
  PlayersData *players_data = players_data_create(false);
  const char *expected = BOARD_DIM >= 21 ? "tws,qws,windows" : "tws,windows";
  for (int player_index = 0; player_index < 2; player_index++) {
    char *enabled = pat_classes_mask_to_string(
        PAT_CLASS_MASK_ALL &
        ~players_data_get_pat_rollout_disabled_classes_mask(players_data,
                                                            player_index));
    assert_strings_equal(enabled, expected);
    free(enabled);
  }
  players_data_destroy(players_data);
}

void test_pat_eval(void) {
  test_pat_eval_hand_position();
  test_pat_eval_class_masks();
  test_pat_eval_empty_bag();
  test_pat_eval_class_names();
  test_pat_eval_usage_options();
  test_pat_rollout_default_classes();
}
