#include "outplay_check_test.h"

#include "../src/compat/cfile.h"
#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/autoplay_results.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/outplay_check_weights.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/outplay_check.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_platform.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  // test_outplay_check_few_plays: room for every play of its position, and
  // the candidates the check takes (OUTPLAY_CHECK_CANDIDATES).
  FEW_PLAYS_LIST_CAPACITY = 100,
  FEW_PLAYS_CHECK_CANDIDATES = 8,
};

// Weights whose value the test works out by hand. A pair is in the letter
// distribution's order.
#define OUTPLAY_CHECK_TEST_WEIGHTS                                             \
  "# a comment\n"                                                              \
  "\n"                                                                         \
  "letterdistribution,english\n"                                               \
  "const|no,2\n"                                                               \
  "const|oo,-3\n"                                                              \
  " obo , 0.5 \n"                                                              \
  "obs,-0.25\n"                                                                \
  "face|no,0.1\n"                                                              \
  "n3|no,1.5\n"                                                                \
  "A,0.25\n"                                                                   \
  "AB|oo,4\n"                                                                  \
  "EE,1\n"                                                                     \
  "own|no,1\n"                                                                 \
  "ownb|no,0.5\n"                                                              \
  "ownl2|no,2\n"                                                               \
  "ownl3|no,4\n"                                                               \
  "ownn3|no,0.75\n"                                                            \
  "center|2|3,-40\n"                                                           \
  "center|2|3,-2\n"

// Writes contents to <dir>/strategy/<name>.csv.
static void write_weights_file(const char *dir, const char *name,
                               const char *contents) {
  char *strategy_dir = get_formatted_string("%s/strategy", dir);
  (void)cfile_mkdir(strategy_dir);
  char *filename = get_formatted_string("%s/%s.csv", strategy_dir, name);
  ErrorStack *error_stack = error_stack_create();
  write_string_to_file(filename, "w", contents, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  free(filename);
  free(strategy_dir);
}

// The data paths for weights written to dir: dir first, then the test data,
// which has the letter distributions.
static char *weights_data_paths(const char *dir) {
  return get_formatted_string("%s:%s", dir, DEFAULT_TEST_DATA_PATH);
}

static void assert_weights_file_error(const char *dir, const char *contents,
                                      error_code_t expected) {
  write_weights_file(dir, "bad", contents);
  char *data_paths = weights_data_paths(dir);
  ErrorStack *error_stack = error_stack_create();
  const OutplayCheckWeights *weights =
      outplay_check_weights_create(data_paths, "bad", error_stack);
  assert(weights == NULL);
  assert(error_stack_top(error_stack) == expected);
  error_stack_destroy(error_stack);
  free(data_paths);
}

static void assert_value(const OutplayCheckWeights *weights,
                         const LetterDistribution *ld, const char *held_tiles,
                         const OutplayCheckFeatures *features,
                         double expected) {
  Rack *held = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, held, held_tiles);
  const double value = outplay_check_weights_value(weights, ld, held, features);
  assert(fabs(value - expected) < 1e-9);
  rack_destroy(held);
}

