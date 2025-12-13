#include "../src/def/board_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/game.h"
#include "../src/ent/heat_map.h"
#include "../src/ent/move.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/config.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

void assert_heat_map_add_move(const Game *game, const char *ucgi_move_str,
                              HeatMap *hm) {
  ErrorStack *error_stack = error_stack_create();
  ValidatedMoves *vms = validated_moves_create(game, 0, ucgi_move_str, true,
                                               false, true, error_stack);
  assert(error_stack_is_empty(error_stack));
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    assert(0);
  }
  heat_map_add_move(hm, validated_moves_get_move(vms, 0));
  validated_moves_destroy(vms);
  error_stack_destroy(error_stack);
}

void assert_heat_map_count(const char *name, int row, int col, heat_map_t type,
                           const uint64_t actual_count,
                           const uint64_t expected_count) {
  if (actual_count != expected_count) {
    printf("heat map counts mismatch for %s:\n", name);
    printf("row: %d, col: %d, type: %d\n", row, col, type);
    printf("actual:   %lu\n", actual_count);
    printf("expected: %lu\n", expected_count);
    assert(0);
  }
}

void assert_heat_map_values(HeatMap *hm, int row, int col, heat_map_t type,
                            const uint64_t expected_count,
                            const uint64_t expected_max_count,
                            const uint64_t expected_total) {
  const uint64_t actual_count = heat_map_get_count(hm, row, col, type);
  assert_heat_map_count("count", row, col, type, actual_count, expected_count);
  const uint64_t actual_max_count = heat_map_get_board_count_max(hm, type);
  assert_heat_map_count("max count", row, col, type, actual_max_count,
                        expected_max_count);
  const uint64_t actual_total_count = heat_map_get_total_count(hm);
  assert_heat_map_count("total count", row, col, type, actual_total_count,
                        expected_total);
}

void assert_heat_map_single_move(const Game *game, const char *ucgi_move_str,
                                 HeatMap *hm) {
  ErrorStack *error_stack = error_stack_create();
  ValidatedMoves *vms = validated_moves_create(game, 0, ucgi_move_str, true,
                                               false, true, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    assert(0);
  }
  const Move *move = validated_moves_get_move(vms, 0);
  heat_map_reset(hm);
  heat_map_add_move(hm, move);
  const int move_tiles_played = move_get_tiles_played(move);
  const bool is_bingo = move_tiles_played == RACK_SIZE;
  uint64_t expected_total_count_sum = move_tiles_played;
  if (is_bingo) {
    expected_total_count_sum *= 2;
  }
  assert(heat_map_get_total_count(hm) == expected_total_count_sum);
  const int move_len = move_get_tiles_length(move);
  int *move_coords = malloc_or_die(sizeof(int) * move_len * 2);
  const int row_inc = move_get_dir(move) == BOARD_VERTICAL_DIRECTION;
  const int col_inc = move_get_dir(move) == BOARD_HORIZONTAL_DIRECTION;
  int curr_row = move_get_row_start(move);
  int curr_col = move_get_col_start(move);
  for (int i = 0; i < move_len; i++) {
    move_coords[(ptrdiff_t)i * 2] = curr_row;
    move_coords[(ptrdiff_t)i * 2 + 1] = curr_col;
    curr_row += row_inc;
    curr_col += col_inc;
  }
  int move_coords_index = 0;
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const uint64_t all_count =
          heat_map_get_count(hm, row, col, HEAT_MAP_TYPE_ALL);
      const uint64_t bingo_count =
          heat_map_get_count(hm, row, col, HEAT_MAP_TYPE_BINGO);
      if (move_coords_index < move_len &&
          row == move_coords[(ptrdiff_t)move_coords_index * 2] &&
          col == move_coords[(ptrdiff_t)move_coords_index * 2 + 1]) {
        assert(all_count == 1);
        assert(bingo_count == is_bingo);
        move_coords_index++;
      } else {
        assert(all_count == 0);
        assert(bingo_count == 0);
      }
    }
  }

  free(move_coords);

  validated_moves_destroy(vms);
  error_stack_destroy(error_stack);
}

