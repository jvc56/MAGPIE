#include "inference_temperatures_test.h"

#include "../src/compat/cfile.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/alias_method.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/inference_args.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/inference_temperatures.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/inference.h"
#include "../src/impl/inference_temperature_fit.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_platform.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  // Games played to reach a position for the inference test.
  INFERENCE_TEMPERATURES_TEST_SEED = 4100,
  // The inference test infers from a play made from a bag of at most this
  // many tiles, between the knots of its table.
  INFERENCE_TEMPERATURES_TEST_MAX_BAG = 40,
  INFERENCE_TEMPERATURES_TEST_FIT_GAMES = 3,
};

#define INFERENCE_TEMPERATURES_TEST_TABLE                                      \
  "# a comment\n"                                                              \
  "\n"                                                                         \
  "4,12\n"                                                                     \
  " 10 , 6 \n"                                                                 \
  "75,1.5\n"                                                                   \
  "exchange,3\n"

// Writes contents to <dir>/strategy/<name>.csv.
static void write_table_file(const char *dir, const char *name,
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

static void assert_table_file_error(const char *dir, const char *contents) {
  write_table_file(dir, "bad", contents);
  ErrorStack *error_stack = error_stack_create();
  const InferenceTemperatures *table =
      inference_temperatures_create(dir, "bad", error_stack);
  assert(table == NULL);
  assert(error_stack_top(error_stack) ==
         ERROR_STATUS_INFERENCE_TEMPERATURES_INVALID_ROW);
  error_stack_destroy(error_stack);
}

static void assert_points(const InferenceTemperatures *table, int bag,
                          bool is_exchange, double expected) {
  const double points =
      inference_temperatures_get_points(table, bag, is_exchange);
  assert(fabs(points - expected) < 1e-9);
  assert(inference_temperatures_get(table, bag, is_exchange) ==
         double_to_equity(expected));
}

static void test_inference_temperatures_file(void) {
  char *dir = test_platform_make_temporary_directory("inftemp_file");
  write_table_file(dir, "good", INFERENCE_TEMPERATURES_TEST_TABLE);
  ErrorStack *error_stack = error_stack_create();
  InferenceTemperatures *table =
      inference_temperatures_create(dir, "good", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_strings_equal(inference_temperatures_get_name(table), "good");
  assert(inference_temperatures_get_number_of_knots(table) == 3);
  assert(inference_temperatures_get_knot_bag(table, 1) == 10);
  assert(inference_temperatures_get_knot_temperature(table, 1) == 6);
  assert(inference_temperatures_has_exchange(table));
  // Flat outside the knots, interpolated on log T between them, and the
  // exchange row for any exchange.
  assert_points(table, 0, false, 12);
  assert_points(table, 4, false, 12);
  assert_points(table, 7, false, sqrt(12.0 * 6.0));
  assert_points(table, 10, false, 6);
  assert_points(table, 75, false, 1.5);
  assert_points(table, 90, false, 1.5);
  const double weight = (40.0 - 10.0) / (75.0 - 10.0);
  assert_points(table, 40, false,
                exp(((1 - weight) * log(6.0)) + (weight * log(1.5))));
  assert_points(table, 7, true, 3);
  assert_points(table, 90, true, 3);
  inference_temperatures_destroy(table);

  // Without an exchange row an exchange takes the bag's temperature.
  write_table_file(dir, "no_exchange", "4,12\n10,6\n");
  table = inference_temperatures_create(dir, "no_exchange", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(!inference_temperatures_has_exchange(table));
  assert_points(table, 7, true, sqrt(12.0 * 6.0));
  inference_temperatures_destroy(table);

  // One knot is a constant temperature.
  write_table_file(dir, "one", "50,5\n");
  table = inference_temperatures_create(dir, "one", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_points(table, 1, false, 5);
  assert_points(table, 86, true, 5);
  inference_temperatures_destroy(table);
  error_stack_destroy(error_stack);

  assert_table_file_error(dir, "4,12,1\n");
  assert_table_file_error(dir, "4\n");
  assert_table_file_error(dir, "4,\n");
  assert_table_file_error(dir, "x,5\n");
  assert_table_file_error(dir, "-1,5\n");
  assert_table_file_error(dir, "4,x\n");
  assert_table_file_error(dir, "4,0\n");
  assert_table_file_error(dir, "4,-2\n");
  assert_table_file_error(dir, "4,nan\n");
  assert_table_file_error(dir, "4,inf\n");
  assert_table_file_error(dir, "4,1000000\n");
  assert_table_file_error(dir, "10,6\n4,12\n");
  assert_table_file_error(dir, "4,6\n4,12\n");
  assert_table_file_error(dir, "4,6\nexchange,3\nexchange,2\n");
  assert_table_file_error(dir, "exchange,0\n4,6\n");
  // A table needs a bag row.
  assert_table_file_error(dir, "# nothing\n");
  assert_table_file_error(dir, "exchange,3\n");
  test_platform_remove_directory_tree(dir);
  free(dir);
}

static void assert_table_names(const PlayersData *players_data,
                               const char *p1_name, const char *p2_name) {
  const char *expected[2] = {p1_name, p2_name};
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    const InferenceTemperatures *table =
        players_data_get_inference_temperatures(players_data, player_idx);
    if (expected[player_idx] == NULL) {
      assert(table == NULL);
    } else {
      assert(table != NULL);
      assert_strings_equal(inference_temperatures_get_name(table),
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

static void test_inference_temperatures_config(void) {
  char *dir = test_platform_make_temporary_directory("inftemp_config");
  write_table_file(dir, "CSW21_inftemp", "4,12\n75,1.5\n");
  write_table_file(dir, "NWL20_inftemp", "4,10\n75,2\n");
  write_table_file(dir, "other", "50,5\n");
  write_table_file(dir, "broken", "4,nan\n");
  char *set_cmd = get_formatted_string("set -lex CSW21 -wmp false -path %s:%s",
                                       dir, DEFAULT_TEST_DATA_PATH);
  Config *config = config_create_or_die(set_cmd);
  const PlayersData *players_data = config_get_players_data(config);
  // Tests state what they use: no table until one is asked for.
  assert_table_names(players_data, NULL, NULL);
  assert(settings_contain(config, " -inftemps1 none"));
  load_and_exec_config_or_die(config, "set -inftemps auto");
  assert_table_names(players_data, "CSW21_inftemp", "CSW21_inftemp");
  // The table follows the leaves, not the lexicon, per player.
  load_and_exec_config_or_die(config, "set -lex NWL20");
  assert_table_names(players_data, "CSW21_inftemp", "CSW21_inftemp");
  load_and_exec_config_or_die(config, "set -leaves NWL20");
  assert_table_names(players_data, "NWL20_inftemp", "NWL20_inftemp");
  load_and_exec_config_or_die(config, "set -k1 CSW21");
  assert_table_names(players_data, "CSW21_inftemp", "NWL20_inftemp");
  // A name or none takes the player off auto.
  load_and_exec_config_or_die(config, "set -inftemps2 other -inftemps1 none");
  assert_table_names(players_data, NULL, "other");
  load_and_exec_config_or_die(config, "set -leaves CSW21");
  assert_table_names(players_data, NULL, "other");
  load_and_exec_config_or_die(config, "set -inftemps1 default");
  assert_table_names(players_data, "CSW21_inftemp", "other");
  // A table that is missing or does not load is an error, which leaves the
  // players as they were.
  assert_config_exec_status(config, "set -inftemps2 broken",
                            ERROR_STATUS_INFERENCE_TEMPERATURES_INVALID_ROW);
  assert_config_exec_status(config, "set -inftemps missing",
                            ERROR_STATUS_FILEPATH_FILE_NOT_FOUND);
  assert_table_names(players_data, "CSW21_inftemp", "other");

  // The settings say which, and loading them back restores them.
  assert(settings_contain(config, " -inftemps1 auto"));
  assert(settings_contain(config, " -inftemps2 other"));
  StringBuilder *settings = string_builder_create();
  config_add_settings_to_string_builder(config, settings);
  Config *restored = config_create_or_die(string_builder_peek(settings));
  assert_table_names(config_get_players_data(restored), "CSW21_inftemp",
                     "other");
  config_destroy(restored);
  string_builder_destroy(settings);
  config_destroy(config);
  free(set_cmd);
  test_platform_remove_directory_tree(dir);
  free(dir);
}

// Sets played to the tiles a tile placement takes from the rack.
static void set_played_tiles(const Move *move, Rack *played) {
  rack_reset(played);
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    const MachineLetter ml = move_get_tile(move, tile_idx);
    if (ml != PLAYED_THROUGH_MARKER) {
      rack_add_letter(played, get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
    }
  }
}

// Infers the mover's leave from move in game, with temperature and each
// player's table, into results.
static void infer_with(const Config *config, const Game *game, const Move *move,
                       Equity temperature,
                       const InferenceTemperatures *p1_table,
                       const InferenceTemperatures *p2_table,
                       InferenceResults *results) {
  const int ld_size = ld_get_size(game_get_ld(game));
  Rack played;
  rack_set_dist_size_and_reset(&played, ld_size);
  set_played_tiles(move, &played);
  Rack known;
  rack_set_dist_size_and_reset(&known, ld_size);
  Rack observer;
  rack_set_dist_size_and_reset(&observer, ld_size);
  const int mover = game_get_player_on_turn_index(game);
  rack_copy(&observer, player_get_rack(game_get_player(game, 1 - mover)));
  ThreadControl *thread_control = config_get_thread_control(config);
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  InferenceArgs args;
  infer_args_fill(&args, 0, int_to_equity(10000), NULL, game, 1, 0, 0,
                  thread_control, false, true, mover, move_get_score(move), 0,
                  &played, &known, &observer);
  args.temperature = temperature;
  args.target_move = move;
  args.temperatures[0] = p1_table;
  args.temperatures[1] = p2_table;
  ErrorStack *error_stack = error_stack_create();
  infer_without_ctx(&args, results, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
}

static void assert_same_inference(const InferenceResults *first,
                                  const InferenceResults *second) {
  assert(inference_results_get_temperature(first) ==
         inference_results_get_temperature(second));
  const AliasMethod *first_alias = inference_results_get_alias_method(first);
  const AliasMethod *second_alias = inference_results_get_alias_method(second);
  assert(first_alias->num_items == second_alias->num_items);
  for (uint32_t item_idx = 0; item_idx < first_alias->num_items; item_idx++) {
    assert(first_alias->items[item_idx].probability ==
           second_alias->items[item_idx].probability);
    assert(first_alias->items[item_idx].alias ==
           second_alias->items[item_idx].alias);
  }
}

// With a table for the inferring player, inference uses the table's
// temperature for the real bag before the play: the same inference as with
// that temperature and no table. The target's own table and a temperature of
// 0 (a hard margin) leave the table out.
static void test_inference_temperatures_inference(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp false -s1 equity -s2 equity -r1 all -r2 all");
  Game *game = config_game_create(config);
  game_reset(game);
  game_seed(game, INFERENCE_TEMPERATURES_TEST_SEED);
  draw_starting_racks(game);
  MoveList *best_list = move_list_create(1);
  const Move *best = get_top_equity_move(game, best_list);
  while (bag_get_letters(game_get_bag(game)) >
             INFERENCE_TEMPERATURES_TEST_MAX_BAG ||
         move_get_type(best) != GAME_EVENT_TILE_PLACEMENT_MOVE ||
         move_get_tiles_played(best) == RACK_SIZE) {
    play_move(best, game, NULL);
    best = get_top_equity_move(game, best_list);
  }
  assert(!game_over(game));
  const int bag = game_get_real_bag_size(game);
  const int mover = game_get_player_on_turn_index(game);
  InferenceTemperatures *table = inference_temperatures_create_from_knots(
      "test", (const int[]){4, 10, 75}, (const double[]){12, 6, 1.5}, 3, true,
      3);
  const Equity scheduled = inference_temperatures_get(table, bag, false);
  assert(scheduled != int_to_equity(8));
  const InferenceTemperatures *tables[2] = {NULL, NULL};
  tables[1 - mover] = table;

  InferenceResults *with_table = inference_results_create(NULL);
  InferenceResults *expected = inference_results_create(NULL);
  infer_with(config, game, best, int_to_equity(8), tables[0], tables[1],
             with_table);
  infer_with(config, game, best, scheduled, NULL, NULL, expected);
  assert(inference_results_get_temperature(with_table) == scheduled);
  assert_same_inference(with_table, expected);

  // The target's table is not the inferring player's.
  const InferenceTemperatures *target_only[2] = {NULL, NULL};
  target_only[mover] = table;
  infer_with(config, game, best, int_to_equity(8), target_only[0],
             target_only[1], with_table);
  infer_with(config, game, best, int_to_equity(8), NULL, NULL, expected);
  assert_same_inference(with_table, expected);

  // A hard margin leaves the table out.
  infer_with(config, game, best, 0, tables[0], tables[1], with_table);
  infer_with(config, game, best, 0, NULL, NULL, expected);
  assert_same_inference(with_table, expected);

  inference_results_destroy(expected);
  inference_results_destroy(with_table);
  inference_temperatures_destroy(table);
  move_list_destroy(best_list);
  game_destroy(game);
  config_destroy(config);
}

static void test_inference_temperature_fit_band_edges(void) {
  int edges[INFERENCE_TEMPERATURE_FIT_NUMBER_OF_BANDS - 1];
  inference_temperature_fit_get_band_edges(86, edges);
  const int standard[] = {7, 13, 20, 30, 45, 60};
  for (int band_idx = 0;
       band_idx < INFERENCE_TEMPERATURE_FIT_NUMBER_OF_BANDS - 1; band_idx++) {
    assert(edges[band_idx] == standard[band_idx]);
  }
  // A super distribution's bands past 20 are wider.
  inference_temperature_fit_get_band_edges(186, edges);
  const int super[] = {7, 13, 20, 65, 97, 130};
  for (int band_idx = 0;
       band_idx < INFERENCE_TEMPERATURE_FIT_NUMBER_OF_BANDS - 1; band_idx++) {
    assert(edges[band_idx] == super[band_idx]);
  }
}

// Runs inftempfit on a small lexicon and returns the table it writes.
static char *fit_table(const char *dir, const char *name, int threads) {
  char *set_cmd = get_formatted_string(
      "set -path %s:%s -lex CSW21_ab -ld english_ab -wmp false -s1 equity -s2 "
      "equity -r1 all -r2 all -numplays 1 -threads %d -seed 11",
      dir, DEFAULT_TEST_DATA_PATH, threads);
  Config *config = config_create_or_die(set_cmd);
  char *fit_cmd = get_formatted_string(
      "inftempfit %d %s", INFERENCE_TEMPERATURES_TEST_FIT_GAMES, name);
  load_and_exec_config_or_die(config, fit_cmd);
  char *filename = get_formatted_string("%s/strategy/%s.csv", dir, name);
  char *table = get_string_from_file_or_die(filename);
  // The table it writes loads.
  ErrorStack *error_stack = error_stack_create();
  InferenceTemperatures *loaded =
      inference_temperatures_create(dir, name, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(inference_temperatures_get_number_of_knots(loaded) > 0);
  inference_temperatures_destroy(loaded);
  error_stack_destroy(error_stack);
  free(filename);
  free(fit_cmd);
  free(set_cmd);
  config_destroy(config);
  return table;
}

static void test_inference_temperature_fit_command(void) {
  char *dir = test_platform_make_temporary_directory("inftemp_fit");
  char *strategy_dir = get_formatted_string("%s/strategy", dir);
  (void)cfile_mkdir(strategy_dir);
  char *single = fit_table(dir, "fit_1", 1);
  char *multi = fit_table(dir, "fit_3", 3);
  // The same table for any thread count.
  assert_strings_equal(single, multi);
  assert(strstr(single, "inftempfit") != NULL);
  assert(strstr(single, "p1 static, p2 static") != NULL);
  free(multi);
  free(single);

  // The default name is the leaves'.
  char *set_cmd = get_formatted_string(
      "set -path %s:%s -lex CSW21_ab -ld english_ab -wmp false -s1 equity -s2 "
      "equity -r1 all -r2 all -numplays 1 -threads 2",
      dir, DEFAULT_TEST_DATA_PATH);
  Config *config = config_create_or_die(set_cmd);
  load_and_exec_config_or_die(config, "inftempfit 1");
  char *filename =
      get_formatted_string("%s/CSW21_ab_inftemp.csv", strategy_dir);
  free(get_string_from_file_or_die(filename));
  assert_config_exec_status(config, "inftempfit 0",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG);
  assert_config_exec_status(config, "inftempfit x",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG);
  config_destroy(config);
  free(filename);
  free(set_cmd);
  free(strategy_dir);
  test_platform_remove_directory_tree(dir);
  free(dir);
}

void test_inference_temperatures(void) {
  test_inference_temperatures_file();
  test_inference_temperatures_config();
  test_inference_temperatures_inference();
  test_inference_temperature_fit_band_edges();
  test_inference_temperature_fit_command();
}