static void test_outplay_check_weights_file(void) {
  char *dir = test_platform_make_temporary_directory("outcheck_file");
  write_weights_file(dir, "test_weights", OUTPLAY_CHECK_TEST_WEIGHTS);
  char *data_paths = weights_data_paths(dir);
  ErrorStack *error_stack = error_stack_create();
  OutplayCheckWeights *weights =
      outplay_check_weights_create(data_paths, "test_weights", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(weights != NULL);
  assert_strings_equal(outplay_check_weights_get_name(weights), "test_weights");
  assert_strings_equal(outplay_check_weights_get_ld_name(weights), "english");
  LetterDistribution *ld = ld_create(data_paths, "english", error_stack);
  assert(error_stack_is_empty(error_stack));

  // The opponent cannot go out: AEE, face value 3, against an opponent whose
  // best play scores 20, and the mover can go out next with two plays, the
  // best scoring 15, in three lanes. Static equity keeping AEE is -2 x 3 - 10;
  // const|no; obs x 20; n3|no, face|no x 3, A and EE (a pair of E); own|no,
  // ownb|no x 15, ownl2|no, ownl3|no and ownn3|no.
  const OutplayCheckFeatures no_out = {.opponent_can_go_out = false,
                                       .opponent_best_out_score = 0,
                                       .opponent_best_score = 20,
                                       .own_can_go_out = true,
                                       .own_best_out_score = 15,
                                       .own_out_lanes = 3};
  assert_value(weights, ld, "AEE", &no_out,
               (-2.0 * 3 - 10) + 2 + (-0.25 * 20) + (1.5 + 0.1 * 3 + 0.25 + 1) +
                   (1 + 0.5 * 15 + 2 + 4 + 0.75));
  // In two lanes ownl3 does not count, and without an out play none of the
  // own terms do.
  OutplayCheckFeatures two_lanes = no_out;
  two_lanes.own_out_lanes = 2;
  assert_value(weights, ld, "AEE", &two_lanes,
               (-2.0 * 3 - 10) + 2 + (-0.25 * 20) + (1.5 + 0.1 * 3 + 0.25 + 1) +
                   (1 + 0.5 * 15 + 2 + 0.75));
  OutplayCheckFeatures no_own = no_out;
  no_own.own_can_go_out = false;
  assert_value(weights, ld, "AEE", &no_own,
               (-2.0 * 3 - 10) + 2 + (-0.25 * 20) + (1.5 + 0.1 * 3 + 0.25 + 1));

  // The opponent can go out for 30 holding AB, face value 4: less the 30,
  // const|oo, obo x 30, A (untagged, so both tags) and AB|oo; the opponent's
  // best play other than an out does not count.
  const OutplayCheckFeatures opp_out = {.opponent_can_go_out = true,
                                        .opponent_best_out_score = 30,
                                        .opponent_best_score = 45,
                                        .own_can_go_out = false,
                                        .own_best_out_score = 0,
                                        .own_out_lanes = 0};
  assert_value(weights, ld, "AB", &opp_out,
               (-2.0 * 4 - 10) - 30 - 3 + 0.5 * 30 + (0.25 + 4));
  // The same rack without the opponent's out: AB|oo does not count.
  assert_value(weights, ld, "AB", &no_own,
               (-2.0 * 4 - 10) + 2 + (-0.25 * 20) + (0.1 * 4 + 0.25));

  // Centers add up, and are 0 where the file has none or out of range.
  assert(outplay_check_weights_get_center(weights, 2, 3) == -42.0);
  assert(outplay_check_weights_get_center(weights, 1, 1) == 0.0);
  assert(outplay_check_weights_get_center(weights, -1, 0) == 0.0);
  assert(outplay_check_weights_get_center(weights, 2, 99) == 0.0);

  ld_destroy(ld);
  outplay_check_weights_destroy(weights);
  error_stack_destroy(error_stack);

  const error_code_t row = ERROR_STATUS_OUTPLAY_CHECK_INVALID_ROW;
  assert_weights_file_error(dir, "A,1\n", row);
  assert_weights_file_error(dir, "# nothing\n", row);
  assert_weights_file_error(dir, "letterdistribution,\n", row);
  assert_weights_file_error(
      dir, "letterdistribution,english\nletterdistribution,english\n", row);
  assert_weights_file_error(dir, "letterdistribution,english\nconst,1\n", row);
  assert_weights_file_error(dir, "letterdistribution,english\nconst|xx,1\n",
                            row);
  assert_weights_file_error(dir, "letterdistribution,english\nBA,1\n", row);
  assert_weights_file_error(dir, "letterdistribution,english\nABC,1\n", row);
  assert_weights_file_error(dir, "letterdistribution,english\na,1\n", row);
  assert_weights_file_error(dir, "letterdistribution,english\nn9|no,1\n", row);
  assert_weights_file_error(dir, "letterdistribution,english\nobo|oo,1\n", row);
  assert_weights_file_error(dir, "letterdistribution,english\nA|no|oo,1\n",
                            row);
  assert_weights_file_error(dir, "letterdistribution,english\ncenter|1,1\n",
                            row);
  assert_weights_file_error(dir, "letterdistribution,english\nA,abc\n", row);
  assert_weights_file_error(dir, "letterdistribution,english\nA,1,2\n", row);
  assert_weights_file_error(dir, "letterdistribution,nonexistent\n",
                            ERROR_STATUS_FILEPATH_FILE_NOT_FOUND);
  char *missing_paths = weights_data_paths(dir);
  ErrorStack *missing_stack = error_stack_create();
  assert(outplay_check_weights_create(missing_paths, "missing",
                                      missing_stack) == NULL);
  assert(error_stack_top(missing_stack) ==
         ERROR_STATUS_FILEPATH_FILE_NOT_FOUND);
  error_stack_destroy(missing_stack);
  free(missing_paths);

  free(data_paths);
  test_platform_remove_directory_tree(dir);
  free(dir);
}

static void assert_weights_names(const PlayersData *players_data,
                                 const char *p1_name, const char *p2_name) {
  const char *expected[2] = {p1_name, p2_name};
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    const OutplayCheckWeights *weights =
        players_data_get_outplay_check(players_data, player_idx);
    if (expected[player_idx] == NULL) {
      assert(weights == NULL);
    } else {
      assert(weights != NULL);
      assert_strings_equal(outplay_check_weights_get_name(weights),
                           expected[player_idx]);
    }
  }
}