void show_heat_maps_for_cgp(Config *config, const char *cgp, const char *rack) {
  StringBuilder *cmd_sb = string_builder_create();
  string_builder_add_formatted_string(cmd_sb, "cgp %s", cgp);

  load_and_exec_config_or_die(config, string_builder_peek(cmd_sb));
  string_builder_clear(cmd_sb);

  string_builder_add_formatted_string(cmd_sb, "r %s", rack);
  load_and_exec_config_or_die(config, string_builder_peek(cmd_sb));

  string_builder_destroy(cmd_sb);

  load_and_exec_config_or_die(
      config,
      "gsim -iter 1000 -minp 10 -numplays 10 "
      "-useh true -plies 5 -boardcolor true -threads 12 -sr rr -thres none");

  for (int i = 0; i < 2; i++) {
    load_and_exec_config_or_die(config, "heat 1");
    load_and_exec_config_or_die(config, "heat 2");
    load_and_exec_config_or_die(config, "heat 3");
    load_and_exec_config_or_die(config, "heat 2 a");
    load_and_exec_config_or_die(config, "heat 3 b");
    load_and_exec_config_or_die(config, "heat 5 1");
    load_and_exec_config_or_die(config, "heat 3 1 a");
    load_and_exec_config_or_die(config, "heat 3 2 b");
    load_and_exec_config_or_die(config, "heat 3 3 a");
    load_and_exec_config_or_die(config, "heat 3 4 b");
    load_and_exec_config_or_die(config, "heat 3 5 b");
    load_and_exec_config_or_die(config, "set -boardcolor none");
  }
}