static bool settings_contain(const Config *config, const char *text) {
  StringBuilder *settings = string_builder_create();
  config_add_settings_to_string_builder(config, settings);
  const bool contains = strstr(string_builder_peek(settings), text) != NULL;
  string_builder_destroy(settings);
  return contains;
}

static void test_outplay_check_config(void) {
  char *dir = test_platform_make_temporary_directory("outcheck_config");
  write_weights_file(dir, "CSW21_outcheck",
                     "letterdistribution,english\nconst|oo,-1\n");
  write_weights_file(dir, "NWL20_outcheck",
                     "letterdistribution,english\nconst|oo,-2\n");
  write_weights_file(dir, "other", "letterdistribution,english\nobo,1\n");
  write_weights_file(dir, "broken", "letterdistribution,english\nBA,1\n");
  char *set_cmd = get_formatted_string("set -lex CSW21 -wmp false -path %s:%s",
                                       dir, DEFAULT_TEST_DATA_PATH);
  Config *config = config_create_or_die(set_cmd);
  const PlayersData *players_data = config_get_players_data(config);
  // Tests state what they use: no weights until they are asked for.
  assert_weights_names(players_data, NULL, NULL);
  assert(settings_contain(config, " -outcheck1 none"));
  load_and_exec_config_or_die(config, "set -outcheck auto");
  assert_weights_names(players_data, "CSW21_outcheck", "CSW21_outcheck");
  // The weights follow the lexicon, per player.
  load_and_exec_config_or_die(config, "set -lex NWL20");
  assert_weights_names(players_data, "NWL20_outcheck", "NWL20_outcheck");
  load_and_exec_config_or_die(config, "set -l1 CSW21");
  assert_weights_names(players_data, "CSW21_outcheck", "NWL20_outcheck");
  // A name or none takes the player off auto.
  load_and_exec_config_or_die(config, "set -outcheck2 other -outcheck1 none");
  assert_weights_names(players_data, NULL, "other");
  load_and_exec_config_or_die(config, "set -lex NWL20");
  assert_weights_names(players_data, NULL, "other");
  load_and_exec_config_or_die(config, "set -outcheck1 default");
  assert_weights_names(players_data, "NWL20_outcheck", "other");
  // Weights that are missing or do not load are an error, which leaves the
  // players as they were.
  assert_config_exec_status(config, "set -outcheck2 broken",
                            ERROR_STATUS_OUTPLAY_CHECK_INVALID_ROW);
  assert_config_exec_status(config, "set -outcheck missing",
                            ERROR_STATUS_FILEPATH_FILE_NOT_FOUND);
  assert_weights_names(players_data, "NWL20_outcheck", "other");

  // The settings say which, and loading them back restores them.
  assert(settings_contain(config, " -outcheck1 auto"));
  assert(settings_contain(config, " -outcheck2 other"));
  StringBuilder *settings = string_builder_create();
  config_add_settings_to_string_builder(config, settings);
  Config *restored = config_create_or_die(string_builder_peek(settings));
  assert_weights_names(config_get_players_data(restored), "NWL20_outcheck",
                       "other");
  config_destroy(restored);
  string_builder_destroy(settings);
  config_destroy(config);
  free(set_cmd);
  test_platform_remove_directory_tree(dir);
  free(dir);
}

// The results of static self-play with player 1's out-play check set to
// p1_weights (a name or none), on threads threads.
static char *self_play_results(Config *config, const char *p1_weights,
                               int threads) {
  char *command = get_formatted_string(
      "autoplay games 40 -outcheck1 %s -outcheck2 none -threads %d -seed 91",
      p1_weights, threads);
  load_and_exec_config_or_die(config, command);
  free(command);
  return autoplay_results_to_string(config_get_autoplay_results(config), false,
                                    false);
}

static void test_outplay_check_play(void) {
  char *dir = test_platform_make_temporary_directory("outcheck_play");
  // Avoid leaving the opponent an out play, and like keeping an E.
  write_weights_file(dir, "avoid_outs",
                     "letterdistribution,english\nconst|oo,-60\nE,3\n");
  // The same weights for another letter distribution: they do not apply.
  write_weights_file(dir, "french_weights",
                     "letterdistribution,french\nconst|oo,-60\nE,3\n");
  char *set_cmd = get_formatted_string(
      "set -lex CSW21 -wmp false -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -gp true -path %s:%s",
      dir, DEFAULT_TEST_DATA_PATH);
  Config *config = config_create_or_die(set_cmd);
  char *without = self_play_results(config, "none", 1);
  char *with = self_play_results(config, "avoid_outs", 1);
  char *with_threads = self_play_results(config, "avoid_outs", 4);
  char *other_ld = self_play_results(config, "french_weights", 1);
  // The check changes some plays, the games do not depend on the threads,
  // and weights for another distribution change nothing.
  assert(!strings_equal(with, without));
  assert_strings_equal(with, with_threads);
  assert_strings_equal(other_ld, without);
  free(other_ld);
  free(with_threads);
  free(with);
  free(without);
  config_destroy(config);
  free(set_cmd);
  test_platform_remove_directory_tree(dir);
  free(dir);
}

// What every play of the player on turn, listed, says: whether one uses the
// whole rack, the best score of those that do and of all plays, and the lanes
// those that do lie in, counted up to 3.
typedef struct ListedSide {
  bool can_go_out;
  int best_out_score;
  int best_score;
  int out_lanes;
} ListedSide;