void test_heat_map(void) {
  Config *config =
      config_create_or_die("set -lex CSW21 -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, "newgame");

  HeatMap *hm = heat_map_create();
  heat_map_reset(hm);

  Game *game = config_get_game(config);

  assert_heat_map_add_move(game, "pass", hm);
  assert_heat_map_values(hm, 0, 0, HEAT_MAP_TYPE_ALL, 0, 0, 0);
  assert_heat_map_values(hm, 0, 0, HEAT_MAP_TYPE_BINGO, 0, 0, 0);

  assert_heat_map_add_move(game, "ex.ABC.ABCDEFG", hm);
  assert_heat_map_values(hm, 0, 0, HEAT_MAP_TYPE_ALL, 0, 0, 0);
  assert_heat_map_values(hm, 0, 0, HEAT_MAP_TYPE_BINGO, 0, 0, 0);

  assert_heat_map_add_move(game, "8h.DO", hm);
  assert_heat_map_values(hm, 7, 7, HEAT_MAP_TYPE_ALL, 1, 1, 2);
  assert_heat_map_values(hm, 7, 8, HEAT_MAP_TYPE_ALL, 1, 1, 2);
  assert_heat_map_values(hm, 7, 9, HEAT_MAP_TYPE_ALL, 0, 1, 2);
  assert_heat_map_values(hm, 7, 7, HEAT_MAP_TYPE_BINGO, 0, 0, 2);
  assert_heat_map_values(hm, 7, 8, HEAT_MAP_TYPE_BINGO, 0, 0, 2);
  assert_heat_map_values(hm, 7, 9, HEAT_MAP_TYPE_BINGO, 0, 0, 2);

  assert_heat_map_add_move(game, "8h.PRETZEL", hm);
  assert_heat_map_values(hm, 7, 7, HEAT_MAP_TYPE_ALL, 2, 2, 16);
  assert_heat_map_values(hm, 7, 8, HEAT_MAP_TYPE_ALL, 2, 2, 16);
  assert_heat_map_values(hm, 7, 9, HEAT_MAP_TYPE_ALL, 1, 2, 16);
  assert_heat_map_values(hm, 7, 10, HEAT_MAP_TYPE_ALL, 1, 2, 16);
  assert_heat_map_values(hm, 7, 11, HEAT_MAP_TYPE_ALL, 1, 2, 16);
  assert_heat_map_values(hm, 7, 12, HEAT_MAP_TYPE_ALL, 1, 2, 16);
  assert_heat_map_values(hm, 7, 13, HEAT_MAP_TYPE_ALL, 1, 2, 16);
  assert_heat_map_values(hm, 7, 6, HEAT_MAP_TYPE_ALL, 0, 2, 16);
  assert_heat_map_values(hm, 7, 7, HEAT_MAP_TYPE_BINGO, 1, 1, 16);
  assert_heat_map_values(hm, 7, 8, HEAT_MAP_TYPE_BINGO, 1, 1, 16);
  assert_heat_map_values(hm, 7, 9, HEAT_MAP_TYPE_BINGO, 1, 1, 16);
  assert_heat_map_values(hm, 7, 10, HEAT_MAP_TYPE_BINGO, 1, 1, 16);
  assert_heat_map_values(hm, 7, 11, HEAT_MAP_TYPE_BINGO, 1, 1, 16);
  assert_heat_map_values(hm, 7, 12, HEAT_MAP_TYPE_BINGO, 1, 1, 16);
  assert_heat_map_values(hm, 7, 13, HEAT_MAP_TYPE_BINGO, 1, 1, 16);
  assert_heat_map_values(hm, 7, 6, HEAT_MAP_TYPE_BINGO, 0, 1, 16);

  assert_heat_map_add_move(game, "h8.PRETZEL", hm);
  assert_heat_map_values(hm, 7, 7, HEAT_MAP_TYPE_ALL, 3, 3, 30);
  assert_heat_map_values(hm, 8, 7, HEAT_MAP_TYPE_ALL, 1, 3, 30);
  assert_heat_map_values(hm, 9, 7, HEAT_MAP_TYPE_ALL, 1, 3, 30);
  assert_heat_map_values(hm, 10, 7, HEAT_MAP_TYPE_ALL, 1, 3, 30);
  assert_heat_map_values(hm, 11, 7, HEAT_MAP_TYPE_ALL, 1, 3, 30);
  assert_heat_map_values(hm, 12, 7, HEAT_MAP_TYPE_ALL, 1, 3, 30);
  assert_heat_map_values(hm, 13, 7, HEAT_MAP_TYPE_ALL, 1, 3, 30);
  assert_heat_map_values(hm, 6, 7, HEAT_MAP_TYPE_ALL, 0, 3, 30);
  assert_heat_map_values(hm, 7, 7, HEAT_MAP_TYPE_BINGO, 2, 2, 30);
  assert_heat_map_values(hm, 8, 7, HEAT_MAP_TYPE_BINGO, 1, 2, 30);
  assert_heat_map_values(hm, 9, 7, HEAT_MAP_TYPE_BINGO, 1, 2, 30);
  assert_heat_map_values(hm, 10, 7, HEAT_MAP_TYPE_BINGO, 1, 2, 30);
  assert_heat_map_values(hm, 11, 7, HEAT_MAP_TYPE_BINGO, 1, 2, 30);
  assert_heat_map_values(hm, 12, 7, HEAT_MAP_TYPE_BINGO, 1, 2, 30);
  assert_heat_map_values(hm, 13, 7, HEAT_MAP_TYPE_BINGO, 1, 2, 30);
  assert_heat_map_values(hm, 6, 7, HEAT_MAP_TYPE_BINGO, 0, 2, 30);

  assert_heat_map_add_move(game, "h7.FAV", hm);
  assert_heat_map_values(hm, 6, 7, HEAT_MAP_TYPE_ALL, 1, 4, 33);
  assert_heat_map_values(hm, 7, 7, HEAT_MAP_TYPE_ALL, 4, 4, 33);
  assert_heat_map_values(hm, 8, 7, HEAT_MAP_TYPE_ALL, 2, 4, 33);
  assert_heat_map_values(hm, 7, 7, HEAT_MAP_TYPE_BINGO, 2, 2, 33);
  assert_heat_map_values(hm, 8, 7, HEAT_MAP_TYPE_BINGO, 1, 2, 33);
  assert_heat_map_values(hm, 9, 7, HEAT_MAP_TYPE_BINGO, 1, 2, 33);
  assert_heat_map_values(hm, 10, 7, HEAT_MAP_TYPE_BINGO, 1, 2, 33);
  assert_heat_map_values(hm, 11, 7, HEAT_MAP_TYPE_BINGO, 1, 2, 33);
  assert_heat_map_values(hm, 12, 7, HEAT_MAP_TYPE_BINGO, 1, 2, 33);
  assert_heat_map_values(hm, 13, 7, HEAT_MAP_TYPE_BINGO, 1, 2, 33);
  assert_heat_map_values(hm, 6, 7, HEAT_MAP_TYPE_BINGO, 0, 2, 33);

  heat_map_reset(hm);
  assert(heat_map_get_total_count(hm) == 0);

  load_and_exec_config_or_die(
      config,
      "cgp 15/1AI9IA1/1E11E1/15/15/15/15/15/15/15/15/15/1E11E1/1AI9IA1/15 / "
      "0/0 0");

  assert_heat_map_single_move(game, "1A.RETINAS", hm);
  assert_heat_map_single_move(game, "1A.RETINA", hm);
  assert_heat_map_single_move(game, "A1.RETINAS", hm);
  assert_heat_map_single_move(game, "A1.RETINA", hm);
  assert_heat_map_single_move(game, "O1.FRAWZEY", hm);
  assert_heat_map_single_move(game, "O1.FROZEN", hm);
  assert_heat_map_single_move(game, "1H.FRAWZEY", hm);
  assert_heat_map_single_move(game, "1H.FROZEN", hm);
  assert_heat_map_single_move(game, "O8.BUSUUTI", hm);
  assert_heat_map_single_move(game, "O8.BUSTIS", hm);
  assert_heat_map_single_move(game, "15H.BUSUUTI", hm);
  assert_heat_map_single_move(game, "15H.BUSTIS", hm);
  assert_heat_map_single_move(game, "15A.SHORTEN", hm);
  assert_heat_map_single_move(game, "15A.SHORTS", hm);
  assert_heat_map_single_move(game, "A8.SHORTEN", hm);
  assert_heat_map_single_move(game, "A8.SHORTS", hm);

  load_and_exec_config_or_die(config, "newgame");
  load_and_exec_config_or_die(config, "r ABCDEFG");

  assert_config_exec_status(config, "heat 1", ERROR_STATUS_NO_HEAT_MAP_TO_SHOW);

  load_and_exec_config_or_die(
      config,
      "gsim -iter 10 -minp 1 -numplays 10 "
      "-useh true -plies 5 -boardcolor true -threads 12 -sr rr -thres none");

  assert_config_exec_status(config, "heat 0",
                            ERROR_STATUS_HEAT_MAP_MOVE_INDEX_OUT_OF_RANGE);
  assert_config_exec_status(config, "heat 11",
                            ERROR_STATUS_HEAT_MAP_MOVE_INDEX_OUT_OF_RANGE);
  assert_config_exec_status(config, "heat 1 a b",
                            ERROR_STATUS_EXTRANEOUS_HEAT_MAP_ARG);
  assert_config_exec_status(config, "heat 2 6",
                            ERROR_STATUS_HEAT_MAP_PLY_INDEX_OUT_OF_RANGE);
  assert_config_exec_status(config, "heat 2 0",
                            ERROR_STATUS_HEAT_MAP_PLY_INDEX_OUT_OF_RANGE);
  assert_config_exec_status(config, "heat 2 c",
                            ERROR_STATUS_HEAT_MAP_UNRECOGNIZED_TYPE);
  assert_config_exec_status(config, "heat 2 alls",
                            ERROR_STATUS_HEAT_MAP_UNRECOGNIZED_TYPE);

  show_heat_maps_for_cgp(config, EMPTY_CGP, "ABCDEFG");
  show_heat_maps_for_cgp(config, XU_UH_CGP, "BBCCFFG");

  load_and_exec_config_or_die(config, "gsim -iter 1000 -minp 10 -numplays 10 "
                                      "-useh false -plies 5 -boardcolor true");
  assert_config_exec_status(config, "heat 1", ERROR_STATUS_NO_HEAT_MAP_TO_SHOW);

  heat_map_destroy(hm);
  config_destroy(config);
}