static ListedSide listed_side(Game *game, MoveList *plays) {
  const int on_turn = game_get_player_on_turn_index(game);
  const int rack_tiles =
      rack_get_total_letters(player_get_rack(game_get_player(game, on_turn)));
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
  ListedSide side = {0};
  uint64_t lane_bits = 0;
  for (int play_idx = 0; play_idx < move_list_get_count(plays); play_idx++) {
    const Move *play = move_list_get_move(plays, play_idx);
    if (move_get_type(play) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    const int score = equity_to_int(move_get_score(play));
    if (score > side.best_score) {
      side.best_score = score;
    }
    if (move_get_tiles_played(play) != rack_tiles) {
      continue;
    }
    if (!side.can_go_out || score > side.best_out_score) {
      side.best_out_score = score;
    }
    side.can_go_out = true;
    lane_bits |= UINT64_C(1) << (board_is_dir_vertical(move_get_dir(play))
                                     ? BOARD_DIM + move_get_col_start(play)
                                     : move_get_row_start(play));
  }
  for (int lane = 0; lane < 2 * BOARD_DIM && side.out_lanes < 3; lane++) {
    if ((lane_bits >> lane) & 1) {
      side.out_lanes++;
    }
  }
  return side;
}

// The features the check finds with its best-play generations equal what
// every play of each side, listed, says, in the empty-bag positions of static
// games, with or without the word map (by which the mover's searches skip
// windows that no word using the whole rack can fill).
static void test_outplay_check_features_with_wmp(bool use_wmp) {
  Config *config = config_create_or_die(
      use_wmp ? "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 "
                "all -numplays 1"
              : "set -lex CSW21 -wmp false -s1 equity -s2 equity -r1 all -r2 "
                "all -numplays 1");
  Game *game = config_game_create(config);
  MoveList *plays = move_list_create(20000);
  int positions = 0;
  int opponent_outs = 0;
  int own_outs = 0;
  int own_two_lanes = 0;
  int own_three_lanes = 0;
  for (int game_idx = 0; game_idx < 40; game_idx++) {
    game_reset(game);
    game_seed(game, 7000 + (uint64_t)game_idx);
    draw_starting_racks(game);
    while (!game_over(game)) {
      if (bag_is_empty(game_get_bag(game))) {
        OutplayCheckFeatures features;
        outplay_check_find_features(game, &features);
        const int opp_idx = game_get_player_on_turn_index(game);
        const ListedSide opp = listed_side(game, plays);
        game_set_player_on_turn_index(game, 1 - opp_idx);
        const ListedSide own = listed_side(game, plays);
        game_set_player_on_turn_index(game, opp_idx);
        assert(features.opponent_can_go_out == opp.can_go_out);
        if (opp.can_go_out) {
          assert(features.opponent_best_out_score == opp.best_out_score);
        } else {
          assert(features.opponent_best_score == opp.best_score);
        }
        assert(features.own_can_go_out == own.can_go_out);
        if (own.can_go_out) {
          assert(features.own_best_out_score == own.best_out_score);
          assert(features.own_out_lanes == own.out_lanes);
        }
        positions++;
        opponent_outs += opp.can_go_out ? 1 : 0;
        own_outs += own.can_go_out ? 1 : 0;
        own_two_lanes += own.out_lanes >= 2 ? 1 : 0;
        own_three_lanes += own.out_lanes >= 3 ? 1 : 0;
      }
      play_top_n_equity_move(game, 0);
    }
  }
  // The positions cover every case the features tell apart.
  assert(positions >= 100);
  assert(opponent_outs > 0 && opponent_outs < positions);
  assert(own_outs > 0 && own_outs < positions);
  assert(own_two_lanes > 0 && own_three_lanes > 0);
  move_list_destroy(plays);
  game_destroy(game);
  config_destroy(config);
}

static void test_outplay_check_features(void) {
  test_outplay_check_features_with_wmp(false);
  test_outplay_check_features_with_wmp(true);
}

// A player with fewer plays than the check takes candidates has its pass
// among them, which has no equity to read. The position is from NWL23
// self-play: seven plays, four tiles in the bag.
static void test_outplay_check_few_plays(void) {
  char *dir = test_platform_make_temporary_directory("outcheck_few_plays");
  write_weights_file(dir, "avoid_outs",
                     "letterdistribution,english\nconst|oo,-60\nE,3\n");
  char *data_paths = weights_data_paths(dir);
  ErrorStack *error_stack = error_stack_create();
  OutplayCheckWeights *weights =
      outplay_check_weights_create(data_paths, "avoid_outs", error_stack);
  assert(error_stack_is_empty(error_stack));
  Config *config = config_create_or_die(
      "set -lex NWL23 -wmp false -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1");
  load_and_exec_config_or_die(
      config,
      "cgp 14u/12GIN/11WETS/10NOM1H/10AW1Ai/9TIE1IF/7V1HAET1T/6VIBED1INS/"
      "7G1YE1T2/5JIAO1S1LEE/4COD1XU1QI1R/5TOR3UNDE/5SLIP1LAG2/7FAB1K1PE/"
      "8MONEYER ACDENOR/AAILRUU 315/443 0");
  Game *game = config_get_game(config);
  MoveList *plays = move_list_create(FEW_PLAYS_LIST_CAPACITY);
  const MoveGenArgs args = {
      .game = game,
      .move_list = plays,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&args);
  int tile_plays = 0;
  int passes = 0;
  for (int play_idx = 0; play_idx < move_list_get_count(plays); play_idx++) {
    const game_event_t type =
        move_get_type(move_list_get_move(plays, play_idx));
    tile_plays += type == GAME_EVENT_TILE_PLACEMENT_MOVE ? 1 : 0;
    passes += type == GAME_EVENT_PASS ? 1 : 0;
  }
  assert(tile_plays > 0 && tile_plays < FEW_PLAYS_CHECK_CANDIDATES);
  assert(passes == 1);
  MoveList *best = move_list_create(1);
  const Move top = *get_top_equity_move(game, best);
  assert(move_get_type(&top) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  outplay_check_choose(game, best, weights);
  assert(move_get_type(move_list_get_move(best, 0)) ==
         GAME_EVENT_TILE_PLACEMENT_MOVE);
  move_list_destroy(best);
  move_list_destroy(plays);
  config_destroy(config);
  outplay_check_weights_destroy(weights);
  error_stack_destroy(error_stack);
  free(data_paths);
  test_platform_remove_directory_tree(dir);
  free(dir);
}

void test_outplay_check(void) {
  test_outplay_check_few_plays();
  test_outplay_check_weights_file();
  test_outplay_check_config();
  test_outplay_check_features();
  test_outplay_check_play();
}
